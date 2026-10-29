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
    assert(glXMakeCurrent(d,0,0));glXDestroyContext(d,ctx);
    int pbattrs[]={GLX_DRAWABLE_TYPE,GLX_PBUFFER_BIT,0},np=0;
    GLXFBConfig *pc=glXChooseFBConfig(d,screen,pbattrs,&np);assert(pc&&np);
    assert(strstr(glXQueryExtensionsString(d,screen),"GLX_EXT_create_context_es2_profile"));
    for(int version=0;version<=3;version++){
        if(version==1)continue; /* Host has GLES2/3, no GLES1 resident implementation. */
        int size[]={GLX_PBUFFER_WIDTH,3,GLX_PBUFFER_HEIGHT,2,GLX_PRESERVED_CONTENTS,1,0};
        GLXPbuffer pb=glXCreatePbuffer(d,pc[0],size);assert(pb);
        unsigned int width=0,height=0;glXQueryDrawable(d,pb,GLX_WIDTH,&width);glXQueryDrawable(d,pb,GLX_HEIGHT,&height);assert(width==3&&height==2);
        int ca[]={GLX_CONTEXT_MAJOR_VERSION_ARB,version,GLX_CONTEXT_PROFILE_MASK_ARB,GLX_CONTEXT_ES_PROFILE_BIT_EXT,0};
        ctx=version?glXCreateContextAttribsARB(d,pc[0],0,1,ca):glXCreateNewContext(d,pc[0],0x8014,0,1);
        assert(ctx&&glXMakeCurrent(d,pb,ctx));
        const unsigned char *(*getstring)(unsigned)=(void *)getproc((const GLubyte *)"glGetString");
        if(version)assert(strstr((const char *)getstring(GL_VERSION),"OpenGL ES"));
        void (*readpixel)(int,int,int,int,unsigned,unsigned,void*)=(void *)getproc((const GLubyte *)"glReadPixels");
        unsigned char pixel[4]={0};color(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);readpixel(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]<10&&pixel[1]>245&&pixel[2]<10);
        glXSwapBuffers(d,pb); /* Offscreen drawables never enter XGetWindowAttributes. */
        assert(glXMakeCurrent(d,window,ctx));assert(glXMakeCurrent(d,pb,ctx));
        memset(pixel,0,4);readpixel(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>245);
        glXDestroyPbuffer(d,pb); /* Still current: destruction must be deferred. */
        memset(pixel,0,4);readpixel(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>245);
        assert(glXMakeCurrent(d,0,0));glXDestroyContext(d,ctx);
    }
    free(pc);free(all);XDestroyWindow(d,window);XCloseDisplay(d);
    puts("PASS GLX real visuals/window swap; desktop + ES2/ES3 pbuffer pixels, dimensions, persistence and current destruction");
}
