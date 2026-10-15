/* Probe for the Xwayland EGL shim: loads Mesa's EGL and GBM through the
 * same-process-HAL namespace (as the shim will), opens an EGL display on the
 * render node via EGL_EXT_platform_device and prints what glamor needs.
 * Run on the device: probe [/dev/dri/renderD128] */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

typedef void* (*LoadSphal)(const char*, int);
#define LOAD(lib, name) (__typeof__(&name))dlsym(lib, #name)

int main(int argc, char** argv) {
  const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
  void* vndk = dlopen("libvndksupport.so", RTLD_NOW);
  LoadSphal load = vndk ? (LoadSphal)dlsym(vndk, "android_load_sphal_library") : NULL;
  if (!load) { printf("FAIL: android_load_sphal_library unavailable (%s)\n", dlerror()); return 1; }
  void* egl = load("libEGL_mesa.so", RTLD_NOW | RTLD_LOCAL);
  void* gles = load("libGLESv2_mesa.so", RTLD_NOW | RTLD_LOCAL);
  void* gbm = load("libgbm_mesa.so", RTLD_NOW | RTLD_LOCAL);
  printf("sphal: libEGL_mesa=%s libGLESv2_mesa=%s libgbm_mesa=%s\n", egl ? "ok" : "FAIL",
         gles ? "ok" : "FAIL", gbm ? "ok" : "FAIL");
  if (!egl) { printf("  %s\n", dlerror()); return 1; }

  __typeof__(&eglQueryString) query = LOAD(egl, eglQueryString);
  __typeof__(&eglGetProcAddress) proc = LOAD(egl, eglGetProcAddress);
  __typeof__(&eglInitialize) init = LOAD(egl, eglInitialize);
  __typeof__(&eglBindAPI) bind = LOAD(egl, eglBindAPI);
  __typeof__(&eglCreateContext) create_context = LOAD(egl, eglCreateContext);
  __typeof__(&eglMakeCurrent) make_current = LOAD(egl, eglMakeCurrent);
  printf("client extensions: %s\n", query(EGL_NO_DISPLAY, EGL_EXTENSIONS));

  PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)proc("eglQueryDevicesEXT");
  PFNEGLQUERYDEVICESTRINGEXTPROC device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)proc("eglQueryDeviceStringEXT");
  PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)proc("eglGetPlatformDisplayEXT");
  if (!query_devices || !device_string || !platform_display) { printf("FAIL: device-platform entry points missing\n"); return 1; }
  struct stat want;
  if (stat(node, &want)) { printf("FAIL: stat %s\n", node); return 1; }
  EGLDeviceEXT devices[16]; EGLint count = 0;
  query_devices(16, devices, &count);
  EGLDeviceEXT match = EGL_NO_DEVICE_EXT;
  for (EGLint i = 0; i < count; ++i) {
    const char* path = device_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
    struct stat st;
    printf("device %d: render node %s\n", i, path ? path : "(none)");
    if (path && !stat(path, &st) && st.st_rdev == want.st_rdev) match = devices[i];
  }
  if (match == EGL_NO_DEVICE_EXT) { printf("FAIL: no EGL device for %s\n", node); return 1; }
  EGLDisplay display = platform_display(EGL_PLATFORM_DEVICE_EXT, match, NULL);
  EGLint major = 0, minor = 0;
  if (display == EGL_NO_DISPLAY || !init(display, &major, &minor)) { printf("FAIL: device display init\n"); return 1; }
  printf("display: EGL %d.%d vendor=%s\n", major, minor, query(display, EGL_VENDOR));
  const char* ext = query(display, EGL_EXTENSIONS);
  const char* needed[] = {"EGL_KHR_surfaceless_context", "EGL_KHR_no_config_context",
      "EGL_EXT_image_dma_buf_import", "EGL_EXT_image_dma_buf_import_modifiers",
      "EGL_MESA_image_dma_buf_export", "EGL_ANDROID_native_fence_sync", "EGL_KHR_fence_sync"};
  for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i)
    printf("  %-40s %s\n", needed[i], ext && strstr(ext, needed[i]) ? "yes" : "NO");

  if (!bind(EGL_OPENGL_ES_API)) { printf("FAIL: bind GLES\n"); return 1; }
  const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = create_context(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
  if (context == EGL_NO_CONTEXT || !make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
    printf("FAIL: surfaceless GLES context\n"); return 1;
  }
  const GLubyte* (*get_string)(GLenum) = gles ? dlsym(gles, "glGetString") : NULL;
  if (!get_string) get_string = (const GLubyte* (*)(GLenum))proc("glGetString");
  printf("GL_RENDERER=%s\nGL_VERSION=%s\n", get_string(GL_RENDERER), get_string(GL_VERSION));
  const char* gl_ext = (const char*)get_string(GL_EXTENSIONS);
  printf("  %-40s %s\n", "GL_OES_EGL_image", gl_ext && strstr(gl_ext, "GL_OES_EGL_image") ? "yes" : "NO");
  printf("gbm_create_device: %s\n", gbm && dlsym(gbm, "gbm_create_device") ? "ok" : "FAIL");
  printf("PASS (probe complete)\n");
  return 0;
}
