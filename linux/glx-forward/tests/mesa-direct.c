/* Host-only resident boundary: real Mesa EGL desktop rendering, no Android TLS. */
#include "maton_egl_direct.h"
static void *proc(const char *name){return (void *)eglGetProcAddress(name);}
const struct maton_egl_direct_v1 *maton_egl_direct_v1(void){
    static const struct maton_egl_direct_v1 api={
        .version=1,.get_platform_display=eglGetPlatformDisplay,.get_display=eglGetDisplay,
        .initialize=eglInitialize,.bind_api=eglBindAPI,.query_api=eglQueryAPI,.choose_config=eglChooseConfig,
        .create_context=eglCreateContext,.create_pbuffer_surface=eglCreatePbufferSurface,
        .make_current=eglMakeCurrent,.destroy_surface=eglDestroySurface,.destroy_context=eglDestroyContext,
        .get_proc_address=proc};return &api;
}
