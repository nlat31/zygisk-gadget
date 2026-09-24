#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "csoloader.h"

#ifdef __ANDROID__
#define SYSTEM_MATH_LIBRARY "libm.so"
#else
#define SYSTEM_MATH_LIBRARY "libm.so.6"
#endif


int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s /path/to/libfixture.so\n", argv[0]);

    return 64;
  }

  void *libm = dlopen(SYSTEM_MATH_LIBRARY, RTLD_NOW | RTLD_GLOBAL);
  if (!libm) {
    fprintf(stderr, "failed to preload libm: %s\n", dlerror());

    return 67;
  }

  const char *load_only_seconds = getenv("CSOLOADER_LOAD_ONLY_SLEEP");
  if (!load_only_seconds) {
    struct csoloader rejected = { 0 };
    if (csoloader_load_with_mapped_range(&rejected, argv[1], NULL)) {
      fprintf(stderr, "generic fixture unexpectedly accepted as Gadget\n");
      csoloader_unload(&rejected);
      dlclose(libm);

      return 68;
    }
  }

  struct csoloader loader = { 0 };
  const char *gadget_config =
      "{\"interaction\":{\"type\":\"listen\",\"address\":\"127.0.0.1\","
      "\"port\":27042,\"on_load\":\"resume\"}}";
  bool loaded = load_only_seconds
      ? csoloader_load_with_mapped_range(&loader, argv[1], gadget_config)
      : csoloader_load(&loader, argv[1]);
  if (!loaded) {
    fprintf(stderr, "failed to load fixture\n");
    dlclose(libm);

    return 65;
  }

  if (load_only_seconds) {
    printf("READY %d\n", getpid());
    fflush(stdout);
    sleep((unsigned int)strtoul(load_only_seconds, NULL, 10));
    dlclose(libm);

    return 0;
  }

  int (*run)(const char *) =
    (int (*)(const char *))csoloader_get_symbol(&loader, "libdl_compat_run");
  int (*open_lifetime_handle)(const char *) =
    (int (*)(const char *))csoloader_get_symbol(
      &loader, "libdl_compat_open_lifetime_handle");
  int (*close_lifetime_handle)(void) =
    (int (*)(void))csoloader_get_symbol(
      &loader, "libdl_compat_close_lifetime_handle");
  if (!run || !open_lifetime_handle || !close_lifetime_handle) {
    fprintf(stderr, "fixture entry points not found\n");
    csoloader_unload(&loader);

    return 66;
  }

  int result = run(argv[1]);
  const char *sleep_seconds = getenv("CSOLOADER_TEST_SLEEP");
  if (result == 0 && sleep_seconds) {
    printf("READY %d\n", getpid());
    fflush(stdout);
    sleep((unsigned int)strtoul(sleep_seconds, NULL, 10));
  }
  if (result == 0 && open_lifetime_handle(argv[1]) != 0)
    result = 19;
  if (result == 0 && csoloader_unload(&loader))
    result = 20;
  if (result == 0 && close_lifetime_handle() != 0)
    result = 21;
  if (!csoloader_unload(&loader) && result == 0)
    result = 22;
  dlclose(libm);
  if (result != 0)
    fprintf(stderr, "libdl compatibility check failed: %d\n", result);

  return result;
}
