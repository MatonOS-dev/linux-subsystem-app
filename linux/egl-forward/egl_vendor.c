#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <glvnd/libeglabi.h>
#include "runtime.h"
#include "backend.h"
#include "../gpu-bridge-common/bridge.h"
#include "../wayland-client/wayland_client.h"
#include "../vulkan-forward/wayland_resolve.h"
#include "../glx-forward/include/maton_egl_direct.h"

#if defined(__i386__)
#define MV_ANDROID_LIBDIR "/system/lib/"
#else
#define MV_ANDROID_LIBDIR "/system/lib64/"
#endif

#define EXPORT __attribute__((visibility("default")))
#define MV_EGL 1
#define MV_GLES 2
#define MV_DESKTOP 3
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static struct adopt_api *mv_adopt;
static struct gpu_bridge *mv_bridge;
static void *mv_egl_lib, *mv_gles_lib;
static void *mv_desktop_egl_lib, *mv_vndksupport_lib, *mv_libc;
static __eglMustCastToProperFunctionPointerType (*mv_android_egl_get_proc)(const char *);
static __eglMustCastToProperFunctionPointerType (*mv_desktop_egl_get_proc)(const char *);
static int (*mv_property_get)(const char *, char *);
static void *(*mv_load_sphal)(const char *, int);
static int mv_desktop_state;
static int mv_fast, mv_state;
static void *mv_lookup_proc(const char *name);
static int mv_has_token(const char *list, const char *token);

static void mv_egl_diagnostic(const char *message, size_t length) {
    mv_write(message, length);
}
static int mv_enter(struct mv_scope *scope) {
    if (switch_to_android(mv_adopt, mv_fast, scope)) return 1;
    static const char message[] = "matonos-egl: thread adoption failed\n";
    mv_egl_diagnostic(message, sizeof(message) - 1);
    return 0;
}
static void mv_fail(void) {
    static const char message[] = "matonos-egl: EGL forwarding failed\n";
    mv_egl_diagnostic(message, sizeof(message) - 1);
}

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
    if (!mv_bridge || !mv_adopt) {
        static const char message[] = "matonos-egl: GPU bootstrap bridge or adoption API missing\n";
        mv_egl_diagnostic(message, sizeof(message) - 1);
    }
    struct mv_scope scope;
    if (mv_bridge && mv_bridge->version == 2 && mv_enter(&scope)) {
        mv_egl_lib = (void *)mv_hex(mv_env("MATON_GPU_EGL"));
        mv_gles_lib = (void *)mv_hex(mv_env("MATON_GPU_GLES"));
        if (mv_egl_lib)
            mv_android_egl_get_proc = (void *)mv_bridge->symbol(mv_egl_lib, "eglGetProcAddress");
        switch_back(&scope);
    }
    int ok = mv_android_egl_get_proc != 0;
    __atomic_store_n(&mv_state, ok ? 2 : 0, __ATOMIC_RELEASE);
    if (!ok) {
        static const char message[] = "matonos-egl: resident EGL/GLES libraries unavailable\n";
        mv_egl_diagnostic(message, sizeof(message) - 1);
    }
    return ok;
}

/* Called only after switching into resident bionic TLS. */
static void *mv_resolve(const char *name, int which) {
    if (which == MV_DESKTOP)
        return mv_desktop_egl_get_proc ? (void *)mv_desktop_egl_get_proc(name) : 0;
    void *lib = which == MV_EGL ? mv_egl_lib : mv_gles_lib;
    void *p = lib ? mv_bridge->symbol(lib, name) : 0;
    if (!p && mv_android_egl_get_proc)
        p = (void *)mv_android_egl_get_proc(name);
    return p;
}

#include "egl_thunks.inc"
#include "gles_thunks.inc"
#include "desktop_thunks.inc"
#include "shared_thunks.inc"

/* Resolve the already resident vendor EGL through libvndksupport so bionic
 * returns the sphal namespace instance instead of loading a second driver. */
static int mv_desktop_driver_init(void) {
    if (mv_desktop_egl_get_proc) return 1;
    if (!mv_vndksupport_lib) mv_vndksupport_lib = mv_bridge->open(MV_ANDROID_LIBDIR "libvndksupport.so", 2);
    if (!mv_libc) mv_libc = mv_bridge->open(MV_ANDROID_LIBDIR "libc.so", 2);
    if (!mv_load_sphal && mv_vndksupport_lib)
        mv_load_sphal = (void *(*)(const char *, int))mv_bridge->symbol(mv_vndksupport_lib, "android_load_sphal_library");
    if (!mv_property_get && mv_libc)
        mv_property_get = (int (*)(const char *, char *))mv_bridge->symbol(mv_libc, "__system_property_get");
    if (!mv_load_sphal || !mv_property_get) return 0;
    char hardware[96] = {0};
    if (mv_property_get("ro.hardware.egl", hardware) <= 0 || !hardware[0]) return 0;
    char library[128] = "libEGL_";
    size_t i = 0;
    while (hardware[i] && i < sizeof(library) - 13) { library[7 + i] = hardware[i]; ++i; }
    if (hardware[i]) return 0;
    const char suffix[] = ".so";
    for (size_t j = 0; j < sizeof(suffix); ++j) library[7 + i + j] = suffix[j];
    mv_desktop_egl_lib = mv_load_sphal(library, 2 | 0);
    if (!mv_desktop_egl_lib) return 0;
    mv_desktop_egl_get_proc = (__eglMustCastToProperFunctionPointerType (*)(const char *))
        mv_bridge->symbol(mv_desktop_egl_lib, "eglGetProcAddress");
    return mv_desktop_egl_get_proc != 0;
}

/* Called on Android TLS. Probe the resident EGL implementation and restore
 * its per-thread API binding before returning to Linux TLS. */
static int mv_desktop_api_supported(void) {
    int state = __atomic_load_n(&mv_desktop_state, __ATOMIC_ACQUIRE);
    if (state) return state == 1;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&mv_desktop_state, &expected, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while ((state = __atomic_load_n(&mv_desktop_state, __ATOMIC_ACQUIRE)) == 3) {}
        return state == 1;
    }
    int supported = mv_desktop_driver_init();
    typedef EGLenum (*query_api_fn)(void);
    typedef EGLBoolean (*bind_api_fn)(EGLenum);
    query_api_fn query = (query_api_fn)mv_resolve("eglQueryAPI", MV_EGL);
    bind_api_fn bind = (bind_api_fn)mv_resolve("eglBindAPI", MV_EGL);
    EGLenum previous = query ? query() : EGL_OPENGL_ES_API;
    EGLBoolean bound = query && bind && bind(EGL_OPENGL_API);
    if (!bound) supported = 0;
    if (bound && !bind(previous)) supported = 0;
    __atomic_store_n(&mv_desktop_state, supported ? 1 : 2, __ATOMIC_RELEASE);
    return supported;
}

/* The staged libwayland-egl is pinned to Wayland 1.24.0. Its backend ABI
 * version 3 places surface after the driver callbacks; check the version before
 * reading this private field layout. */
struct wl_egl_window_abi {
    intptr_t version;
    int width, height, dx, dy, attached_width, attached_height;
    void *driver_private;
    void (*resize_callback)(struct wl_egl_window_abi *, void *);
    void (*destroy_window_callback)(void *);
    struct wl_surface *surface;
};
struct mv_egl_surface {
    EGLSurface handle; /* Stable app-visible handle: the first pbuffer created. */
    EGLDisplay display;
    EGLConfig config;
    EGLSurface pbuffer;
    struct wl_egl_window_abi *window;
    struct mv_wl_dmabuf *wayland;
    struct mv_wl_buffer *buffer;
    struct mv_wl_sync_surface *sync;
    void *ahb;
    void *image;
    EGLint width, height;
    GLuint texture, framebuffer;
    int submitted, swap_interval;
    unsigned refs; /* Includes one table reference while published. */
    int removed;
    struct mv_egl_surface *next;
};
struct mv_ahb_desc { uint32_t width,height,layers,format; uint64_t usage; uint32_t stride,rfu0; uint64_t rfu1; };
static struct mv_egl_surface *mv_surfaces;
static void *mv_egl_ext(const char *name);
static unsigned mv_surface_lock;
static void mv_surface_lock_acquire(void) {
    while (__atomic_test_and_set(&mv_surface_lock, __ATOMIC_ACQUIRE)) {}
}
static void mv_surface_lock_release(void) { __atomic_clear(&mv_surface_lock, __ATOMIC_RELEASE); }
static void *mv_android_lib;
static void *mv_android_symbol(const char *name) {
    if (!mv_android_lib) mv_android_lib = mv_bridge->open(MV_ANDROID_LIBDIR "libandroid.so", 2);
    return mv_android_lib ? mv_bridge->symbol(mv_android_lib, name) : 0;
}
static struct mv_egl_surface *mv_surface_find(EGLSurface handle) {
    mv_surface_lock_acquire();
    struct mv_egl_surface *found=0;
    for (struct mv_egl_surface *s=mv_surfaces;s;s=s->next) if (s->handle==handle) { ++s->refs; found=s; break; }
    mv_surface_lock_release();
    return found;
}
static void mv_surface_put(struct mv_egl_surface *s) {
    if (!s) return;
    mv_surface_lock_acquire(); int release=--s->refs==0; mv_surface_lock_release();
    if (!release) return;
    struct mv_scope scope;
    if (mv_enter(&scope)) {
        typedef EGLBoolean (*di_fn)(EGLDisplay,void *); di_fn di=(di_fn)mv_egl_ext("eglDestroyImageKHR"); if(s->image&&di)di(s->display,s->image);
        typedef void (*rel_fn)(void *); rel_fn rel=(void (*)(void *))mv_android_symbol("AHardwareBuffer_release"); if(s->ahb&&rel)rel(s->ahb);
        typedef EGLBoolean (*ds_fn)(EGLDisplay,EGLSurface); ds_fn ds=(ds_fn)mv_resolve("eglDestroySurface",MV_EGL); if(s->pbuffer&&ds)ds(s->display,s->pbuffer);
        typedef void (*del_fn)(GLsizei,const GLuint *); del_fn dt=(del_fn)mv_resolve("glDeleteTextures",MV_GLES),df=(del_fn)mv_resolve("glDeleteFramebuffers",MV_GLES);
        if(s->texture&&dt)dt(1,&s->texture); if(s->framebuffer&&df)df(1,&s->framebuffer);
        switch_back(&scope);
    }
    if(s->buffer)mv_wl_buffer_destroy(s->buffer); if(s->sync)mv_wl_sync_surface_destroy(s->sync); if(s->wayland)mv_wl_dmabuf_destroy(s->wayland);
    mv_free(s);
}
static void mv_surface_publish(struct mv_egl_surface *s) {
    mv_surface_lock_acquire(); s->refs=1; s->next=mv_surfaces; mv_surfaces=s; mv_surface_lock_release();
}
static int mv_surface_unpublish(struct mv_egl_surface *s) {
    mv_surface_lock_acquire(); struct mv_egl_surface **p=&mv_surfaces; while(*p&&*p!=s)p=&(*p)->next;
    int found=*p==s; if(found){*p=s->next;s->next=0;s->removed=1;} mv_surface_lock_release();
    if(found)mv_surface_put(s); return found;
}
static void *mv_egl_ext(const char *name) { return (void *)mv_android_egl_get_proc(name); }
static void *mv_current_gl_resolve(const char *name) {
    typedef EGLenum (*query_fn)(void);
    query_fn query=(query_fn)mv_resolve("eglQueryAPI",MV_EGL);
    return mv_resolve(name,query && query()==EGL_OPENGL_API?MV_DESKTOP:MV_GLES);
}

static EGLSurface mv_eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
        void *native_window, const EGLAttrib *attrs) {
    if ((attrs && attrs[0]!=EGL_NONE) || !native_window || !mv_ready()) return EGL_NO_SURFACE;
    struct wl_egl_window_abi *w=native_window;
    if (w->version != 3 || !w->surface || w->width<1 || w->height<1) return EGL_NO_SURFACE;
    struct mv_egl_surface *s=mv_alloc(sizeof(*s));
    if (!s) return EGL_NO_SURFACE;
    memset(s,0,sizeof(*s)); s->display=dpy; s->config=config; s->window=w; s->width=w->width; s->height=w->height; s->swap_interval=1;
    struct mv_scope scope;
    if (!mv_enter(&scope)) { mv_free(s); return EGL_NO_SURFACE; }
    typedef EGLSurface (*pbuf_fn)(EGLDisplay,EGLConfig,const EGLint *);
    static uintptr_t cache;
    pbuf_fn pbuf=(pbuf_fn)__atomic_load_n(&cache,__ATOMIC_ACQUIRE);
    if (!pbuf) { pbuf=(pbuf_fn)mv_resolve("eglCreatePbufferSurface",MV_EGL); if(pbuf)__atomic_store_n(&cache,(uintptr_t)pbuf,__ATOMIC_RELEASE); }
    EGLint pa[]={EGL_WIDTH,s->width,EGL_HEIGHT,s->height,EGL_NONE};
    if (pbuf) s->pbuffer=pbuf(dpy,config,pa);
    typedef int (*alloc_fn)(const struct mv_ahb_desc *,void **);
    typedef int (*send_fn)(const void *,int);
    alloc_fn alloc=(alloc_fn)mv_android_symbol("AHardwareBuffer_allocate");
    send_fn send=(send_fn)mv_android_symbol("AHardwareBuffer_sendHandleToUnixSocket");
    typedef EGLClientBuffer (*client_fn)(const void *);
    typedef void *(*image_fn)(EGLDisplay,EGLContext,EGLenum,EGLClientBuffer,const EGLint *);
    client_fn client=(client_fn)mv_egl_ext("eglGetNativeClientBufferANDROID");
    image_fn create_image=(image_fn)mv_egl_ext("eglCreateImageKHR");
    struct mv_ahb_desc desc={(uint32_t)s->width,(uint32_t)s->height,1,1,0x200|0x800,0,0,0};
    int pair[2]={-1,-1};
    if (alloc && send && client && create_image && s->pbuffer &&
        alloc(&desc,&s->ahb)==0 && s->ahb && mv_socketpair(pair)==0) {
        if (send(s->ahb,pair[1])==0) {
            EGLint ia[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
            s->image=create_image(dpy,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,client(s->ahb),ia);
        }
    }
    if(pair[1]>=0)mv_close(pair[1]);
    switch_back(&scope);
    if (!s->pbuffer || !s->image || pair[0]<0) goto fail;
    typedef struct wl_display *(*get_display_fn)(struct wl_proxy *);
    get_display_fn get_display=(get_display_fn)mv_wayland_symbol("wl_proxy_get_display");
    struct wl_display *wl_display=get_display?get_display((struct wl_proxy *)w->surface):0;
    s->wayland=wl_display?mv_wl_dmabuf_create(wl_display):0;
    s->sync=s->wayland?mv_wl_sync_surface_create(s->wayland,w->surface):0;
    s->buffer=s->wayland?mv_wl_ahb_create_buffer(s->wayland,(uint32_t)s->width,(uint32_t)s->height,0x34324241u,pair[0]):0;
    mv_close(pair[0]);
    if (!s->wayland || !s->sync || !s->buffer) goto fail;
    s->handle=s->pbuffer;
    mv_surface_publish(s);
    return s->handle;
fail:
    if (s->buffer) mv_wl_buffer_destroy(s->buffer);
    if (s->sync) mv_wl_sync_surface_destroy(s->sync);
    if (s->wayland) mv_wl_dmabuf_destroy(s->wayland);
    if (s->image || s->ahb || s->pbuffer) {
        if (mv_enter(&scope)) {
            typedef EGLBoolean (*destroy_image_fn)(EGLDisplay,void *);
            destroy_image_fn di=(destroy_image_fn)mv_egl_ext("eglDestroyImageKHR"); if(s->image&&di)di(dpy,s->image);
            typedef void (*release_fn)(void *); release_fn rel=(release_fn)mv_android_symbol("AHardwareBuffer_release"); if(s->ahb&&rel)rel(s->ahb);
            typedef EGLBoolean (*destroy_surface_fn)(EGLDisplay,EGLSurface); destroy_surface_fn ds=(destroy_surface_fn)mv_resolve("eglDestroySurface",MV_EGL); if(s->pbuffer&&ds)ds(dpy,s->pbuffer);
            switch_back(&scope);
        }
    }
    mv_free(s); return EGL_NO_SURFACE;
}
static EGLSurface mv_eglCreateWindowSurface(EGLDisplay dpy,EGLConfig config,
        EGLNativeWindowType window,const EGLint *attrs) {
    if(attrs && attrs[0]!=EGL_NONE)return EGL_NO_SURFACE;
    return mv_eglCreatePlatformWindowSurface(dpy,config,(void *)window,0);
}
static int mv_surface_resize(struct mv_egl_surface *s) {
    if(!s||!s->window||(s->width==s->window->width&&s->height==s->window->height))return 1;
    if(s->window->width<1||s->window->height<1)return 0;
    if(s->submitted){while(!mv_wl_buffer_released(s->buffer))if(mv_wl_dmabuf_dispatch_one(s->wayland)<0)return 0;}
    EGLSurface replacement=mv_eglCreatePlatformWindowSurface(s->display,s->config,s->window,0);
    struct mv_egl_surface *n=mv_surface_find(replacement); if(!n)return 0;
    struct mv_scope scope;
    if(!mv_enter(&scope)){mv_eglDestroySurface(s->display,replacement);mv_surface_put(n);return 0;}
    typedef EGLContext (*getctx_fn)(void); getctx_fn getctx=(getctx_fn)mv_resolve("eglGetCurrentContext",MV_EGL);
    typedef EGLSurface (*get_surface_fn)(EGLint); get_surface_fn get_surface=(get_surface_fn)mv_resolve("eglGetCurrentSurface",MV_EGL);
    typedef EGLBoolean (*make_fn)(EGLDisplay,EGLSurface,EGLSurface,EGLContext); make_fn make=(make_fn)mv_resolve("eglMakeCurrent",MV_EGL);
    EGLContext ctx=getctx?getctx():EGL_NO_CONTEXT;
    EGLSurface draw=get_surface?get_surface(EGL_DRAW):EGL_NO_SURFACE, read=get_surface?get_surface(EGL_READ):EGL_NO_SURFACE;
    if(ctx&&(draw==s->pbuffer||read==s->pbuffer)&&(!make||!make(s->display,n->pbuffer,n->pbuffer,ctx))){switch_back(&scope);mv_eglDestroySurface(s->display,replacement);mv_surface_put(n);return 0;}
    typedef void (*delete_fn)(GLsizei,const GLuint *); delete_fn deltex=(delete_fn)mv_resolve("glDeleteTextures",MV_GLES),delfb=(delete_fn)mv_resolve("glDeleteFramebuffers",MV_GLES);
    if(s->texture&&deltex)deltex(1,&s->texture);if(s->framebuffer&&delfb)delfb(1,&s->framebuffer);
    typedef EGLBoolean (*di_fn)(EGLDisplay,void *); di_fn di=(di_fn)mv_egl_ext("eglDestroyImageKHR"); if(s->image&&di)di(s->display,s->image);
    typedef void (*rel_fn)(void *); rel_fn rel=(rel_fn)mv_android_symbol("AHardwareBuffer_release"); if(s->ahb&&rel)rel(s->ahb);
    typedef EGLBoolean (*ds_fn)(EGLDisplay,EGLSurface); ds_fn ds=(ds_fn)mv_resolve("eglDestroySurface",MV_EGL); if(s->pbuffer&&ds)ds(s->display,s->pbuffer);
    switch_back(&scope);
    mv_wl_buffer_destroy(s->buffer);mv_wl_sync_surface_destroy(s->sync);mv_wl_dmabuf_destroy(s->wayland);
    /* Keep the first pbuffer as the application handle; translate it to the
     * current native pbuffer at every Android EGL call after a resize. */
    mv_surface_lock_acquire();
    EGLSurface old_pbuffer=s->pbuffer; void *old_image=s->image,*old_ahb=s->ahb;
    struct mv_wl_dmabuf *old_wayland=s->wayland; struct mv_wl_buffer *old_buffer=s->buffer; struct mv_wl_sync_surface *old_sync=s->sync;
    s->pbuffer=n->pbuffer; s->image=n->image; s->ahb=n->ahb; s->wayland=n->wayland; s->buffer=n->buffer; s->sync=n->sync;
    s->width=n->width; s->height=n->height; s->texture=n->texture; s->framebuffer=n->framebuffer; s->submitted=0;
    n->pbuffer=EGL_NO_SURFACE; n->image=0; n->ahb=0; n->wayland=0; n->buffer=0; n->sync=0; n->texture=0; n->framebuffer=0;
    mv_surface_lock_release();
    if(old_image){typedef EGLBoolean(*f)(EGLDisplay,void*);f x=(f)mv_egl_ext("eglDestroyImageKHR");if(x)x(s->display,old_image);}
    if(old_ahb){typedef void(*f)(void*);f x=(f)mv_android_symbol("AHardwareBuffer_release");if(x)x(old_ahb);}
    if(old_pbuffer){typedef EGLBoolean(*f)(EGLDisplay,EGLSurface);f x=(f)mv_resolve("eglDestroySurface",MV_EGL);if(x)x(s->display,old_pbuffer);}
    if(old_buffer)mv_wl_buffer_destroy(old_buffer); if(old_sync)mv_wl_sync_surface_destroy(old_sync); if(old_wayland)mv_wl_dmabuf_destroy(old_wayland);
    mv_surface_unpublish(n); mv_surface_put(n); return 1;
}
static EGLBoolean mv_eglDestroySurface(EGLDisplay dpy,EGLSurface handle) {
    struct mv_egl_surface *s=mv_surface_find(handle);
    if (!s) { struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope))return EGL_FALSE;
        typedef EGLBoolean (*fn_t)(EGLDisplay,EGLSurface); fn_t fn=(fn_t)mv_resolve("eglDestroySurface",MV_EGL); EGLBoolean r=fn?fn(dpy,handle):EGL_FALSE; switch_back(&scope); return r; }
    if(s->submitted)while(!mv_wl_buffer_released(s->buffer))if(mv_wl_dmabuf_dispatch_one(s->wayland)<0){mv_surface_put(s);return EGL_FALSE;}
    if (!mv_surface_unpublish(s)) { mv_surface_put(s); return EGL_FALSE; }
    /* Unpublishing drops the table reference; this operation drops its lookup
     * reference. Active swaps/queries keep the resources alive until their puts. */
    mv_surface_put(s);
    return EGL_TRUE;
}
static EGLBoolean mv_eglMakeCurrent(EGLDisplay dpy,EGLSurface draw,EGLSurface read,EGLContext ctx) {
    struct mv_egl_surface *ds=mv_surface_find(draw),*rs=mv_surface_find(read);
    EGLBoolean result=EGL_FALSE;
    if((!ds||mv_surface_resize(ds)) && (!rs||rs==ds||mv_surface_resize(rs))) {
        struct mv_scope scope;
        if(mv_ready()&&mv_enter(&scope)) {
            typedef EGLBoolean (*fn_t)(EGLDisplay,EGLSurface,EGLSurface,EGLContext); fn_t fn=(fn_t)mv_resolve("eglMakeCurrent",MV_EGL);
            result=fn?fn(dpy,ds?ds->pbuffer:draw,rs?rs->pbuffer:read,ctx):EGL_FALSE; switch_back(&scope);
        }
    }
    mv_surface_put(ds); if(rs!=ds)mv_surface_put(rs);
    return result;
}
static EGLSurface mv_eglGetCurrentSurface(EGLint which) {
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope))return EGL_NO_SURFACE;
    typedef EGLSurface (*fn_t)(EGLint); fn_t fn=(fn_t)mv_resolve("eglGetCurrentSurface",MV_EGL);
    EGLSurface current=fn?fn(which):EGL_NO_SURFACE; switch_back(&scope);
    EGLSurface result=current;
    mv_surface_lock_acquire();
    for(struct mv_egl_surface *s=mv_surfaces;s;s=s->next) if(s->pbuffer==current) { result=s->handle; break; }
    mv_surface_lock_release();
    return result;
}
static EGLBoolean mv_eglSwapBuffers_owned(EGLDisplay dpy, struct mv_egl_surface *s) {
    if((s->width!=s->window->width||s->height!=s->window->height)&&!mv_surface_resize(s))return EGL_FALSE;
    if (s->submitted) {
        while (!mv_wl_buffer_released(s->buffer)) if (mv_wl_dmabuf_dispatch_one(s->wayland)<0) return EGL_FALSE;
        if (s->swap_interval>0 && mv_wl_sync_wait_frame(s->sync)<0) return EGL_FALSE;
        mv_wl_buffer_clear_released(s->buffer);
    }
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope))return EGL_FALSE;
    typedef EGLBoolean (*swap_fn)(EGLDisplay,EGLSurface); swap_fn swap=(swap_fn)mv_resolve("eglSwapBuffers",MV_EGL);
    EGLBoolean ok=swap?swap(dpy,s->pbuffer):EGL_FALSE;
    typedef void (*bind_fn)(GLenum,GLuint); typedef void (*gen_fn)(GLsizei,GLuint *); typedef void (*image_target_fn)(GLenum,void *);
    typedef void (*get_fn)(GLenum,GLint *); typedef void (*blit_fn)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
    bind_fn bind=(bind_fn)mv_current_gl_resolve("glBindFramebuffer"); gen_fn gen=(gen_fn)mv_current_gl_resolve("glGenTextures");
    image_target_fn target=(image_target_fn)mv_egl_ext("glEGLImageTargetTexture2DOES"); get_fn get=(get_fn)mv_current_gl_resolve("glGetIntegerv");
    blit_fn blit=(blit_fn)mv_current_gl_resolve("glBlitFramebuffer");
    typedef void (*genfb_fn)(GLsizei,GLuint *); genfb_fn genfb=(genfb_fn)mv_current_gl_resolve("glGenFramebuffers");
    typedef void (*bindtex_fn)(GLenum,GLuint); bindtex_fn bindtex=(bindtex_fn)mv_current_gl_resolve("glBindTexture");
    typedef void (*attach_fn)(GLenum,GLenum,GLenum,GLuint,GLint); attach_fn attach=(attach_fn)mv_current_gl_resolve("glFramebufferTexture2D");
    typedef GLenum (*check_fn)(GLenum); check_fn check=(check_fn)mv_current_gl_resolve("glCheckFramebufferStatus");
    if(ok && gen && target && get && bindtex) {
        if(!s->texture){GLint oldtex=0;get(0x8069,&oldtex);gen(1,&s->texture);bindtex(0x0DE1,s->texture);target(0x0DE1,s->image);if(genfb)genfb(1,&s->framebuffer);bindtex(0x0DE1,(GLuint)oldtex);}
        if(blit&&bind&&genfb&&attach&&check&&s->framebuffer) {
            GLint oldr=0,oldd=0;get(GL_READ_FRAMEBUFFER_BINDING,&oldr);get(GL_DRAW_FRAMEBUFFER_BINDING,&oldd);
            bind(GL_DRAW_FRAMEBUFFER,s->framebuffer);attach(GL_DRAW_FRAMEBUFFER,0x8CE0,0x0DE1,s->texture,0);
            if(check(GL_DRAW_FRAMEBUFFER)!=0x8CD5)ok=EGL_FALSE;
            if(ok){bind(GL_READ_FRAMEBUFFER,0);blit(0,0,s->width,s->height,0,0,s->width,s->height,0x4000,0x2600);}
            bind(GL_READ_FRAMEBUFFER,(GLuint)oldr);bind(GL_DRAW_FRAMEBUFFER,(GLuint)oldd);
        } else {
            typedef void (*copy_fn)(GLenum,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei);
            copy_fn copy=(copy_fn)mv_current_gl_resolve("glCopyTexSubImage2D");
            typedef void (*bindfb_fn)(GLenum,GLuint); bindfb_fn bindfb=(bindfb_fn)mv_current_gl_resolve("glBindFramebuffer");
            GLint oldfb=0,oldtex=0;get(0x8CA6,&oldfb);get(0x8069,&oldtex);
            if(copy&&bindfb){bindfb(0x8D40,0);bindtex(0x0DE1,s->texture);copy(0x0DE1,0,0,0,0,0,s->width,s->height);bindtex(0x0DE1,(GLuint)oldtex);bindfb(0x8D40,(GLuint)oldfb);}
            else ok=EGL_FALSE;
        }
    } else ok=EGL_FALSE;
    typedef void *(*create_sync_fn)(EGLDisplay,EGLenum,const EGLint *);
    typedef EGLint (*dup_fence_fn)(EGLDisplay,void *);
    typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay,void *);
    create_sync_fn create_sync=(create_sync_fn)mv_egl_ext("eglCreateSyncKHR");
    dup_fence_fn dup_fence=(dup_fence_fn)mv_egl_ext("eglDupNativeFenceFDANDROID");
    destroy_sync_fn destroy_sync=(destroy_sync_fn)mv_egl_ext("eglDestroySyncKHR");
    typedef void (*flush_gl_fn)(void); flush_gl_fn flush_gl=(flush_gl_fn)mv_current_gl_resolve("glFlush");
    int fence=-1;
    if(ok && create_sync && dup_fence && destroy_sync && flush_gl) {
        EGLint sync_attrs[]={0x3145,-1,EGL_NONE};
        flush_gl(); void *sync=create_sync(dpy,0x3144,sync_attrs);
        if(sync){fence=dup_fence(dpy,sync);destroy_sync(dpy,sync);if(fence<0)fence=-1;}
    }
    if(ok && fence<0) {
        typedef void (*finish_fn)(void);finish_fn finish=(finish_fn)mv_current_gl_resolve("glFinish");
        if(finish)finish();else ok=EGL_FALSE;
    }
    switch_back(&scope);
    if (ok && mv_wl_surface_present(s->sync,s->window->surface,s->buffer,fence)==0) s->submitted=1;
    else ok=EGL_FALSE;
    if(fence>=0)mv_close(fence);
    return ok;
}
static EGLBoolean mv_eglSwapBuffers(EGLDisplay dpy,EGLSurface handle) {
    struct mv_egl_surface *s=mv_surface_find(handle);
    if(s) { EGLBoolean r=mv_eglSwapBuffers_owned(dpy,s); mv_surface_put(s); return r; }
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope))return EGL_FALSE;
    typedef EGLBoolean (*swap_fn)(EGLDisplay,EGLSurface); swap_fn swap=(swap_fn)mv_resolve("eglSwapBuffers",MV_EGL);
    EGLBoolean result=swap?swap(dpy,handle):EGL_FALSE; switch_back(&scope); return result;
}
static EGLBoolean mv_eglSwapInterval(EGLDisplay dpy,EGLint interval) {
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope))return EGL_FALSE;
    typedef EGLBoolean (*fn_t)(EGLDisplay,EGLint); fn_t fn=(fn_t)mv_resolve("eglSwapInterval",MV_EGL);
    typedef EGLSurface (*get_surface_fn)(EGLint); get_surface_fn get_surface=(get_surface_fn)mv_resolve("eglGetCurrentSurface",MV_EGL);
    EGLBoolean r=fn?fn(dpy,interval):EGL_FALSE; EGLSurface current=get_surface?get_surface(EGL_DRAW):EGL_NO_SURFACE; switch_back(&scope);
    struct mv_egl_surface *found=0;
    if(r) { mv_surface_lock_acquire(); for(struct mv_egl_surface *s=mv_surfaces;s;s=s->next) if(s->pbuffer==current){++s->refs;found=s;break;} mv_surface_lock_release(); }
    if(found){found->swap_interval=interval;mv_surface_put(found);} return r;
}
static EGLBoolean mv_eglQuerySurface(EGLDisplay dpy,EGLSurface handle,EGLint attr,EGLint *value) {
    struct mv_egl_surface *s=mv_surface_find(handle);
    if(s && value && (attr==EGL_WIDTH||attr==EGL_HEIGHT)) { *value=attr==EGL_WIDTH?s->width:s->height; mv_surface_put(s); return EGL_TRUE; }
    EGLSurface native=s?s->pbuffer:handle;
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope)){mv_surface_put(s);return EGL_FALSE;}
    typedef EGLBoolean (*fn_t)(EGLDisplay,EGLSurface,EGLint,EGLint *); fn_t fn=(fn_t)mv_resolve("eglQuerySurface",MV_EGL);
    EGLBoolean r=fn?fn(dpy,native,attr,value):EGL_FALSE; switch_back(&scope); mv_surface_put(s); return r;
}
static EGLBoolean mv_eglSurfaceAttrib(EGLDisplay dpy,EGLSurface handle,EGLint attr,EGLint value) {
    struct mv_egl_surface *s=mv_surface_find(handle); EGLSurface native=s?s->pbuffer:handle;
    struct mv_scope scope; if(!mv_ready()||!mv_enter(&scope)){mv_surface_put(s);return EGL_FALSE;}
    typedef EGLBoolean (*fn_t)(EGLDisplay,EGLSurface,EGLint,EGLint); fn_t fn=(fn_t)mv_resolve("eglSurfaceAttrib",MV_EGL);
    EGLBoolean r=fn?fn(dpy,native,attr,value):EGL_FALSE; switch_back(&scope); mv_surface_put(s); return r;
}

/* Android's EGL is used as the implementation, while the surfaceless
 * platform is a Linux-side property of this vendor adapter. Preserve the
 * Android extension strings and advertise the platform we map to its default
 * display. The strings are copied before restoring Linux TLS. */
static char mv_extension_string[2][16384];
static int mv_extension_lock[2];
#include "egl_extensions.inc"
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
static char mv_extension_blocklist[8192];
static int mv_extension_probe_state;
static char mv_extension_probe_key[1024];
static char mv_extension_cache_path[512];
static size_t mv_extension_len(const char *s){size_t n=0;while(s[n])n++;return n;}
static void mv_extension_paths(char *key,size_t cap) {
    if(!mv_property_get){
        if(!mv_libc)mv_libc=mv_bridge->open(MV_ANDROID_LIBDIR "libc.so",2);
        if(mv_libc)mv_property_get=(int(*)(const char*,char*))mv_bridge->symbol(mv_libc,"__system_property_get");
    }
    const char *props[]={"ro.build.fingerprint","vendor.maton.graphics.backend","vendor.maton.graphics.renderer","vendor.maton.graphics.egl","vendor.maton.graphics.gles","vendor.maton.graphics.vulkan"};
    size_t n=0;
    for(size_t i=0;i<sizeof(props)/sizeof(props[0]);i++){
        char value[192]={0};if(mv_property_get)mv_property_get(props[i],value);
        for(size_t j=0;props[i][j]&&n+1<cap;j++)key[n++]=props[i][j];
        if(n+1<cap)key[n++]='=';
        for(size_t j=0;value[j]&&n+1<cap;j++)key[n++]=value[j];
        if(n+1<cap)key[n++]=';';
    }
    key[n]=0;
    if(mv_extension_cache_path[0])return;
    const char *dir=mv_env("MATON_EGL_CACHE");if(!dir||!dir[0])return;
    static const char suffix[]="/matonos-egl-extensions.cache";size_t d=0;
    while(dir[d]&&d+sizeof(suffix)<sizeof(mv_extension_cache_path)){mv_extension_cache_path[d]=dir[d];d++;}
    if(dir[d]){mv_extension_cache_path[0]=0;return;}
    for(size_t i=0;i<sizeof(suffix);i++)mv_extension_cache_path[d+i]=suffix[i];
}
static void mv_extension_probe(const char *advertised) {
    char key[sizeof(mv_extension_probe_key)];mv_extension_paths(key,sizeof(key));
    for(;;){
        int state=__atomic_load_n(&mv_extension_probe_state,__ATOMIC_ACQUIRE);
        if(state==2&&!strcmp(key,mv_extension_probe_key))return;
        if(state==1)continue;
        int expected=state;
        if(__atomic_compare_exchange_n(&mv_extension_probe_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
    }
    mv_extension_blocklist[0]=0;
    if(mv_extension_cache_path[0]){
        char data[9216];size_t n=0;long fd=mv_open(mv_extension_cache_path);
        if(fd>=0){while(n+1<sizeof(data)){long r=mv_read(fd,data+n,sizeof(data)-1-n);if(r<=0)break;n+=(size_t)r;}mv_close(fd);data[n]=0;
            size_t k=0;while(k<n&&data[k]!='\n')k++;
            if(k==mv_extension_len(key)&&!memcmp(key,data,k)){size_t q=k+1;for(size_t i=q;i<n&&i-q+1<sizeof(mv_extension_blocklist);i++)mv_extension_blocklist[i-q]=data[i];mv_extension_blocklist[n-q]=0;memcpy(mv_extension_probe_key,key,mv_extension_len(key)+1);__atomic_store_n(&mv_extension_probe_state,2,__ATOMIC_RELEASE);return;}
        }
    }
    for(size_t i=0;i<MV_EGL_EXTENSION_COUNT;i++){
        const struct mv_egl_extension_desc *e=&mv_egl_extensions[i];
        if(e->excluded||!mv_has_token(advertised,e->name))continue;
        char command[160];size_t n=0;int failed=0;
        for(const char *p=e->commands;;p++){
            if(*p&&*p!=' '){if(n+1<sizeof(command))command[n++]=*p;continue;}
            if(n){command[n]=0;if(!mv_android_egl_get_proc||!mv_android_egl_get_proc(command))failed=1;n=0;}
            if(!*p)break;
        }
        if(failed){size_t used=mv_extension_len(mv_extension_blocklist),len=mv_extension_len(e->name);if(used+len+2<sizeof(mv_extension_blocklist)){if(used)mv_extension_blocklist[used++]=' ';memcpy(mv_extension_blocklist+used,e->name,len+1);}}
    }
    memcpy(mv_extension_probe_key,key,mv_extension_len(key)+1);
    if(mv_extension_cache_path[0]){
        char data[9216];size_t n=0;for(size_t i=0;key[i]&&n+1<sizeof(data);i++)data[n++]=key[i];data[n++]='\n';
        for(size_t i=0;mv_extension_blocklist[i]&&n+1<sizeof(data);i++)data[n++]=mv_extension_blocklist[i];
        long out=mv_open_write(mv_extension_cache_path,0600);
        if(out>=0){mv_write_fd(out,data,n);mv_close(out);}
    }
    __atomic_store_n(&mv_extension_probe_state,2,__ATOMIC_RELEASE);
}
static int mv_extension_hidden(const char *token) {
    for(size_t i=0;i<MV_EGL_EXTENSION_COUNT;i++)if(!strcmp(token,mv_egl_extensions[i].name))return mv_egl_extensions[i].excluded||mv_has_token(mv_extension_blocklist,token);
    return 0;
}
static EGLBoolean mv_filter_extensions(char *dst,const char *src) {
    size_t used=0;const char *p=src;
    while(*p){while(*p==' ')p++;const char *start=p;while(*p&&*p!=' ')p++;size_t len=(size_t)(p-start);if(!len)break;
        char token[256];if(len>=sizeof(token))continue;memcpy(token,start,len);token[len]=0;if(mv_extension_hidden(token))continue;
        if(used&&used+1<sizeof(mv_extension_string[0]))dst[used++]=' ';
        if(used+len>=sizeof(mv_extension_string[0]))return EGL_FALSE;memcpy(dst+used,token,len);used+=len;dst[used]=0;
    }
    return EGL_TRUE;
}
static const char *mv_eglQueryString(EGLDisplay dpy, EGLint name) {
    struct mv_scope scope;
    if (!mv_ready() || !mv_enter(&scope)) { mv_fail(); return 0; }
    if (name == EGL_CLIENT_APIS) {
        static char client_apis[] = "OpenGL_ES OpenGL";
        int desktop = mv_desktop_api_supported();
        switch_back(&scope);
        return desktop ? client_apis : "OpenGL_ES";
    }
    typedef const char *(*fn_t)(EGLDisplay, EGLint);
    fn_t fn = (fn_t)mv_resolve("eglQueryString", MV_EGL);
    const char *result = fn ? fn(dpy, name) : 0;
    if (name != EGL_EXTENSIONS || !result) { switch_back(&scope); return result; }
    int index = dpy == EGL_NO_DISPLAY ? 0 : 1;
    while (__atomic_exchange_n(&mv_extension_lock[index], 1, __ATOMIC_ACQUIRE)) {}
    mv_extension_probe(result);
    EGLBoolean ok = mv_filter_extensions(mv_extension_string[index], result);
    if(ok)ok = mv_append_token(mv_extension_string[index], mv_extension_string[index], "EGL_MESA_platform_surfaceless");
    if(ok) ok=mv_append_token(mv_extension_string[index], mv_extension_string[index], "EGL_KHR_platform_wayland");
    if(ok) ok=mv_append_token(mv_extension_string[index], mv_extension_string[index], "EGL_EXT_platform_wayland");
    switch_back(&scope);
    __atomic_store_n(&mv_extension_lock[index], 0, __ATOMIC_RELEASE);
    return ok ? mv_extension_string[index] : 0;
}
static __eglMustCastToProperFunctionPointerType mv_eglGetProcAddress(const char *name) {
    return (__eglMustCastToProperFunctionPointerType)mv_lookup_proc(name);
}
#include "proc_lookup.inc"
#include "desktop_proc_lookup.inc"
static void *mv_direct_get_proc(const char *name) {
    void *proc = mv_lookup_desktop_proc(name);
    return proc ? proc : (void *)mv_eglGetProcAddress(name);
}
static EGLBoolean mv_direct_bind_api(EGLenum api) {
    if (api == EGL_OPENGL_API) {
        struct mv_scope scope;
        if (!mv_ready() || !mv_enter(&scope)) return EGL_FALSE;
        int supported = mv_desktop_api_supported();
        switch_back(&scope);
        if (!supported) return EGL_FALSE;
    }
    return mv_eglBindAPI(api);
}

/*
 * GLX enters through libGLdispatch, which owns its make-current transition.
 * Its vendor must bypass libEGL's GLVND front end to avoid a nested dispatch
 * make-current. This private table shares the Maton EGL vendor's TLS-aware
 * wrappers with libGLX_maton.
 */
EXPORT const struct maton_egl_direct_v1 *maton_egl_direct_v1(void) {
    static const struct maton_egl_direct_v1 api = {
        .version = 1,
        .get_platform_display = mv_eglGetPlatformDisplay,
        .get_display = mv_eglGetDisplay,
        .initialize = mv_eglInitialize,
        .bind_api = mv_direct_bind_api,
        .query_api = mv_eglQueryAPI,
        .choose_config = mv_eglChooseConfig,
        .create_context = mv_eglCreateContext,
        .create_pbuffer_surface = mv_eglCreatePbufferSurface,
        .make_current = mv_eglMakeCurrent,
        .destroy_surface = mv_eglDestroySurface,
        .destroy_context = mv_eglDestroyContext,
        .get_proc_address = mv_direct_get_proc,
    };
    return &api;
}

static EGLDisplay mv_vendor_get_platform_display(EGLenum platform, void *native, const EGLAttrib *attrs) {
    if (platform == EGL_PLATFORM_WAYLAND_KHR || platform == EGL_PLATFORM_WAYLAND_EXT) {
        (void)native; (void)attrs;
        return mv_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
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
    if (api == EGL_OPENGL_API) {
        struct mv_scope scope;
        if (!mv_ready() || !mv_enter(&scope)) return EGL_FALSE;
        int supported = mv_desktop_api_supported();
        switch_back(&scope);
        return supported ? EGL_TRUE : EGL_FALSE;
    }
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
