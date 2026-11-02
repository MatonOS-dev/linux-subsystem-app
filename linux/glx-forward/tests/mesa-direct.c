/* Host-only resident boundary: real Mesa EGL desktop rendering, no Android TLS.
 * Xvfb has no DRI3, so this test bridge presents X11 windows by readback; the
 * product forwarder presents the same surfaces zero-copy through DRI3/Present. */
#include "maton_egl_direct.h"
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
static void *proc(const char *name){return (void *)eglGetProcAddress(name);}
struct win { EGLSurface surface; Display *display; Window window; struct win *next; };
static struct win *wins;
static EGLDisplay host_display(void){return eglGetDisplay(EGL_DEFAULT_DISPLAY);}
static EGLSurface create_window(EGLConfig config,void *display,unsigned long window){
    XWindowAttributes a;if(!XGetWindowAttributes(display,window,&a))return EGL_NO_SURFACE;
    EGLint attrs[]={EGL_WIDTH,a.width,EGL_HEIGHT,a.height,EGL_NONE};
    EGLSurface s=eglCreatePbufferSurface(host_display(),config,attrs);if(s==EGL_NO_SURFACE)return s;
    struct win *w=calloc(1,sizeof(*w));if(!w){eglDestroySurface(host_display(),s);return EGL_NO_SURFACE;}
    w->surface=s;w->display=display;w->window=window;w->next=wins;wins=w;return s;
}
static EGLBoolean swap(EGLDisplay d,EGLSurface s){
    struct win *w=wins;while(w&&w->surface!=s)w=w->next;if(!w)return EGL_FALSE;
    EGLint width=0,height=0;eglQuerySurface(d,s,EGL_WIDTH,&width);eglQuerySurface(d,s,EGL_HEIGHT,&height);
    unsigned char *p=malloc((size_t)width*height*4);if(!p)return EGL_FALSE;
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,p);
    XImage *i=XCreateImage(w->display,DefaultVisual(w->display,DefaultScreen(w->display)),24,ZPixmap,0,0,width,height,32,0);
    i->data=malloc((size_t)i->bytes_per_line*height);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){unsigned char *c=p+((size_t)(height-1-y)*width+x)*4;
        XPutPixel(i,x,y,((unsigned long)c[0]<<16)|((unsigned long)c[1]<<8)|c[2]);}
    free(p);GC gc=XCreateGC(w->display,w->window,0,0);XPutImage(w->display,w->window,gc,i,0,0,0,0,width,height);
    XFreeGC(w->display,gc);XDestroyImage(i);XFlush(w->display);return EGL_TRUE;
}
const struct maton_egl_direct_v1 *maton_egl_direct_v1(void){
    static const struct maton_egl_direct_v1 api={
        .version=2,.get_platform_display=eglGetPlatformDisplay,.get_display=eglGetDisplay,
        .initialize=eglInitialize,.bind_api=eglBindAPI,.query_api=eglQueryAPI,.choose_config=eglChooseConfig,
        .create_context=eglCreateContext,.create_pbuffer_surface=eglCreatePbufferSurface,
        .make_current=eglMakeCurrent,.destroy_surface=eglDestroySurface,.destroy_context=eglDestroyContext,
        .get_proc_address=proc,.create_x11_window_surface=create_window,.swap_buffers=swap};return &api;
}
