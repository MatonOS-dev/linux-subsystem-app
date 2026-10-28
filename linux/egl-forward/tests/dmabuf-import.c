#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
/* Run with Maton first and Mesa second in GLVND's vendor list. Mesa owns
 * this GBM display; Maton's extension dispatch must still select Mesa. */
int main(int argc,char **argv) {
    int fd=open(argc>1?argv[1]:"/dev/dri/renderD128",O_RDWR|O_CLOEXEC);
    if(fd<0){perror("render node unavailable");return 77;}
    struct gbm_device *gbm=gbm_create_device(fd); assert(gbm);
    EGLDisplay d=eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,gbm,0); assert(d);
    assert(eglInitialize(d,0,0)); assert(eglBindAPI(EGL_OPENGL_ES_API));
    PFNEGLQUERYDMABUFFORMATSEXTPROC formats=(void *)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    EGLint count=0;assert(formats && formats(d,0,0,&count) && count>0);
    EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE}; EGLConfig config; EGLint n;
    assert(eglChooseConfig(d,ca,&config,1,&n)&&n);
    EGLint ctxa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,ctxa);assert(ctx);
    assert(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx));
    struct gbm_bo *bo=gbm_bo_create(gbm,16,16,GBM_FORMAT_ARGB8888,GBM_BO_USE_RENDERING|GBM_BO_USE_LINEAR);assert(bo);
    int dma=gbm_bo_get_fd(bo);assert(dma>=0);
    EGLint attrs[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_LINUX_DRM_FOURCC_EXT,GBM_FORMAT_ARGB8888,EGL_DMA_BUF_PLANE0_FD_EXT,dma,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)gbm_bo_get_stride(bo),EGL_NONE};
    PFNEGLCREATEIMAGEKHRPROC create=(void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy=(void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target=(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES"); assert(create&&destroy&&target);
    EGLImageKHR im=create(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,0,attrs);assert(im);
    GLuint tex,fb;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);target(GL_TEXTURE_2D,im);assert(glGetError()==GL_NO_ERROR);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);unsigned char pixel[4]={0};glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(glGetError()==GL_NO_ERROR&&pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
    glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&tex);assert(destroy(d,im));close(dma);gbm_bo_destroy(bo);
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,ctx);eglTerminate(d);gbm_device_destroy(gbm);close(fd);
    puts("PASS: GBM dma-buf EGLImage texture framebuffer readback");return 0;
}
