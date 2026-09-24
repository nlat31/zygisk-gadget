#define _GNU_SOURCE

#include <dlfcn.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>


static const char *g_expected_name;
static int g_reentrant_dladdr_ok;
static void *g_lifetime_handle;

#ifdef __ANDROID__
#define SYSTEM_MATH_LIBRARY "libm.so"
#else
#define SYSTEM_MATH_LIBRARY "libm.so.6"
#endif

int libdl_compat_marker(void) {
  return 42;
}

double libdl_compat_cos(double value) {
  return cos(value);
}

int libdl_compat_open_lifetime_handle(const char *path) {
  g_lifetime_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  return g_lifetime_handle ? 0 : -1;
}

int libdl_compat_close_lifetime_handle(void) {
  if (!g_lifetime_handle) return -1;

  int result = dlclose(g_lifetime_handle);
  g_lifetime_handle = NULL;

  return result;
}

static const char *base_name(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : path;
}

static int find_self(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  (void)data;

  if (!info->dlpi_name
      || strcmp(base_name(info->dlpi_name), g_expected_name) != 0)
    return 0;

  Dl_info address_info = { 0 };
  g_reentrant_dladdr_ok =
    dladdr((const void *)&libdl_compat_marker, &address_info) != 0
    && address_info.dli_fname != NULL;

  return 1;
}

static void *run_concurrent_lookups(void *data) {
  (void)data;

  for (int i = 0; i < 1000; i++) {
    Dl_info info = { 0 };
    if (!dladdr((const void *)&libdl_compat_marker, &info))
      return (void *)1;

    int (*marker)(void) =
      (int (*)(void))dlsym(RTLD_DEFAULT, "libdl_compat_marker");
    if (!marker || marker() != 42)
      return (void *)1;
  }

  return NULL;
}

int libdl_compat_run(const char *path) {
  Dl_info address_info = { 0 };
  if (!dladdr((const void *)&libdl_compat_marker, &address_info))
    return 1;
  if (strcmp(base_name(address_info.dli_fname), base_name(path)) != 0)
    return 2;

  g_expected_name = base_name(path);
  if (dl_iterate_phdr(find_self, NULL) != 1)
    return 3;
  if (!g_reentrant_dladdr_ok)
    return 4;

  void *self = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!self)
    return 5;

  int (*marker)(void) = (int (*)(void))dlsym(self, "libdl_compat_marker");
  if (!marker || marker() != 42)
    return 6;
  marker = (int (*)(void))dlsym(RTLD_DEFAULT, "libdl_compat_marker");
  if (!marker || marker() != 42)
    return 7;

  void *soname =
    dlopen("libcsoloader-compat-soname.so", RTLD_NOW | RTLD_LOCAL);
  if (!soname)
    return 8;
  if (dlclose(soname) != 0)
    return 9;

  (void)dlerror();
  if (dlsym(self, "libdl_compat_missing_symbol") != NULL)
    return 10;
  if (!dlerror())
    return 11;

  if (dlclose(self) != 0)
    return 12;
  if (dlsym(self, "libdl_compat_marker") != NULL || !dlerror())
    return 13;
  if (dlclose(self) == 0 || !dlerror())
    return 14;

  void *system = dlopen(SYSTEM_MATH_LIBRARY, RTLD_NOW | RTLD_GLOBAL);
  if (!system)
    return 15;
  if (!dlsym(system, "cos"))
    return 16;
  if (!dlsym(RTLD_NEXT, "cos"))
    return 17;
  if (dlclose(system) != 0)
    return 18;

  pthread_t threads[4];
  for (size_t i = 0; i < 4; i++) {
    if (pthread_create(&threads[i], NULL, run_concurrent_lookups, NULL) != 0)
      return 19;
  }
  for (size_t i = 0; i < 4; i++) {
    void *thread_result = NULL;
    if (pthread_join(threads[i], &thread_result) != 0 || thread_result)
      return 20;
  }

  return 0;
}
