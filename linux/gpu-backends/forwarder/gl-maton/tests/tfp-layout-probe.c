/* tfp-layout-probe: does an X pixmap (drawn by Xwayland/glamor) read back
 * correctly through Zink's DRI3 import (GLX_EXT_texture_from_pixmap)?
 * Writes a per-pixel unique pattern with XPutImage, binds the pixmap as a
 * texture, draws it into an FBO and compares. Exit 0 = layout matches.
 * Usage: tfp-layout-probe [width height] ; log goes to stdout and
 * $HOME/tfp-layout-probe.txt (for runs launched from Steam). */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *logf;
static void say(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (logf) { va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a); fflush(logf); }
}
static uint32_t pattern(uint32_t x, uint32_t y) { /* 0xAARRGGBB */
    return 0xff000000u | ((((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4)) << 16) | ((y & 0xff) << 8) | (x & 0xff);
}

int main(int argc, char **argv) {
    char path[4096]; const char *home = getenv("HOME");
    if (home) { snprintf(path, sizeof path, "%s/tfp-layout-probe.txt", home); logf = fopen(path, "w"); }
    unsigned w = argc > 2 ? (unsigned)atoi(argv[1]) : 1280, h = argc > 2 ? (unsigned)atoi(argv[2]) : 800;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { say("FAIL XOpenDisplay\n"); return 2; }
    int scr = DefaultScreen(dpy);
    int fba[] = {GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT | GLX_WINDOW_BIT, GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
                 GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
                 GLX_DEPTH_SIZE, 0, None};
    int n = 0; GLXFBConfig *cfgs = glXChooseFBConfig(dpy, scr, fba, &n);
    GLXFBConfig cfg = 0; XVisualInfo *vi = NULL;
    for (int i = 0; i < n && !cfg; i++) {
        XVisualInfo *v = glXGetVisualFromFBConfig(dpy, cfgs[i]);
        if (v && v->depth == 32) { cfg = cfgs[i]; vi = v; } else if (v) XFree(v);
    }
    if (!cfg) { say("FAIL no depth-32 TFP fbconfig (%d candidates)\n", n); return 2; }

    /* Pixmap with the pattern, drawn by the X server (glamor). */
    Window root = RootWindow(dpy, scr);
    Pixmap pm = XCreatePixmap(dpy, root, w, h, 32);
    uint32_t *px = malloc((size_t)w * h * 4);
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) px[y * w + x] = pattern(x, y);
    XImage *img = XCreateImage(dpy, vi->visual, 32, ZPixmap, 0, (char *)px, w, h, 32, w * 4);
    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XPutImage(dpy, pm, gc, img, 0, 0, 0, 0, w, h);
    XSync(dpy, False);

    /* GL context on a small window, as an app would have. */
    Colormap cm = XCreateColormap(dpy, root, vi->visual, AllocNone);
    XSetWindowAttributes swa = {.colormap = cm, .border_pixel = 0, .background_pixel = 0};
    Window win = XCreateWindow(dpy, root, 0, 0, 64, 64, 0, 32, InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel, &swa);
    GLXContext ctx = glXCreateNewContext(dpy, cfg, GLX_RGBA_TYPE, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) { say("FAIL GLX context\n"); return 2; }
    say("gl: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    PFNGLXBINDTEXIMAGEEXTPROC bind = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
    if (!bind) { say("FAIL no glXBindTexImageEXT\n"); return 2; }
    int pa[] = {GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT, GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT, None};
    GLXPixmap gp = glXCreatePixmap(dpy, cfg, pm, pa);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    bind(dpy, gp, GLX_FRONT_LEFT_EXT, NULL);

    /* Read the texture back with the GPU (FBO + blit), no CPU guesses. */
    GLuint fbo, rbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo); glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { say("FAIL fbo\n"); return 2; }
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glEnable(GL_TEXTURE_2D); glDisable(GL_BLEND);
    glBegin(GL_QUADS); /* texel (x,y) -> fbo pixel (x,y): row 0 of the pixmap lands in fbo row 0 */
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
    uint8_t *rb = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, rb);
    GLenum err = glGetError();
    if (err) { say("FAIL GL error 0x%x\n", err); return 2; }

    uint64_t bad = 0; long first = -1;
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t got; memcpy(&got, rb + (size_t)i * 4, 4);
        if ((got & 0xffffff) != (pattern(i % w, i / w) & 0xffffff)) { bad++; if (first < 0) first = i; }
    }
    if (bad) {
        uint32_t got; memcpy(&got, rb + (size_t)first * 4, 4);
        uint32_t gx = (got & 0xff) | (((got >> 16) & 0xf) << 8), gy = ((got >> 8) & 0xff) | (((got >> 20) & 0xf) << 8);
        say("RESULT tfp %ux%u: MISMATCH %llu/%u pixels; first at (%ld,%ld) holds the pixel written to (%u,%u)\n", w, h,
            (unsigned long long)bad, w * h, first % w, first / w, gx, gy);
        return 1;
    }
    say("RESULT tfp %ux%u: MATCH all %u pixels\n", w, h, w * h);
    return 0;
}
