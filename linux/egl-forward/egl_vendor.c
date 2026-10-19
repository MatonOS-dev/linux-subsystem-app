#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <glvnd/libeglabi.h>
#include "runtime.h"
#include "backend.h"
#include "../gpu-forward-spike/bridge.h"

#define EXPORT __attribute__((visibility("default")))
#define MV_EGL 1
#define MV_GLES 2
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static struct adopt_api *mv_adopt;
static struct gpu_bridge *mv_bridge;
static void *mv_egl_lib, *mv_gles_lib;
static __eglMustCastToProperFunctionPointerType (*mv_android_egl_get_proc)(const char *);
static int mv_fast, mv_state;
static void *mv_lookup_proc(const char *name);
static int mv_has_token(const char *list, const char *token);

static void mv_egl_diagnostic(void) {
    static const char message[] = "matonos-egl: bridge or thread adoption unavailable\n";
    mv_write(message, sizeof(message) - 1);
}
static int mv_enter(struct mv_scope *scope) {
    if (switch_to_android(mv_adopt, mv_fast, scope)) return 1;
    mv_egl_diagnostic();
    return 0;
}
static void mv_fail(void) { mv_egl_diagnostic(); }

static int mv_ready(void) {
    int state = __atomic_load_n(&mv_state, __ATOMIC_ACQUIRE);
    if (state == 2) return 1;
    if (state == 3) return 0;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&mv_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while ((state = __atomic_load_n(&mv_state, __ATOMIC_ACQUIRE)) == 1) {}
        return state == 2;
    }
    mv_bridge = (void *)mv_hex(mv_env("MATON_GPU_BRIDGE"));
    mv_adopt = (void *)mv_hex(mv_env("MATON_THREAD_ADOPT"));
    mv_fast = mv_hex(mv_env("MATON_FAST_TLS")) != 0;
    struct mv_scope scope;
    if (mv_bridge && mv_bridge->version == 2 && mv_enter(&scope)) {
        mv_egl_lib = mv_bridge->open("/system/lib64/libEGL.so", 2);
        mv_gles_lib = mv_bridge->open("/system/lib64/libGLESv2.so", 2);
        if (mv_egl_lib)
            mv_android_egl_get_proc = (void *)mv_bridge->symbol(mv_egl_lib, "eglGetProcAddress");
        switch_back(&scope);
    }
    int ok = mv_egl_lib && mv_gles_lib && mv_android_egl_get_proc;
    __atomic_store_n(&mv_state, ok ? 2 : 3, __ATOMIC_RELEASE);
    if (!ok) mv_egl_diagnostic();
    return ok;
}

/* Called only after switching into resident bionic TLS. */
static void *mv_resolve(const char *name, int which) {
    void *lib = which == MV_EGL ? mv_egl_lib : mv_gles_lib;
    void *p = mv_bridge->symbol(lib, name);
    if (!p && mv_android_egl_get_proc)
        p = (void *)mv_android_egl_get_proc(name);
    return p;
}

#include "egl_thunks.inc"
#include "gles_thunks.inc"

/* Android's EGL is used as the implementation, while the surfaceless
 * platform is a Linux-side property of this vendor adapter. Preserve the
 * Android extension strings and advertise the platform we map to its default
 * display. The strings are copied before restoring Linux TLS. */
static char mv_extension_string[2][16384];
static int mv_extension_lock[2];
static int mv_has_token(const char *list, const char *token) {
    size_t len = 0;
    while (token[len]) ++len;
    for (const char *p = list; *p;) {
        const char *start = p;
        while (*p && *p != ' ') ++p;
        if ((size_t)(p - start) == len) {
            size_t i = 0;
            while (i < len && start[i] == token[i]) ++i;
            if (i == len) return 1;
        }
        while (*p == ' ') ++p;
    }
    return 0;
}
static EGLBoolean mv_append_token(char *dst, const char *src, const char *token) {
    size_t n = 0;
    while (src[n] && n + 1 < sizeof(mv_extension_string[0])) { dst[n] = src[n]; ++n; }
    if (src[n]) return EGL_FALSE;
    dst[n] = '\0';
    if (!mv_has_token(dst, token)) {
        size_t t = 0;
        while (token[t]) ++t;
        if (n && n + 1 < sizeof(mv_extension_string[0])) dst[n++] = ' ';
        if (n + t >= sizeof(mv_extension_string[0])) return EGL_FALSE;
        for (size_t i = 0; i < t; ++i) dst[n++] = token[i];
        dst[n] = '\0';
    }
    return EGL_TRUE;
}
static const char *mv_eglQueryString(EGLDisplay dpy, EGLint name) {
    struct mv_scope scope;
    if (!mv_ready() || !mv_enter(&scope)) { mv_fail(); return 0; }
    typedef const char *(*fn_t)(EGLDisplay, EGLint);
    fn_t fn = (fn_t)mv_resolve("eglQueryString", MV_EGL);
    const char *result = fn ? fn(dpy, name) : 0;
    if (name != EGL_EXTENSIONS || !result) { switch_back(&scope); return result; }
    int index = dpy == EGL_NO_DISPLAY ? 0 : 1;
    while (__atomic_exchange_n(&mv_extension_lock[index], 1, __ATOMIC_ACQUIRE)) {}
    EGLBoolean ok = mv_append_token(mv_extension_string[index], result, "EGL_MESA_platform_surfaceless");
    switch_back(&scope);
    __atomic_store_n(&mv_extension_lock[index], 0, __ATOMIC_RELEASE);
    return ok ? mv_extension_string[index] : 0;
}
static __eglMustCastToProperFunctionPointerType mv_eglGetProcAddress(const char *name) {
    return (__eglMustCastToProperFunctionPointerType)mv_lookup_proc(name);
}
#include "proc_lookup.inc"

static EGLDisplay mv_vendor_get_platform_display(EGLenum platform, void *native, const EGLAttrib *attrs) {
    if (platform == EGL_PLATFORM_SURFACELESS_MESA) {
        EGLDisplay display = mv_eglGetPlatformDisplay(platform, native, attrs);
        if (display != EGL_NO_DISPLAY) return display;
        return mv_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (platform == EGL_NONE) return mv_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return EGL_NO_DISPLAY;
}
static EGLBoolean mv_vendor_supports_api(EGLenum api) {
    const char *apis = mv_eglQueryString(EGL_NO_DISPLAY, EGL_CLIENT_APIS);
    if (!apis) return EGL_FALSE;
    if (api == EGL_OPENGL_ES_API) return mv_has_token(apis, "OpenGL_ES") ? EGL_TRUE : EGL_FALSE;
    if (api == EGL_OPENGL_API) return mv_has_token(apis, "OpenGL") ? EGL_TRUE : EGL_FALSE;
    return EGL_FALSE;
}
static const char *mv_vendor_string(int name) {
    (void)name;
    return "MatonOS Android EGL passthrough";
}
static void *mv_vendor_get_proc(const char *name) { return mv_lookup_proc(name); }
static void *mv_vendor_get_dispatch(const char *name) { return mv_lookup_proc(name); }
static void mv_vendor_set_dispatch(const char *name, int index) { (void)name; (void)index; }

EXPORT EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports,
                             __EGLvendorInfo *vendor, __EGLapiImports *imports) {
    (void)exports;
    (void)vendor;
    if (!imports || EGL_VENDOR_ABI_GET_MAJOR_VERSION(version) != EGL_VENDOR_ABI_MAJOR_VERSION ||
        EGL_VENDOR_ABI_GET_MINOR_VERSION(version) < 2) return EGL_FALSE;
    imports->getPlatformDisplay = mv_vendor_get_platform_display;
    imports->getSupportsAPI = mv_vendor_supports_api;
    imports->getVendorString = mv_vendor_string;
    imports->getProcAddress = mv_vendor_get_proc;
    imports->getDispatchAddress = mv_vendor_get_dispatch;
    imports->setDispatchIndex = mv_vendor_set_dispatch;
    imports->isPatchSupported = 0;
    imports->initiatePatch = 0;
    imports->releasePatch = 0;
    imports->patchThreadAttach = 0;
    imports->findNativeDisplayPlatform = 0;
    return EGL_TRUE;
}
