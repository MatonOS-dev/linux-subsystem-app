/* Surfaceless EGL: desktop core/compat and GLES FBO readback. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (EGL=%x GL=%x)\n",__FILE__,__LINE__,#x,eglGetError(),glGetError()); exit(1); } } while (0)
static GLuint shader(GLenum type,const char *src)
{
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s,sizeof(log),NULL,log); fprintf(stderr,"%s\n",log); }
    REQUIRE(ok); return s;
}
static void render(int es)
{
    const char *vs=es ? "#version 300 es\nconst vec2 v[3]=vec2[3](vec2(-.75,-.75),vec2(.75,-.75),vec2(0,.75));void main(){gl_Position=vec4(v[gl_VertexID],0,1);}" : "#version 330 core\nconst vec2 v[3]=vec2[3](vec2(-.75,-.75),vec2(.75,-.75),vec2(0,.75));void main(){gl_Position=vec4(v[gl_VertexID],0,1);}";
    const char *fs=es ? "#version 300 es\nprecision highp float;out vec4 color;void main(){color=vec4(1,0,0,1);}" : "#version 330 core\nout vec4 color;void main(){color=vec4(1,0,0,1);}";
    GLuint v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs), p=glCreateProgram();
    glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
    GLint ok; glGetProgramiv(p,GL_LINK_STATUS,&ok);REQUIRE(ok);glUseProgram(p);
    GLuint tex,fb,vao;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,64,64);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glViewport(0,0,64,64);
    glDisable(GL_DITHER);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    REQUIRE(glGetError()==GL_NO_ERROR);
    unsigned red=0;uint64_t sum=0,hash=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<4096;i++) {
        REQUIRE(pixels[4*i+1]==0 && pixels[4*i+2]==0 && pixels[4*i+3]==255);
        REQUIRE(pixels[4*i]==0 || pixels[4*i]==255);red+=pixels[4*i]==255;
    }
    for(unsigned i=0;i<sizeof(pixels);i++){sum+=pixels[i];hash=(hash^pixels[i])*UINT64_C(1099511628211);}
    REQUIRE(red==1152 && sum==1338240);
    printf("PASS %s FBO triangle: red=%u sum=%llu FNV1a=%016llx\n",es?"GLES":"desktop GL",red,(unsigned long long)sum,(unsigned long long)hash);
    glDeleteVertexArrays(1,&vao);glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&tex);
    glDeleteProgram(p);glDeleteShader(v);glDeleteShader(f);
}
int main(int argc,char **argv)
{
    int es=argc>1 && !strcmp(argv[1],"es"), compat=argc>1 && !strcmp(argv[1],"compat");
    EGLDisplay d=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
    REQUIRE(d!=EGL_NO_DISPLAY);EGLint major,minor;REQUIRE(eglInitialize(d,&major,&minor));
    printf("EGL %d.%d vendor=%s\n",major,minor,eglQueryString(d,EGL_VENDOR));
    REQUIRE(eglBindAPI(es?EGL_OPENGL_ES_API:EGL_OPENGL_API));
    EGLint attrs[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,es?0:3,EGL_CONTEXT_OPENGL_PROFILE_MASK,compat?EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT:EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    if(es) attrs[4]=EGL_NONE;
    EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attrs);REQUIRE(c!=EGL_NO_CONTEXT);
    REQUIRE(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c));
    printf("GL_VERSION=%s\nGL_RENDERER=%s\nGLSL_VERSION=%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER),glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint glmajor,glminor;glGetIntegerv(GL_MAJOR_VERSION,&glmajor);glGetIntegerv(GL_MINOR_VERSION,&glminor);
    REQUIRE(glmajor>=3 && (es || glmajor>3 || glminor>=3));render(es);
    REQUIRE(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));REQUIRE(eglDestroyContext(d,c));REQUIRE(eglTerminate(d));return 0;
}
