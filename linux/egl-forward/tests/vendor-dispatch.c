/* Exercise the actual generated GLVND stubs without an Android bridge. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glvnd/libeglabi.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
static int initialized, called, last, error;
static __EGLvendorInfo *owner = (__EGLvendorInfo *)0x1234;
static void init(void) { ++initialized; }
static __EGLvendorInfo *vendor(EGLDisplay d) { return d == (EGLDisplay)0x5678 ? owner : 0; }
static void set_error(EGLint e) { error=e; }
static EGLBoolean set_last(__EGLvendorInfo *v) { assert(v==owner); ++last; return EGL_TRUE; }
static EGLImageKHR image(EGLDisplay d,EGLContext c,EGLenum t,EGLClientBuffer b,const EGLint *a) {
    assert(d==(EGLDisplay)0x5678 && c==EGL_NO_CONTEXT && t==EGL_LINUX_DMA_BUF_EXT && !b && a[0]==EGL_NONE);
    ++called; return (EGLImageKHR)0x9876;
}
static __eglMustCastToProperFunctionPointerType fetch(__EGLvendorInfo *v,int index) {
    assert(v==owner && index==42); return (__eglMustCastToProperFunctionPointerType)image;
}
int main(int argc,char **argv) {
    assert(argc==2);
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(lib);
    __PFNEGLMAINPROC main_fn=(__PFNEGLMAINPROC)dlsym(lib,"__egl_Main"); assert(main_fn);
    __EGLapiExports exports={.threadInit=init,.getVendorFromDisplay=vendor,.fetchDispatchEntry=fetch,.setLastVendor=set_last,.setEGLError=set_error};
    __EGLapiImports imports={0}; assert(main_fn(EGL_VENDOR_ABI_VERSION,&exports,0,&imports));
    imports.setDispatchIndex("eglCreateImageKHR",42);
    PFNEGLCREATEIMAGEKHRPROC create=(PFNEGLCREATEIMAGEKHRPROC)imports.getDispatchAddress("eglCreateImageKHR"); assert(create);
    EGLint attrs[]={EGL_NONE};
    assert(create((EGLDisplay)0x5678,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,0,attrs)==(EGLImageKHR)0x9876);
    assert(initialized==1 && last==1 && called==1);
    assert(!create(EGL_NO_DISPLAY,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,0,attrs));
    assert(error==EGL_BAD_DISPLAY && initialized==2 && called==1);
    assert(!imports.getDispatchAddress("glBindTexture"));
    puts("PASS: EGL image extension dispatch selects display owner and preserves error routing");
}
