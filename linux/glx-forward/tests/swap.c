/* Exercise presentation without a server or GPU. In particular, readback must
 * use the vendor thunk, rather than an uninitialized front-end GL dispatch. */
#define XGetWindowAttributes fake_attributes
#define XCreateImage fake_image
#define XPutPixel fake_pixel
#define XCreateGC fake_gc
#define XPutImage fake_upload
#define XDestroyImage fake_destroy
#define XFreeGC fake_free_gc
#define XFlush fake_flush
#include "../vendor.c"
#include <assert.h>
#include <stdio.h>
static Visual visual={.red_mask=0xff0000,.green_mask=0xff00,.blue_mask=0xff};
static unsigned long pixels[4];
static int finished,packed,uploaded,flushed;
static void finish(void){finished++;}
static void pack(GLenum a,GLint b){assert(a==GL_PACK_ALIGNMENT&&b==1);packed++;}
static void read_pixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum t,void*p){
    assert(finished&&packed&&x==0&&y==0&&w==2&&h==2&&f==BGRA&&t==GL_UNSIGNED_BYTE);
    const unsigned char data[]={0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255};
    memcpy(p,data,sizeof(data));
}
static __eglMustCastToProperFunctionPointerType proc(const char*n){
    if(!strcmp(n,"glFinish"))return (__eglMustCastToProperFunctionPointerType)finish;
    if(!strcmp(n,"glPixelStorei"))return (__eglMustCastToProperFunctionPointerType)pack;
    if(!strcmp(n,"glReadPixels"))return (__eglMustCastToProperFunctionPointerType)read_pixels;
    return 0;
}
int fake_attributes(Display*d,unsigned long w,XWindowAttributes*a){(void)d;(void)w;memset(a,0,sizeof(*a));a->width=a->height=2;a->depth=24;a->visual=&visual;return 1;}
XImage *fake_image(Display*d,Visual*v,unsigned int depth,int format,int offset,char*data,unsigned int w,unsigned int h,int pad,int stride){
    (void)d;(void)v;(void)offset;(void)data;(void)pad;(void)stride;assert(depth==24&&format==2&&w==2&&h==2);
    XImage*i=calloc(1,sizeof(*i));i->width=i->height=2;i->bytes_per_line=8;return i;
}
int fake_pixel(XImage*i,int x,int y,unsigned long p){(void)i;pixels[y*2+x]=p;return 1;}
void *fake_gc(Display*d,unsigned long w,unsigned long m,void*v){(void)d;(void)w;(void)m;(void)v;return (void*)1;}
int fake_upload(Display*d,unsigned long w,void*g,XImage*i,int sx,int sy,int dx,int dy,unsigned int width,unsigned int height){
    (void)d;(void)w;(void)g;(void)i;assert(!sx&&!sy&&!dx&&!dy&&width==2&&height==2);uploaded++;return 0;
}
int fake_destroy(XImage*i){free(i->data);free(i);return 0;}
int fake_free_gc(Display*d,void*g){(void)d;(void)g;return 0;}
int fake_flush(Display*d){(void)d;flushed++;return 0;}
int main(void){
    struct ctx c={.width=2,.height=2};current=(GLXContext)&c;p_eglGetProcAddress=proc;
    glXSwapBuffers((Display*)1,1);
    assert(uploaded==1&&flushed==1);
    glXWaitGL();assert(finished==2);
    assert(pixels[0]==0xff&&pixels[1]==0xffffff&&pixels[2]==0xff0000&&pixels[3]==0xff00);
    puts("PASS GLX swap uses vendor readback, visual masks and vertical flip");return 0;
}
