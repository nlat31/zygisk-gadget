/* Copyright (c) 2025 ThePedroo. All rights reserved.
 *
 * This source code is licensed under the GNU AGPLv3 License found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <link.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unwind.h>

#include "elf_util.h"
#include "linker.h"
#include "logging.h"

/* INFO: System libunwind functions */
void __register_frame(void *eh_frame) __attribute__((weak));
void __deregister_frame(void *eh_frame) __attribute__((weak));

#define MAX_CUSTOM_LIBS (MAX_DEPS + 1)
#define CUSTOM_HANDLE_MAGIC UINT64_C(0x43534f4c444c4844)

struct custom_lib_info;

struct custom_lib_handle {
  uint64_t magic;
  unsigned int refs;
  bool valid;
  struct custom_lib_info *lib;
  struct custom_lib_handle *next;
};

struct custom_lib_info {
  struct csoloader_elf *img;
  struct dl_phdr_info phdr_info;
  bool in_use;
  bool unregistering;
  ElfW(Phdr) *phdr_copy;
  const void *owner;
  size_t scope_index;
  unsigned int active_refs;
  struct custom_lib_handle *handle;

  /* INFO: Broken! */
  void *eh_frame_registered;
  size_t eh_frame_size;
};

/* TODO: Transform into a dynamic structure to remove limitations */
static struct custom_lib_info g_custom_libs[MAX_CUSTOM_LIBS];
static pthread_mutex_t g_custom_libs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_custom_libs_cond = PTHREAD_COND_INITIALIZER;
static struct custom_lib_handle *g_custom_handles;
static ElfW(Addr) g_custom_dlpi_adds;
static ElfW(Addr) g_custom_dlpi_subs;

struct original_libdl_api {
  int (*dl_iterate_phdr)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
  int (*dladdr)(const void *, Dl_info *);
  void *(*dlopen)(const char *, int);
  void *(*dlsym)(void *, const char *);
  int (*dlclose)(void *);
  char *(*dlerror)(void);
};

static struct original_libdl_api g_original_libdl;
static pthread_once_t g_original_libdl_once = PTHREAD_ONCE_INIT;
static __thread bool g_custom_dlerror_pending;
static __thread char g_custom_dlerror_message[256];

static void resolve_original_libdl_api(void) {
  const char *candidates[] = {
    "libdl.so",
    #ifndef __ANDROID__
      "libdl.so.2",
      "libc.so.6",
    #endif
    NULL
  };
  bool found_image = false;
  for (size_t i = 0; candidates[i]; i++) {
    struct csoloader_elf *libdl_elf =
      csoloader_elf_create(candidates[i], NULL);
    if (!libdl_elf) continue;

    found_image = true;
    if (!g_original_libdl.dl_iterate_phdr)
      g_original_libdl.dl_iterate_phdr =
        (int (*)(int (*)(struct dl_phdr_info *, size_t, void *), void *))
          csoloader_elf_symb_address(libdl_elf, "dl_iterate_phdr");
    if (!g_original_libdl.dladdr)
      g_original_libdl.dladdr =
        (int (*)(const void *, Dl_info *))
          csoloader_elf_symb_address(libdl_elf, "dladdr");
    if (!g_original_libdl.dlopen)
      g_original_libdl.dlopen =
        (void *(*)(const char *, int))
          csoloader_elf_symb_address(libdl_elf, "dlopen");
    if (!g_original_libdl.dlsym)
      g_original_libdl.dlsym =
        (void *(*)(void *, const char *))
          csoloader_elf_symb_address(libdl_elf, "dlsym");
    if (!g_original_libdl.dlclose)
      g_original_libdl.dlclose =
        (int (*)(void *))
          csoloader_elf_symb_address(libdl_elf, "dlclose");
    if (!g_original_libdl.dlerror)
      g_original_libdl.dlerror =
        (char *(*)(void))
          csoloader_elf_symb_address(libdl_elf, "dlerror");

    csoloader_elf_destroy(libdl_elf);
  }

  if (!found_image) {
    LOGE("Failed to locate the system libdl implementation");
  }
}

static void set_custom_dlerror(const char *message) {
  snprintf(g_custom_dlerror_message, sizeof(g_custom_dlerror_message), "%s",
           message ? message : "unknown custom linker error");
  g_custom_dlerror_pending = true;
}

static const char *path_basename(const char *path) {
  if (!path) return "";

  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static const char *custom_image_soname(struct csoloader_elf *img) {
  if (!img || !img->header || !img->strtab_start) return NULL;

  ElfW(Phdr) *phdr =
    (ElfW(Phdr) *)((uintptr_t)img->header + img->header->e_phoff);
  for (int i = 0; i < img->header->e_phnum; i++) {
    if (phdr[i].p_type != PT_DYNAMIC) continue;

    ElfW(Dyn) *dyn =
      (ElfW(Dyn) *)((uintptr_t)img->base + phdr[i].p_vaddr - img->bias);
    for (ElfW(Dyn) *entry = dyn; entry && entry->d_tag != DT_NULL; entry++) {
      if (entry->d_tag == DT_SONAME)
        return (const char *)img->strtab_start + entry->d_un.d_val;
    }
  }

  return NULL;
}

static bool custom_image_name_matches(struct csoloader_elf *img,
                                      const char *filename) {
  if (!img || !img->elf || !filename) return false;
  if (strcmp(img->elf, filename) == 0) return true;

  const char *requested_base = path_basename(filename);
  if (strcmp(path_basename(img->elf), requested_base) == 0) return true;

  const char *soname = custom_image_soname(img);
  return soname && strcmp(soname, requested_base) == 0;
}

static void *find_custom_symbol(struct csoloader_elf *img,
                                const char *symbol) {
  if (!img || !symbol) return NULL;

  void *result = (void *)csoloader_elf_symb_address_exported(img, symbol);
  if (!result) result = (void *)csoloader_elf_symb_address(img, symbol);

  return result;
}

struct pinned_custom_image {
  struct custom_lib_info *lib;
  struct csoloader_elf *img;
};

static struct custom_lib_handle *find_custom_handle_locked(void *handle) {
  for (struct custom_lib_handle *current = g_custom_handles;
       current;
       current = current->next) {
    if ((void *)current == handle) return current;
  }

  return NULL;
}

static struct custom_lib_info *find_custom_caller_locked(const void *address) {
  uintptr_t target = (uintptr_t)address;

  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    struct custom_lib_info *lib = &g_custom_libs[i];
    if (!lib->in_use || lib->unregistering) continue;

    for (size_t j = 0; j < lib->phdr_info.dlpi_phnum; j++) {
      const ElfW(Phdr) *phdr = &lib->phdr_info.dlpi_phdr[j];
      if (phdr->p_type != PT_LOAD) continue;

      uintptr_t start = lib->phdr_info.dlpi_addr + phdr->p_vaddr;
      uintptr_t end = start + phdr->p_memsz;
      if (target >= start && target < end) return lib;
    }
  }

  return NULL;
}

static void unpin_custom_images(struct pinned_custom_image *pinned,
                                size_t count) {
  pthread_mutex_lock(&g_custom_libs_mutex);
  for (size_t i = 0; i < count; i++) {
    if (pinned[i].lib->active_refs > 0)
      pinned[i].lib->active_refs--;
  }
  pthread_cond_broadcast(&g_custom_libs_cond);
  pthread_mutex_unlock(&g_custom_libs_mutex);
}

static void capture_system_dlerror(void) {
  if (!g_original_libdl.dlerror) {
    set_custom_dlerror("system libdl operation failed");

    return;
  }

  const char *message = g_original_libdl.dlerror();
  set_custom_dlerror(message ? message : "system libdl operation failed");
}

/* INFO: Minimal readers for decoding .eh_frame_hdr when section headers are absent */
static uint64_t read_uleb128(const uint8_t **p, const uint8_t *end) {
  const uint8_t *s = *p;
  uint64_t r = 0;
  unsigned shift = 0;

  while (s < end) {
    uint8_t b = *s++;
    r |= ((uint64_t)(b & 0x7f)) << shift;
  
    if ((b & 0x80) == 0) break;
  
    shift += 7;
  
    if (shift >= 64) break;
  }

  *p = s;

  return r;
}

/* DW_EH_PE encodings used in .eh_frame_hdr */
#define DW_EH_PE_omit     0xff
#define DW_EH_PE_ptr      0x00
#define DW_EH_PE_uleb128  0x01
#define DW_EH_PE_udata2   0x02
#define DW_EH_PE_udata4   0x03
#define DW_EH_PE_udata8   0x04
#define DW_EH_PE_sdata2   0x0a
#define DW_EH_PE_sdata4   0x0b
#define DW_EH_PE_sdata8   0x0c

#define DW_EH_PE_absptr   0x00
#define DW_EH_PE_pcrel    0x10
#define DW_EH_PE_datarel  0x30
#define DW_EH_PE_indirect 0x80

static int read_u16(const uint8_t **p, const uint8_t *end, uint16_t *out) {
  if ((size_t)(end - *p) < sizeof(uint16_t)) {
    return -1;
  }

  uint16_t v = 0;
  memcpy(&v, *p, sizeof(uint16_t));

  *p += sizeof(uint16_t);
  *out = v;

  return 0;
}

static int read_u32(const uint8_t **p, const uint8_t *end, uint32_t *out) {
  if ((size_t)(end - *p) < sizeof(uint32_t)) {
    return -1;
  }

  uint32_t v = 0;
  memcpy(&v, *p, sizeof(uint32_t));

  *p += sizeof(uint32_t);
  *out = v;

  return 0;
}

static int read_u64(const uint8_t **p, const uint8_t *end, uint64_t *out) {
  if ((size_t)(end - *p) < sizeof(uint64_t)) {
    return -1;
  }

  uint64_t v = 0;
  memcpy(&v, *p, sizeof(uint64_t));

  *p += sizeof(uint64_t);
  *out = v;

  return 0;
}

static uintptr_t decode_eh_value(uint8_t enc, const uint8_t **p, uintptr_t base, uintptr_t data_base, const uint8_t *end) {
  if (enc == DW_EH_PE_omit) return 0;

  uint8_t fmt = enc & 0x0f;
  uint8_t app = enc & 0x70;
  int indir = (enc & DW_EH_PE_indirect) != 0;

  uintptr_t value = 0;
  switch (fmt) {
    case DW_EH_PE_ptr: {
      #ifdef __LP64__
        if (read_u64(p, end, (uintptr_t *)&value) != 0) return 0;
      #else
        if (read_u32(p, end, (uintptr_t *)&value) != 0) return 0;
      #endif

      break;
    }
    case DW_EH_PE_uleb128: {
      value = (uintptr_t)read_uleb128(p, end);

      break;
    }
    case DW_EH_PE_udata2: {
      if (read_u16(p, end, (uint16_t *)&value) != 0) return 0;

      break;
    }
    case DW_EH_PE_udata4: {
      if (read_u32(p, end, (uint32_t *)&value) != 0) return 0;

      break;
    }
    case DW_EH_PE_udata8: {
      if (read_u64(p, end, (uint64_t *)&value) != 0) return 0;

      break;
    }
    case DW_EH_PE_sdata2: {
      uint16_t raw = 0;
      if (read_u16(p, end, &raw) != 0) return 0;

      value = (uintptr_t)(intptr_t)(int16_t)raw;

      break;
    }
    case DW_EH_PE_sdata4: {
      uint32_t raw = 0;
      if (read_u32(p, end, &raw) != 0) return 0;

      value = (uintptr_t)(intptr_t)(int32_t)raw;

      break;
    }
    case DW_EH_PE_sdata8: {
      uint64_t raw = 0;
      if (read_u64(p, end, &raw) != 0) return 0;

      value = (uintptr_t)(intptr_t)(int64_t)raw;

      break;
    }
    default: return 0;
  }

  switch (app) {
    case DW_EH_PE_absptr: break;
    case DW_EH_PE_pcrel: value += base; break;
    case DW_EH_PE_datarel: value += data_base; break;
    default: break;
  }

  if (indir) {
    if (!value) {
      LOGE("Failed to decode indirect .eh_frame_hdr value: pointer is NULL");

      return 0;
    }

    value = *(const uintptr_t *)(const void *)value;
  }

  return value;
}

static int locate_eh_frame_ptr(struct csoloader_elf *img, void **out_ptr, size_t *out_size) {
  *out_ptr = NULL;
  if (out_size) *out_size = 0;

  /* INFO: Try to find .eh_frame (direct) first */
  if (img->section_header && img->header->e_shstrndx != SHN_UNDEF && img->header->e_shstrndx < img->header->e_shnum) {
    ElfW(Shdr) *shstr = img->section_header + img->header->e_shstrndx;
    char *names = (char *)((uintptr_t)img->header + shstr->sh_offset);
    if (names) {
      for (int i = 0; i < img->header->e_shnum; i++) {
        ElfW(Shdr) *sh = img->section_header + i;
        const char *sname = names + sh->sh_name;
        if (!sname || strcmp(sname, ".eh_frame") != 0) continue;

        *out_ptr = (void *)((uintptr_t)img->base + sh->sh_addr - img->bias);
        if (out_size) *out_size = sh->sh_size;
  
        return 0;
      }
    }
  }

  /* INFO: Fallback to PT_GNU_EH_FRAME, then decode hdr to get eh_frame_ptr */
  if (img->header->e_phoff > 0 && img->header->e_phnum > 0) {
    ElfW(Phdr) *phdr = (ElfW(Phdr) *)((uintptr_t)img->header + img->header->e_phoff);
    for (int i = 0; i < img->header->e_phnum; i++) {
      if (phdr[i].p_type != PT_GNU_EH_FRAME) continue;

      const uint8_t *hdr = (const uint8_t *)((uintptr_t)img->base + phdr[i].p_vaddr - img->bias);
      size_t hdr_sz = phdr[i].p_memsz;

      if (!hdr || hdr_sz < 4) {
        LOGW("PT_GNU_EH_FRAME too small for %s", img->elf);

        continue;
      }

      const uint8_t *p = hdr;
      const uint8_t *end = hdr + hdr_sz;

      uint8_t version = *p++;
      uint8_t eh_frame_ptr_enc = *p++;
      uint8_t fde_count_enc = *p++;
      uint8_t table_enc = *p++;

      (void)table_enc;
      (void)fde_count_enc;

      if (version != 1) {
        LOGW(".eh_frame_hdr version %u not supported (image %s)", version, img->elf);

        continue;
      }

      /* INFO: eh_frame_ptr follows next. For pcrel, base is address of the encoded field */
      uintptr_t eh_frame_ptr = decode_eh_value(eh_frame_ptr_enc, &p, (uintptr_t)p, (uintptr_t)hdr, end);
      if (!eh_frame_ptr) {
        LOGW("Failed to decode eh_frame_ptr in %s", img->elf);

        continue;
      }

      *out_ptr = (void *)eh_frame_ptr;
      if (out_size) *out_size = 0;

      LOGD("Located .eh_frame via PT_GNU_EH_FRAME in %s at %p", img->elf, *out_ptr);

      return 0;
    }
  }

  LOGW("Failed to locate .eh_frame for %s", img->elf);

  return -1;
}

struct custom_phdr_snapshot {
  struct dl_phdr_info info;
  ElfW(Phdr) *phdr;
  char *name;
  struct custom_lib_info *lib;
};

int custom_dl_iterate_phdr(
    int (*callback)(struct dl_phdr_info *, size_t, void *), void *data) {
  if (!callback) return -1;

  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);
  if (!g_original_libdl.dl_iterate_phdr) {
    LOGE("Failed to locate original dl_iterate_phdr in libdl.so");

    return -1;
  }

  int result = g_original_libdl.dl_iterate_phdr(callback, data);
  if (result != 0) return result;

  struct custom_phdr_snapshot snapshots[MAX_CUSTOM_LIBS] = { 0 };
  size_t snapshot_count = 0;

  pthread_mutex_lock(&g_custom_libs_mutex);
  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    if (!g_custom_libs[i].in_use || g_custom_libs[i].unregistering)
      continue;

    struct custom_phdr_snapshot *snapshot = &snapshots[snapshot_count];
    snapshot->info = g_custom_libs[i].phdr_info;
    snapshot->info.dlpi_adds = g_custom_dlpi_adds;
    snapshot->info.dlpi_subs = g_custom_dlpi_subs;
    snapshot->lib = &g_custom_libs[i];
    snapshot->lib->active_refs++;

    size_t phdr_size =
      snapshot->info.dlpi_phnum * sizeof(ElfW(Phdr));
    snapshot->phdr = (ElfW(Phdr) *)malloc(phdr_size);
    snapshot->name = strdup(snapshot->info.dlpi_name
                              ? snapshot->info.dlpi_name
                              : "");
    if (!snapshot->phdr || !snapshot->name) {
      free(snapshot->phdr);
      free(snapshot->name);
      snapshot->lib->active_refs--;
      for (size_t j = 0; j < snapshot_count; j++)
        snapshots[j].lib->active_refs--;
      pthread_cond_broadcast(&g_custom_libs_cond);
      pthread_mutex_unlock(&g_custom_libs_mutex);

      for (size_t j = 0; j < snapshot_count; j++) {
        free(snapshots[j].phdr);
        free(snapshots[j].name);
      }

      LOGE("Failed to snapshot custom program headers");

      return -1;
    }

    memcpy(snapshot->phdr, snapshot->info.dlpi_phdr, phdr_size);
    snapshot->info.dlpi_phdr = snapshot->phdr;
    snapshot->info.dlpi_name = snapshot->name;
    snapshot_count++;
  }
  pthread_mutex_unlock(&g_custom_libs_mutex);

  LOGD("Enumerating %zu custom ELF images after system images",
       snapshot_count);

  for (size_t i = 0; i < snapshot_count; i++) {
    result = callback(&snapshots[i].info, sizeof(struct dl_phdr_info), data);
    if (result != 0) break;
  }

  for (size_t i = 0; i < snapshot_count; i++) {
    free(snapshots[i].phdr);
    free(snapshots[i].name);
  }

  pthread_mutex_lock(&g_custom_libs_mutex);
  for (size_t i = 0; i < snapshot_count; i++)
    snapshots[i].lib->active_refs--;
  pthread_cond_broadcast(&g_custom_libs_cond);
  pthread_mutex_unlock(&g_custom_libs_mutex);

  return result;
}

/* INFO: libc functions such as backtrace rely on the below functions. Since we
           "cannot" tamper the PLT of libc to redirect for our functions, we must
           declare them here, so that the system linker understands where we want
           it to call. */
/* INFO: Disable it, as it will cause infinite recursion. If one day we use it as
           main linker, it won't even need to search in the system linker anyway. */
// int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data) {
//   return custom_dl_iterate_phdr(callback, data);
// }

int custom_dladdr(const void *addr, Dl_info *info) {
  if (!info) return 0;

  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);
  if (!g_original_libdl.dladdr) {
    LOGE("Failed to locate original dladdr in libdl.so");

    return 0;
  }

  if (g_original_libdl.dladdr(addr, info))
    return 1;

  pthread_mutex_lock(&g_custom_libs_mutex);
  struct custom_lib_info *lib = find_custom_caller_locked(addr);
  if (lib) lib->active_refs++;
  pthread_mutex_unlock(&g_custom_libs_mutex);
  if (!lib) return 0;

  info->dli_fname = lib->phdr_info.dlpi_name;
  info->dli_fbase = (void *)lib->phdr_info.dlpi_addr;

  struct sym_info sym =
    csoloader_elf_get_symbol(lib->img, (uintptr_t)addr);
  info->dli_sname = sym.name;
  info->dli_saddr = sym.name ? (void *)sym.address : NULL;

  struct pinned_custom_image pinned = { lib, lib->img };
  unpin_custom_images(&pinned, 1);

  return 1;
}

__attribute__((noinline))
void *custom_dlopen(const char *filename, int flags) {
  void *caller = __builtin_return_address(0);
  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);

  if (filename) {
    pthread_mutex_lock(&g_custom_libs_mutex);
    struct custom_lib_info *caller_lib =
      find_custom_caller_locked(caller);
    for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
      struct custom_lib_info *lib = &g_custom_libs[i];
      if (!lib->in_use || lib->unregistering
          || !caller_lib
          || lib->owner != caller_lib->owner
          || !custom_image_name_matches(lib->img, filename))
        continue;

      lib->handle->refs++;
      void *handle = lib->handle;
      pthread_mutex_unlock(&g_custom_libs_mutex);

      return handle;
    }
    pthread_mutex_unlock(&g_custom_libs_mutex);
  }

  if (!g_original_libdl.dlopen) {
    set_custom_dlerror("system dlopen is unavailable");

    return NULL;
  }

  void *result = g_original_libdl.dlopen(filename, flags);
  if (!result) capture_system_dlerror();

  return result;
}

__attribute__((noinline))
void *custom_dlsym(void *handle, const char *symbol) {
  void *caller = __builtin_return_address(0);

  if (!symbol) {
    set_custom_dlerror("dlsym called with a null symbol name");

    return NULL;
  }

  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);

  struct pinned_custom_image pinned[MAX_CUSTOM_LIBS] = { 0 };
  size_t pinned_count = 0;
  bool is_custom_handle = false;
  bool is_custom_next = false;
  struct linker *scope_owner = NULL;
  struct csoloader_elf *scope_requester = NULL;

  pthread_mutex_lock(&g_custom_libs_mutex);
  struct custom_lib_handle *custom_handle =
    find_custom_handle_locked(handle);
  struct custom_lib_info *caller_lib =
    find_custom_caller_locked(caller);

  if (custom_handle) {
    is_custom_handle = true;
    if (custom_handle->magic != CUSTOM_HANDLE_MAGIC
        || !custom_handle->valid
        || !custom_handle->lib
        || custom_handle->refs == 0) {
      pthread_mutex_unlock(&g_custom_libs_mutex);
      set_custom_dlerror("dlsym called with a closed custom handle");

      return NULL;
    }

    struct custom_lib_info *target = custom_handle->lib;
    target->active_refs++;
    pinned[pinned_count++] =
      (struct pinned_custom_image){ target, target->img };
    scope_owner = (struct linker *)target->owner;
    scope_requester = target->img;
  } else if (handle == RTLD_DEFAULT || handle == RTLD_NEXT) {
    if (handle == RTLD_NEXT && !caller_lib) {
      pthread_mutex_unlock(&g_custom_libs_mutex);
      set_custom_dlerror("RTLD_NEXT caller is outside the custom linker");

      return NULL;
    }

    if (caller_lib) {
      caller_lib->active_refs++;
      pinned[pinned_count++] =
        (struct pinned_custom_image){ caller_lib, caller_lib->img };
      scope_owner = (struct linker *)caller_lib->owner;
      scope_requester = caller_lib->img;
      is_custom_next = handle == RTLD_NEXT;
    }
  }
  pthread_mutex_unlock(&g_custom_libs_mutex);

  void *result = NULL;
  if (scope_owner)
    result = is_custom_next
      ? linker_dlsym_next(scope_owner, scope_requester, symbol)
      : is_custom_handle
        ? linker_dlsym_handle(scope_owner, scope_requester, symbol)
        : linker_dlsym_default(scope_owner, symbol);
  unpin_custom_images(pinned, pinned_count);

  if (result) return result;
  if (is_custom_handle) {
    set_custom_dlerror("symbol not found in custom linker scope");

    return NULL;
  }
  if (is_custom_next) {
    set_custom_dlerror("symbol not found after the custom caller");

    return NULL;
  }

  if (!g_original_libdl.dlsym) {
    set_custom_dlerror("system dlsym is unavailable");

    return NULL;
  }

  if (g_original_libdl.dlerror) (void)g_original_libdl.dlerror();
  result = g_original_libdl.dlsym(handle, symbol);
  if (g_original_libdl.dlerror) {
    const char *error = g_original_libdl.dlerror();
    if (error) set_custom_dlerror(error);
  }

  return result;
}

int custom_dlclose(void *handle) {
  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);

  pthread_mutex_lock(&g_custom_libs_mutex);
  struct custom_lib_handle *custom_handle =
    find_custom_handle_locked(handle);
  if (custom_handle) {
    if (custom_handle->magic != CUSTOM_HANDLE_MAGIC
        || !custom_handle->valid
        || custom_handle->refs == 0) {
      pthread_mutex_unlock(&g_custom_libs_mutex);
      set_custom_dlerror("dlclose called with a closed custom handle");

      return -1;
    }

    custom_handle->refs--;
    pthread_mutex_unlock(&g_custom_libs_mutex);

    return 0;
  }
  pthread_mutex_unlock(&g_custom_libs_mutex);

  if (!g_original_libdl.dlclose) {
    set_custom_dlerror("system dlclose is unavailable");

    return -1;
  }

  int result = g_original_libdl.dlclose(handle);
  if (result != 0) capture_system_dlerror();

  return result;
}

char *custom_dlerror(void) {
  if (g_custom_dlerror_pending) {
    g_custom_dlerror_pending = false;

    return g_custom_dlerror_message;
  }

  pthread_once(&g_original_libdl_once, resolve_original_libdl_api);
  return g_original_libdl.dlerror ? g_original_libdl.dlerror() : NULL;
}

/* INFO: libc functions such as backtrace rely on the below functions. Since we
           "cannot" tamper the PLT of libc to redirect for our functions, we must
           declare them here, so that the system linker understands where we want
           it to call. */
/* INFO: Disable it, as it will cause infinite recursion. If one day we use it as
           main linker, it won't even need to search in the system linker anyway. */
// int dladdr(const void *addr, Dl_info *info) {
//   return custom_dladdr(addr, info);
// }

static ElfW(Phdr) *copy_program_headers(struct csoloader_elf *img) {
  size_t phdr_size = img->header->e_phnum * sizeof(ElfW(Phdr));
  ElfW(Phdr) *phdr_copy = (ElfW(Phdr) *)malloc(phdr_size);
  if (!phdr_copy) {
    LOGE("Failed to allocate memory for program header copy");

    return NULL;
  }

  ElfW(Phdr) *original_phdr = (ElfW(Phdr) *)((uintptr_t)img->header + img->header->e_phoff);
  memcpy(phdr_copy, original_phdr, phdr_size);

  return phdr_copy;
}

bool register_custom_library_for_backtrace(struct csoloader_elf *img,
                                           const void *owner,
                                           size_t scope_index) {
  struct custom_lib_handle *handle =
    (struct custom_lib_handle *)calloc(1, sizeof(*handle));
  if (!handle) {
    LOGE("Failed to allocate a custom library handle");

    return false;
  }

  pthread_mutex_lock(&g_custom_libs_mutex);

  int slot = -1;
  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    if (g_custom_libs[i].in_use) continue;
    
    slot = i;

    break;
  }

  if (slot == -1) {
    pthread_mutex_unlock(&g_custom_libs_mutex);
    free(handle);

    LOGE("No available slots for custom library registration");

    return false;
  }

  struct custom_lib_info *lib_info = &g_custom_libs[slot];
  lib_info->phdr_copy = copy_program_headers(img);
  if (!lib_info->phdr_copy) {
    pthread_mutex_unlock(&g_custom_libs_mutex);
    free(handle);

    LOGE("Failed to copy program headers for custom library %s", img->elf);

    return false;
  }

  lib_info->phdr_info.dlpi_addr = (ElfW(Addr))img->base - img->bias;
  lib_info->phdr_info.dlpi_name = img->elf;
  lib_info->phdr_info.dlpi_phdr = lib_info->phdr_copy;
  lib_info->phdr_info.dlpi_phnum = img->header->e_phnum;
  lib_info->phdr_info.dlpi_adds = ++g_custom_dlpi_adds;
  lib_info->phdr_info.dlpi_subs = g_custom_dlpi_subs;

  if (img->tls_segment) {
    lib_info->phdr_info.dlpi_tls_modid = img->tls_mod_id;
    lib_info->phdr_info.dlpi_tls_data = NULL;
  } else {
    lib_info->phdr_info.dlpi_tls_modid = 0;
    lib_info->phdr_info.dlpi_tls_data = NULL;
  }

  lib_info->img = img;
  lib_info->in_use = true;
  lib_info->unregistering = false;
  lib_info->owner = owner;
  lib_info->scope_index = scope_index;
  lib_info->active_refs = 0;
  lib_info->handle = handle;
  lib_info->eh_frame_registered = NULL;
  lib_info->eh_frame_size = 0;

  handle->magic = CUSTOM_HANDLE_MAGIC;
  handle->valid = true;
  handle->lib = lib_info;
  handle->next = g_custom_handles;
  g_custom_handles = handle;

  pthread_mutex_unlock(&g_custom_libs_mutex);

  LOGD("Registered custom library %s at %p in scope %zu",
       img->elf, img->base, scope_index);

  return true;
}

bool unregister_custom_library_for_backtrace(struct csoloader_elf *img) {
  pthread_mutex_lock(&g_custom_libs_mutex);

  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    if (!g_custom_libs[i].in_use || g_custom_libs[i].img != img) continue;

    g_custom_libs[i].unregistering = true;
    while (g_custom_libs[i].active_refs > 0)
      pthread_cond_wait(&g_custom_libs_cond, &g_custom_libs_mutex);

    if (g_custom_libs[i].eh_frame_registered) {
      __deregister_frame(g_custom_libs[i].eh_frame_registered);

      LOGD("Deregistered .eh_frame for %s", img->elf);

      g_custom_libs[i].eh_frame_registered = NULL;
    }

    if (g_custom_libs[i].handle) {
      g_custom_libs[i].handle->valid = false;
      g_custom_libs[i].handle->lib = NULL;
    }

    g_custom_dlpi_subs++;
    if (g_custom_libs[i].phdr_copy) free(g_custom_libs[i].phdr_copy);
    memset(&g_custom_libs[i], 0, sizeof(struct custom_lib_info));

    pthread_mutex_unlock(&g_custom_libs_mutex);

    LOGD("Unregistered custom library for backtrace support");

    return true;
  }

  pthread_mutex_unlock(&g_custom_libs_mutex);

  return false;
}

bool custom_libraries_prepare_unload(const void *owner) {
  pthread_mutex_lock(&g_custom_libs_mutex);

  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    struct custom_lib_info *lib = &g_custom_libs[i];
    if (!lib->in_use || lib->owner != owner) continue;

    if (lib->unregistering
        || lib->active_refs != 0
        || (lib->handle && lib->handle->refs != 0)) {
      pthread_mutex_unlock(&g_custom_libs_mutex);

      return false;
    }
  }

  /* INFO: Mark the complete custom-linker scope while still holding the
           registry lock. New local dlopen/dlsym calls can no longer acquire
           references between the eligibility check and unmapping. */
  for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
    struct custom_lib_info *lib = &g_custom_libs[i];
    if (lib->in_use && lib->owner == owner)
      lib->unregistering = true;
  }

  pthread_mutex_unlock(&g_custom_libs_mutex);

  return true;
}

void register_eh_frame_for_library(struct csoloader_elf *img) {
  #ifdef __arm__
    (void)img; (void)locate_eh_frame_ptr;
  
    LOGD("Skipping .eh_frame registration on ARM32 (EHABI)");

    return;
  #else
    void *eh_frame_ptr = NULL;
    size_t eh_frame_size = 0;
    if (locate_eh_frame_ptr(img, &eh_frame_ptr, &eh_frame_size) != 0 || !eh_frame_ptr) {
      LOGW("No .eh_frame found for %s; exceptions may fail", img->elf);

      return;
    }

    LOGD("Registering .eh_frame at %p (size ~%zu) for %s", eh_frame_ptr, eh_frame_size, img->elf);

    if (__register_frame) {
      __register_frame(eh_frame_ptr);
      LOGD("Registered .eh_frame at %p (size ~%zu) for %s", eh_frame_ptr, eh_frame_size, img->elf);
    } else {
      LOGW("__register_frame not available; skipping .eh_frame registration for %s", img->elf);
      return;
    }

    /* INFO: Store for deregistration */
    pthread_mutex_lock(&g_custom_libs_mutex);
    for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
      if (!g_custom_libs[i].in_use || g_custom_libs[i].img != img) continue;

      g_custom_libs[i].eh_frame_registered = eh_frame_ptr;
      g_custom_libs[i].eh_frame_size = eh_frame_size;

      break;
    }

    pthread_mutex_unlock(&g_custom_libs_mutex);
  #endif
}

void unregister_eh_frame_for_library(struct csoloader_elf *img) {
  #ifdef __arm__
    (void)img;
  #else
    /* INFO: Deregister using stored pointer if available */
    pthread_mutex_lock(&g_custom_libs_mutex);

    for (int i = 0; i < MAX_CUSTOM_LIBS; i++) {
      if (!g_custom_libs[i].in_use || g_custom_libs[i].img != img || !g_custom_libs[i].eh_frame_registered) continue;

      __deregister_frame(g_custom_libs[i].eh_frame_registered);
      g_custom_libs[i].eh_frame_registered = NULL;

      LOGD("Deregistered .eh_frame for %s", img->elf);

      break;
    }

    pthread_mutex_unlock(&g_custom_libs_mutex);
  #endif
}