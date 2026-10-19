#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gpu-forward-spike/bootstrap.h"
#include "../../gpu-forward-spike/bridge.h"
#include "../../vulkan-forward/arch/x86_64/backend.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

static struct adopt_thread thread;
static struct adopt_api adoption;
static void *open_vendor(const char *path, int flags) {
    (void)flags;
    return dlopen(strstr(path, "libGLESv2") ? "libGLESv2.so.2" : "libEGL_mesa.so.0", RTLD_NOW | RTLD_LOCAL);
}
static void *lookup_symbol(void *handle, const char *name) { return dlsym(handle, name); }
static struct adopt_thread *current_thread(void) { return &thread; }
static void publish_bootstrap(void) {
    struct gpu_bridge *bridge = calloc(1, sizeof(*bridge));
    bridge->version = 2;
    bridge->error = NULL;
    bridge->open = open_vendor;
    bridge->symbol = lookup_symbol;
    adoption.version = 1;
    adoption.current = current_thread;
    unsigned long fs = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_FS, &fs) != 0) abort();
    thread.resident_fs = fs;
    struct maton_gpu_bootstrap *b = mmap(0, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) abort();
    b->magic = MATON_GPU_BOOTSTRAP_MAGIC;
    snprintf(b->bridge, sizeof(b->bridge), "%lx", (unsigned long)(uintptr_t)bridge);
    snprintf(b->adopt, sizeof(b->adopt), "%lx", (unsigned long)(uintptr_t)&adoption);
    strcpy(b->fast, "0");
    if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)b, 4096, "maton-gpu-bootstrap") != 0) abort();
    if (mprotect(b, 4096, PROT_READ) != 0) abort();
}

int main(void) {
    publish_bootstrap();
    const char *stage = "get display";
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, 0, 0);
    EGLint major = 0, minor = 0, count = 0;
    stage = "initialize";
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) goto fail;
    printf("EGL vendor=%s version=%s APIs=%s\n", eglQueryString(d, EGL_VENDOR),
           eglQueryString(d, EGL_VERSION), eglQueryString(d, EGL_CLIENT_APIS));
    stage = "bind API";
    if (!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config;
    stage = "choose config";
    if (!eglChooseConfig(d, ca, &config, 1, &count) || !count) goto fail;
    EGLint pa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(d, config, pa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(d, config, EGL_NO_CONTEXT, xa);
    stage = "create pbuffer/context/make current";
    if (!surface || !context || !eglMakeCurrent(d, surface, surface, context)) goto fail;
    const char *vs = "attribute vec2 p; void main(){gl_Position=vec4(p,0.0,1.0);}";
    const char *fs = "precision mediump float; void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    stage = "compile GLES shaders";
    glShaderSource(v, 1, &vs, 0); glShaderSource(f, 1, &fs, 0); glCompileShader(v); glCompileShader(f);
    GLint ok = 0; glGetShaderiv(v, GL_COMPILE_STATUS, &ok); if (!ok) goto fail;
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok); if (!ok) goto fail;
    GLuint program = glCreateProgram(); glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok); if (!ok) goto fail;
    stage = "draw triangle";
    const GLfloat triangle[] = {-0.8f,-0.8f, 0.8f,-0.8f, 0.0f,0.8f};
    glViewport(0, 0, 64, 64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program); GLint loc = glGetAttribLocation(program, "p");
    glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, triangle); glEnableVertexAttribArray((GLuint)loc);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("GL version=%s renderer=%s center=%u,%u,%u,%u\n",
           glGetString(GL_VERSION), glGetString(GL_RENDERER), pixel[0], pixel[1], pixel[2], pixel[3]);
    if (pixel[0] < 200 || pixel[1] > 30 || pixel[2] > 30) return 1;
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, context); eglDestroySurface(d, surface); eglTerminate(d);
    puts("PASS: surfaceless pbuffer GLES triangle readback");
    return 0;
fail:
    fprintf(stderr, "EGL/GLES failure at %s: egl=%#x\n", stage, eglGetError());
    return 1;
}
