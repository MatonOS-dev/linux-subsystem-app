/* Native Android control: tests the vendor path on a real bionic thread.
 * Also builds on Linux with EGL/GLES development headers. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static int desktop;
static void *run(void *arg) {
    (void)arg;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, n;
    if (!eglInitialize(d, &major, &minor)) goto fail;
    printf("vendor=%s APIs=%s\n", eglQueryString(d,EGL_VENDOR), eglQueryString(d,EGL_CLIENT_APIS));
    if (!eglBindAPI(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API)) goto fail;
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,
        desktop ? EGL_OPENGL_BIT : EGL_OPENGL_ES2_BIT,EGL_NONE};
    EGLConfig config;
    if (!eglChooseConfig(d, attrs, &config, 1, &n) || !n) goto fail;
    EGLint size[] = {EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d,config,size);
    EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext c = eglCreateContext(d,config,EGL_NO_CONTEXT,desktop ? NULL : version);
    if (!c || !s || !eglMakeCurrent(d,s,s,c)) goto fail;
    /* Resolve through EGL so the test doesn't assume a desktop libGL soname. */
    void (*clear_color)(float,float,float,float) = (void *)eglGetProcAddress("glClearColor");
    void (*clear)(unsigned) = (void *)eglGetProcAddress("glClear");
    void (*read_pixels)(int,int,int,int,unsigned,unsigned,void*) = (void *)eglGetProcAddress("glReadPixels");
    const unsigned char *(*get_string)(unsigned) = (void *)eglGetProcAddress("glGetString");
    if (!clear_color || !clear || !read_pixels || !get_string) goto fail;
    clear_color(1,0,0,1); clear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] = {0};
    read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    printf("GL=%s renderer=%s pixel=%u,%u,%u,%u\n",get_string(GL_VERSION),get_string(GL_RENDERER),pixel[0],pixel[1],pixel[2],pixel[3]);
    int ok = pixel[0] > 240 && pixel[1] < 10 && pixel[2] < 10;
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(d,c); eglDestroySurface(d,s); eglTerminate(d); eglReleaseThread();
    return (void *)(long)!ok;
fail:
    fprintf(stderr,"EGL failure: %#x\n",eglGetError());
    return (void *)1;
}
int main(int argc, char **argv) {
    desktop = argc > 1 && !strcmp(argv[1],"desktop");
    pthread_t t; void *result;
    if (pthread_create(&t,NULL,run,NULL) || pthread_join(t,&result)) return 2;
    return (int)(long)result;
}
