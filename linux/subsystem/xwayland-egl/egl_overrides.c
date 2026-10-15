/* The part of the Xwayland EGL shim that differs from Mesa: Xwayland's glamor
 * opens its EGL display with EGL_PLATFORM_GBM_MESA, which Mesa leaves out of
 * Android builds. The same display is reachable through EGL_EXT_platform_device
 * by the render node of the gbm device, so GBM-platform requests are
 * translated to that, and the GBM platform extensions are advertised. Every
 * other EGL call goes straight to Mesa (egl_forward.c). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sphal.h"

#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif

struct gbm_device;

static struct {
  __eglMustCastToProperFunctionPointerType (*get_proc_address)(const char*);
  const char* (*query_string)(EGLDisplay, EGLint);
  EGLDisplay (*get_platform_display)(EGLenum, void*, const EGLAttrib*);
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext;
  PFNEGLQUERYDEVICESEXTPROC query_devices;
  PFNEGLQUERYDEVICESTRINGEXTPROC device_string;
  int (*gbm_device_get_fd)(struct gbm_device*);
  char* client_extensions;
} real;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void resolve(void) {
  void* egl = maton_sphal_load("libEGL_mesa.so");
  void* gbm = maton_sphal_load("libgbm_mesa.so");
  if (egl) {
    real.get_proc_address = dlsym(egl, "eglGetProcAddress");
    real.query_string = dlsym(egl, "eglQueryString");
    real.get_platform_display = dlsym(egl, "eglGetPlatformDisplay");
  }
  if (gbm) real.gbm_device_get_fd = dlsym(gbm, "gbm_device_get_fd");
  if (real.get_proc_address) {
    real.get_platform_display_ext = (PFNEGLGETPLATFORMDISPLAYEXTPROC)real.get_proc_address("eglGetPlatformDisplayEXT");
    real.query_devices = (PFNEGLQUERYDEVICESEXTPROC)real.get_proc_address("eglQueryDevicesEXT");
    real.device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)real.get_proc_address("eglQueryDeviceStringEXT");
  }
  const char* base = real.query_string ? real.query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS) : NULL;
  if (base && real.query_devices && real.device_string && real.gbm_device_get_fd) {
    static const char extra[] = " EGL_KHR_platform_gbm EGL_MESA_platform_gbm";
    real.client_extensions = malloc(strlen(base) + sizeof(extra));
    if (real.client_extensions) { strcpy(real.client_extensions, base); strcat(real.client_extensions, extra); }
  }
}

/* The EGL device whose render node is the gbm device's. */
static EGLDeviceEXT device_for_gbm(struct gbm_device* gbm) {
  int fd = gbm && real.gbm_device_get_fd ? real.gbm_device_get_fd(gbm) : -1;
  struct stat want;
  if (fd < 0 || fstat(fd, &want) || !S_ISCHR(want.st_mode)) return EGL_NO_DEVICE_EXT;
  EGLDeviceEXT devices[16];
  EGLint count = 0;
  if (!real.query_devices(16, devices, &count)) return EGL_NO_DEVICE_EXT;
  for (EGLint i = 0; i < count; ++i) {
    const char* path = real.device_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
    struct stat st;
    if (path && !stat(path, &st) && st.st_rdev == want.st_rdev) return devices[i];
  }
  __android_log_print(ANDROID_LOG_ERROR, "MatonXwaylandEGL", "no EGL device for the gbm device's render node");
  return EGL_NO_DEVICE_EXT;
}

static EGLDisplay gbm_display(void* native) {
  EGLDeviceEXT device = device_for_gbm(native);
  if (device == EGL_NO_DEVICE_EXT) return EGL_NO_DISPLAY;
  return real.get_platform_display_ext(EGL_PLATFORM_DEVICE_EXT, device, NULL);
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native, const EGLAttrib* attribs) {
  pthread_once(&once, resolve);
  if (platform == EGL_PLATFORM_GBM_MESA && real.client_extensions) return gbm_display(native);
  return real.get_platform_display ? real.get_platform_display(platform, native, attribs) : EGL_NO_DISPLAY;
}

static EGLDisplay EGLAPIENTRY get_platform_display_ext(EGLenum platform, void* native, const EGLint* attribs) {
  pthread_once(&once, resolve);
  if (platform == EGL_PLATFORM_GBM_MESA && real.client_extensions) return gbm_display(native);
  return real.get_platform_display_ext ? real.get_platform_display_ext(platform, native, attribs) : EGL_NO_DISPLAY;
}

const char* eglQueryString(EGLDisplay display, EGLint name) {
  pthread_once(&once, resolve);
  if (display == EGL_NO_DISPLAY && name == EGL_EXTENSIONS && real.client_extensions) return real.client_extensions;
  return real.query_string ? real.query_string(display, name) : NULL;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
  pthread_once(&once, resolve);
  if (name && !strcmp(name, "eglGetPlatformDisplay")) return (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplay;
  if (name && !strcmp(name, "eglGetPlatformDisplayEXT")) return (__eglMustCastToProperFunctionPointerType)get_platform_display_ext;
  if (name && !strcmp(name, "eglQueryString")) return (__eglMustCastToProperFunctionPointerType)eglQueryString;
  return real.get_proc_address ? real.get_proc_address(name) : NULL;
}
