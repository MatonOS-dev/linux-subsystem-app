/* Window swaps hand the EGL X11 surface to the forwarder's zero-copy swap;
 * no readback or XPutImage. Pbuffers and stale drawables never present. */
#include "../vendor.c"
#include <assert.h>
#include <stdio.h>
static int swaps;static EGLSurface swapped;
static EGLBoolean fake_swap(EGLDisplay d,EGLSurface s){(void)d;swaps++;swapped=s;return EGL_TRUE;}
int main(void){
    static struct maton_egl_direct_v1 api={.version=2,.swap_buffers=fake_swap};egl_direct=&api;
    struct ctx c={.width=2,.height=2,.surface=(EGLSurface)7,.drawable=1};current=(GLXContext)&c;
    glXSwapBuffers((Display*)1,1);assert(swaps==1&&swapped==(EGLSurface)7);
    glXSwapBuffers((Display*)1,2);assert(swaps==1);
    struct pbuffer pb={0};c.pbuffer=&pb;glXSwapBuffers((Display*)1,1);assert(swaps==1);
    puts("PASS GLX window swap presents the EGL X11 surface without readback");return 0;
}
