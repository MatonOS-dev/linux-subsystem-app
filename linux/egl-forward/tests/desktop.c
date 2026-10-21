#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#ifndef APIENTRY
#define APIENTRY
#endif

#define EGL_API(ret, name, args) static ret (*p_##name) args
EGL_API(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *));
EGL_API(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
EGL_API(const char *, eglQueryString, (EGLDisplay, EGLint));
EGL_API(EGLBoolean, eglBindAPI, (EGLenum));
EGL_API(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
EGL_API(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *));
EGL_API(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
EGL_API(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
EGL_API(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext));
EGL_API(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface));
EGL_API(EGLBoolean, eglTerminate, (EGLDisplay));
EGL_API(EGLint, eglGetError, (void));
EGL_API(__eglMustCastToProperFunctionPointerType, eglGetProcAddress, (const char *));
#define eglGetPlatformDisplay p_eglGetPlatformDisplay
#define eglInitialize p_eglInitialize
#define eglQueryString p_eglQueryString
#define eglBindAPI p_eglBindAPI
#define eglChooseConfig p_eglChooseConfig
#define eglCreatePbufferSurface p_eglCreatePbufferSurface
#define eglCreateContext p_eglCreateContext
#define eglMakeCurrent p_eglMakeCurrent
#define eglDestroyContext p_eglDestroyContext
#define eglDestroySurface p_eglDestroySurface
#define eglTerminate p_eglTerminate
#define eglGetError p_eglGetError
#define eglGetProcAddress p_eglGetProcAddress

static int load_egl(void) {
    void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "cannot load EGL: %s\n", dlerror()); return 0; }
#define LOAD_EGL(name) do { *(void **)(&p_##name) = dlsym(lib, #name); if (!p_##name) return 0; } while (0)
    LOAD_EGL(eglGetPlatformDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglQueryString);
    LOAD_EGL(eglBindAPI); LOAD_EGL(eglChooseConfig); LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglCreateContext); LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglDestroySurface); LOAD_EGL(eglTerminate); LOAD_EGL(eglGetError);
    LOAD_EGL(eglGetProcAddress);
#undef LOAD_EGL
    return 1;
}

typedef void (APIENTRY *begin_fn)(GLenum);
typedef void (APIENTRY *end_fn)(void);
typedef void (APIENTRY *vertex3f_fn)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *color3f_fn)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *read_pixels_fn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (APIENTRY *viewport_fn)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *clear_color_fn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *clear_fn)(GLbitfield);
typedef const GLubyte *(APIENTRY *get_string_fn)(GLenum);

static int unsupported_expected(void) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, 0, 0);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "EGL initialization failed: %#x\n", eglGetError());
        return 1;
    }
    EGLBoolean bound = eglBindAPI(EGL_OPENGL_API);
    EGLint error = eglGetError();
    eglTerminate(display);
    if (bound) {
        fputs("desktop GL unexpectedly supported\n", stderr);
        return 1;
    }
    printf("PASS: desktop GL unsupported cleanly (egl error=%#x)\n", error);
    return 0;
}

int main(int argc, char **argv) {
    if (!load_egl()) return 2;
    if (argc == 2 && !strcmp(argv[1], "--expect-unsupported"))
        return unsupported_expected();

    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, 0, 0);
    EGLint major = 0, minor = 0, count = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) goto fail;
    printf("EGL %d.%d vendor=%s APIs=%s\n", major, minor,
           eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_CLIENT_APIS));
    if (!eglBindAPI(EGL_OPENGL_API)) goto fail;

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || !count) goto fail;
    const EGLint surface_attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    const EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 2,
        EGL_CONTEXT_MINOR_VERSION_KHR, 1,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) goto fail;

    begin_fn glBegin_ = (begin_fn)eglGetProcAddress("glBegin");
    end_fn glEnd_ = (end_fn)eglGetProcAddress("glEnd");
    vertex3f_fn glVertex3f_ = (vertex3f_fn)eglGetProcAddress("glVertex3f");
    color3f_fn glColor3f_ = (color3f_fn)eglGetProcAddress("glColor3f");
    read_pixels_fn glReadPixels_ = (read_pixels_fn)eglGetProcAddress("glReadPixels");
    viewport_fn glViewport_ = (viewport_fn)eglGetProcAddress("glViewport");
    clear_color_fn glClearColor_ = (clear_color_fn)eglGetProcAddress("glClearColor");
    clear_fn glClear_ = (clear_fn)eglGetProcAddress("glClear");
    get_string_fn glGetString_ = (get_string_fn)eglGetProcAddress("glGetString");
    if (!glBegin_ || !glEnd_ || !glVertex3f_ || !glColor3f_ || !glReadPixels_ ||
        !glViewport_ || !glClearColor_ || !glClear_ || !glGetString_) {
        fputs("desktop GL entry point missing\n", stderr);
        goto fail;
    }
    glViewport_(0, 0, 64, 64);
    glClearColor_(0, 0, 0, 1);
    glClear_(GL_COLOR_BUFFER_BIT);
    glBegin_(GL_TRIANGLES);
    glColor3f_(1, 0, 0); glVertex3f_(-0.8f, -0.8f, 0);
    glColor3f_(1, 0, 0); glVertex3f_( 0.8f, -0.8f, 0);
    glColor3f_(1, 0, 0); glVertex3f_( 0.0f,  0.8f, 0);
    glEnd_();
    GLubyte pixel[4] = {0};
    glReadPixels_(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("GL version=%s renderer=%s center=%u,%u,%u,%u\n",
           (const char *)glGetString_(GL_VERSION), (const char *)glGetString_(GL_RENDERER),
           pixel[0], pixel[1], pixel[2], pixel[3]);
    if (pixel[0] < 200 || pixel[1] > 30 || pixel[2] > 30) goto fail;

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    puts("PASS: surfaceless compatibility-profile desktop GL triangle readback");
    return 0;
fail:
    fprintf(stderr, "desktop EGL/GL failure: egl=%#x\n", eglGetError());
    return 1;
}
