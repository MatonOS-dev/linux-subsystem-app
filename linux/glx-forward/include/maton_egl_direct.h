#ifndef MATON_EGL_DIRECT_H
#define MATON_EGL_DIRECT_H

#include <stdint.h>
#include <EGL/egl.h>

/* Private, in-process ABI shared only by libGLX_maton and libEGL_maton. */
struct maton_egl_direct_v1 {
    uint32_t version;
    EGLDisplay (*get_platform_display)(EGLenum, void *, const EGLAttrib *);
    EGLDisplay (*get_display)(EGLNativeDisplayType);
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*bind_api)(EGLenum);
    EGLBoolean (*choose_config)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*create_pbuffer_surface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface);
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
    void *(*get_proc_address)(const char *);
};

typedef const struct maton_egl_direct_v1 *(*maton_egl_direct_v1_fn)(void);

#endif
