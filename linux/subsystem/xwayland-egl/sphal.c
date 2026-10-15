#include "sphal.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stdlib.h>

typedef void* (*LoadSphal)(const char*, int);

void* maton_sphal_load(const char* library) {
  static LoadSphal load;
  if (!load) {
    void* vndk = dlopen("libvndksupport.so", RTLD_NOW);
    load = vndk ? (LoadSphal)dlsym(vndk, "android_load_sphal_library") : NULL;
  }
  void* handle = load ? load(library, RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!handle)
    __android_log_print(ANDROID_LOG_ERROR, "MatonXwaylandEGL", "cannot load %s: %s", library, dlerror());
  return handle;
}

static void missing(void) {
  __android_log_print(ANDROID_LOG_FATAL, "MatonXwaylandEGL",
                      "called a function its vendor library lacks (see the earlier warnings)");
  abort();
}

void maton_sphal_resolve(const char* library, const char* const* names, void** table, size_t count) {
  void* handle = maton_sphal_load(library);
  for (size_t i = 0; i < count; ++i) {
    table[i] = handle ? dlsym(handle, names[i]) : NULL;
    if (!table[i]) {
      if (handle)
        __android_log_print(ANDROID_LOG_WARN, "MatonXwaylandEGL", "%s lacks %s", library, names[i]);
      table[i] = (void*)missing;
    }
  }
}
