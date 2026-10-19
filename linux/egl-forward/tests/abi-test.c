#include <EGL/egl.h>
#include <glvnd/libeglabi.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    __PFNEGLMAINPROC main_fn = (__PFNEGLMAINPROC)dlsym(lib, "__egl_Main");
    if (!main_fn) return 1;
    __EGLapiImports imports = {0};
    if (!main_fn(EGL_VENDOR_ABI_VERSION, 0, 0, &imports)) return 1;
    if (!imports.getPlatformDisplay || !imports.getSupportsAPI || !imports.getProcAddress ||
        !imports.getDispatchAddress || !imports.setDispatchIndex) return 1;
    const char *windows[]={"eglCreatePlatformWindowSurface","eglCreateWindowSurface",
        "eglSwapBuffers","eglDestroySurface","eglMakeCurrent","eglGetCurrentSurface"};
    for (unsigned i=0;i<sizeof(windows)/sizeof(windows[0]);++i)
        if (!imports.getProcAddress(windows[i])) { fprintf(stderr,"missing EGL window entry point: %s\n",windows[i]); return 1; }
    if (main_fn(0x00010000, 0, 0, &imports)) return 1;
    puts("PASS: GLVND ABI handshake and required imports");
    return 0;
}
