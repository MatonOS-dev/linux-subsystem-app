/* Thin C++ shim: the platform stable-C mapper header uses typed enums and
 * cannot be included by C11. All policy and metadata validation stays in C. */
#include <android/dlext.h>
#include <android/log.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <string.h>

static AIMapper* mapper;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static int find_mapper(dl_phdr_info* info, size_t, void*) {
  const char* name = strrchr(info->dlpi_name, '/');
  name = name ? name + 1 : info->dlpi_name;
  if (strncmp(name, "mapper.", 7)) return 0;
  /* AHardwareBuffer has already selected/loaded the device mapper. Reuse
   * that library in its exported HAL namespace, never guess a vendor suffix
   * or load an unrelated mapper. Unavailable APIs fail closed. */
  using GetNamespace = android_namespace_t* (*)(const char*);
  void* nativewindow = dlopen("libnativewindow.so", RTLD_NOW);
  auto get_namespace = reinterpret_cast<GetNamespace>(nativewindow ?
      dlsym(nativewindow, "android_get_exported_namespace") : nullptr);
  android_dlextinfo ext = {};
  ext.flags = ANDROID_DLEXT_USE_NAMESPACE;
  ext.library_namespace = get_namespace ? get_namespace("sphal") : nullptr;
  void* lib = ext.library_namespace ? android_dlopen_ext(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD, &ext)
                                   : dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
  if (nativewindow) dlclose(nativewindow);
  using Load = AIMapper_Error (*)(AIMapper**);
  auto load = lib ? reinterpret_cast<Load>(dlsym(lib, "AIMapper_loadIMapper")) : nullptr;
  if (load && load(&mapper) == AIMAPPER_ERROR_NONE && mapper && mapper->version >= AIMAPPER_VERSION_5)
    return 1; /* Keep the library reference for the process lifetime. */
  mapper = nullptr;
  if (lib) dlclose(lib);
  return 0;
}
static void resolve_mapper() {
  dl_iterate_phdr(find_mapper, nullptr);
  if (!mapper) __android_log_print(ANDROID_LOG_WARN, "MatonCompositor",
      "Active stable-C mapper metadata API unavailable; direct dma-bufs disabled");
}

extern "C" int32_t maton_dmabuf_plane_metadata(const native_handle_t* handle, void* data, size_t size) {
  pthread_once(&once, resolve_mapper);
  if (!mapper || !handle) return -1;
  /* The AHB handle is already imported by this mapper in this process. */
  return mapper->v5.getStandardMetadata(handle, 15 /* PLANE_LAYOUTS */, data, size);
}
