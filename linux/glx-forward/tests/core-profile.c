#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 1
#endif

typedef GLXContext (*CreateAttribs)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void (*SwapIntervalEXT)(Display *, GLXDrawable, int);
typedef int (*GetSwapIntervalMESA)(void);
typedef int (*SwapIntervalMESA)(unsigned int);
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

int main(void) {
    Display *d = XOpenDisplay(NULL);
    CHECK(d, "XOpenDisplay failed");
    int major = 0, minor = 0;
    CHECK(glXQueryVersion(d, &major, &minor) && major == 1 && minor >= 4,
          "GLX version is %d.%d", major, minor);
    const char *ext = glXQueryExtensionsString(d, DefaultScreen(d));
    CHECK(ext && strstr(ext, "GLX_ARB_create_context") && strstr(ext, "GLX_ARB_create_context_profile") &&
          strstr(ext, "GLX_EXT_swap_control") && strstr(ext, "GLX_MESA_swap_control"), "required GLX extensions absent");
    int attrs[] = { GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                    GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                    GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None };
    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(d, DefaultScreen(d), attrs, &count);
    CHECK(configs && count, "no window FBConfig");
    XVisualInfo *vi = glXGetVisualFromFBConfig(d, configs[0]);
    CHECK(vi, "no visual");
    Colormap cm = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes wa = { .colormap = cm };
    Window w = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 96, 96, 0, vi->depth,
                             InputOutput, vi->visual, CWColormap, &wa);
    XMapWindow(d, w); XSync(d, False);
    CreateAttribs create = (CreateAttribs)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    CHECK(create, "ARB context function missing");
    int ca[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                 GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None };
    GLXContext ctx = create(d, configs[0], NULL, True, ca);
    CHECK(ctx && glXMakeCurrent(d, w, ctx), "3.3 core context creation failed");
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    CHECK(version && strstr(version, "Core") && glsl, "context is not core profile: %s", version ? version : "null");
    SwapIntervalEXT set_ext = (SwapIntervalEXT)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    SwapIntervalMESA set_mesa = (SwapIntervalMESA)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    GetSwapIntervalMESA get_mesa = (GetSwapIntervalMESA)glXGetProcAddressARB((const GLubyte *)"glXGetSwapIntervalMESA");
    CHECK(set_ext && set_mesa && get_mesa, "swap control functions missing");
    CHECK(set_mesa(0) == 0 && get_mesa() == 0, "swap interval 0 set/get failed");
    CHECK(set_mesa(1) == 0 && get_mesa() == 1, "swap interval 1 set/get failed");

    const char *vs = "#version 330 core\nlayout(location=0) in vec2 p; void main(){gl_Position=vec4(p,0,1);}";
    const char *fs = "#version 330 core\nout vec4 color; void main(){color=vec4(1,0,0,1);}";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v,1,&vs,NULL); glCompileShader(v); glShaderSource(f,1,&fs,NULL); glCompileShader(f);
    GLint ok=0; glGetShaderiv(v,GL_COMPILE_STATUS,&ok); CHECK(ok,"vertex shader compile failed");
    glGetShaderiv(f,GL_COMPILE_STATUS,&ok); CHECK(ok,"fragment shader compile failed");
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p); glGetProgramiv(p,GL_LINK_STATUS,&ok); CHECK(ok,"shader link failed"); glUseProgram(p);
    const GLfloat verts[]={-.8f,-.8f,.8f,-.8f,0,.8f}; GLuint vao=0,vbo=0;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao); glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    glViewport(0,0,96,96); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES,0,3);
    GLubyte pixel[4]={0}; glReadPixels(48,48,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    CHECK(pixel[0]>200 && pixel[1]<20 && pixel[2]<20, "triangle pixel failed: %u,%u,%u",pixel[0],pixel[1],pixel[2]);
    glXSwapBuffers(d,w); printf("PASS GLX %s; core triangle pixel %u,%u,%u\n",version,pixel[0],pixel[1],pixel[2]);
    glXMakeCurrent(d,None,NULL); glXDestroyContext(d,ctx); XDestroyWindow(d,w); XFreeColormap(d,cm); XFree(vi); XFree(configs); XCloseDisplay(d); return 0;
}
