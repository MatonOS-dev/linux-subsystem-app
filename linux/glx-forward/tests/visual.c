/* Reproduce ANGLE DisplayGLX's requested-visual lookup on a real X server,
 * then render with the actual vendor through a host Mesa direct EGL bridge. */
#ifndef MATON_GLX_VENDOR_SOURCE
#define MATON_GLX_VENDOR_SOURCE "../vendor.c"
#endif
#include MATON_GLX_VENDOR_SOURCE
#include <assert.h>
#include <stdio.h>
#ifndef GLX_X_VISUAL_TYPE
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRUE_COLOR 0x8002
#endif
extern int XFree(void *);
extern Display *XOpenDisplay(const char *);
extern unsigned long XDefaultRootWindow(Display *);
extern unsigned long XCreateSimpleWindow(Display *,unsigned long,int,int,unsigned,unsigned,unsigned,unsigned long,unsigned long);
extern int XMapWindow(Display *,unsigned long),XSync(Display *,int),XDestroyWindow(Display *,unsigned long),XCloseDisplay(Display *);
extern XImage *XGetImage(Display *,unsigned long,int,int,unsigned,unsigned,unsigned long,int);
extern unsigned long XGetPixel(XImage *,int,int);
int main(void){
    Display *d=XOpenDisplay(0);assert(d);int screen=XDefaultScreen(d);
    int count=0;int attrs[]={0};GLXFBConfig *all=glXChooseFBConfig(d,screen,attrs,&count);assert(all&&count>0);
    unsigned long requested=XDefaultVisual(d,screen)->visualid;GLXFBConfig chosen=0;
    int depth24=0,depth32=0;
    for(int i=0;i<count;i++){
        int id=0,type=0;assert(!glXGetFBConfigAttrib(d,all[i],GLX_VISUAL_ID,&id)&&id);
        assert(!glXGetFBConfigAttrib(d,all[i],GLX_X_VISUAL_TYPE,&type)&&type==GLX_TRUE_COLOR);
        XVisualInfo *v=glXGetVisualFromFBConfig(d,all[i]);assert(v&&v->visualid==(unsigned long)id&&v->screen==screen&&v->c_class==4);
        depth24+=v->depth==24;depth32+=v->depth==32;XFree(v);
        if((unsigned long)id==requested)chosen=all[i];
    }
    assert(chosen&&depth24&&depth32); /* Xvfb exposes both depths. */
    unsigned long window=XCreateSimpleWindow(d,XDefaultRootWindow(d),0,0,64,64,0,0,0);assert(window);XMapWindow(d,window);XSync(d,0);
    assert(glXCreateWindow(d,chosen,window,0)==window);
    GLXContext ctx=glXCreateNewContext(d,chosen,0x8014,0,1);assert(ctx&&glXMakeCurrent(d,window,ctx));
    void (*color)(float,float,float,float)=(void *)getproc((const GLubyte *)"glClearColor");
    void (*clear)(unsigned)=(void *)getproc((const GLubyte *)"glClear");assert(color&&clear);color(1,0,0,1);clear(0x4000);
    glXSwapBuffers(d,window);XSync(d,0);XImage *image=XGetImage(d,window,0,0,64,64,~0UL,2);assert(image);
    assert((XGetPixel(image,32,32)&0xffffff)==0xff0000);XDestroyImage(image);
    assert(glXMakeCurrent(d,0,0));glXDestroyContext(d,ctx);free(all);XDestroyWindow(d,window);XCloseDisplay(d);
    puts("PASS ANGLE requested GLX visual lookup: real 24/32-bit visuals; EGL context/current/swap -> X11 red pixel");
}
