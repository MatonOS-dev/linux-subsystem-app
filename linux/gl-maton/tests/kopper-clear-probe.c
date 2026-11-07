/* kopper-clear-probe: does a GL frame rendered through Zink+Kopper reach
 * Xwayland? Clears a window to a known colour, checks the back buffer with
 * glReadPixels before each swap, presents 300 frames. Compare with Xwayland's
 * maton-present2 line for the printed window id.
 * Log: stdout and $HOME/kopper-clear-probe.txt. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *logf;
static void say(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (logf) { va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a); fflush(logf); }
}

int main(void) {
    char path[4096]; const char *home = getenv("HOME");
    if (home) { snprintf(path, sizeof path, "%s/kopper-clear-probe.txt", home); logf = fopen(path, "w"); }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { say("FAIL XOpenDisplay\n"); return 2; }
    int attr[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr);
    if (!vi) { say("FAIL glXChooseVisual\n"); return 2; }
    Window root = RootWindow(dpy, vi->screen);
    XSetWindowAttributes swa = {.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone), .border_pixel = 0};
    unsigned w = 640, h = 400;
    Window win = XCreateWindow(dpy, root, 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
    XMapWindow(dpy, win); XSync(dpy, False);
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) { say("FAIL GLX context\n"); return 2; }
    say("gl: %s / %s window=0x%lx depth=%d\n", (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION), (unsigned long)win, vi->depth);
    unsigned char px[4];
    for (int i = 0; i < 300; i++) {
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        glClearColor(0.2f, 0.4f, 0.8f, 1.0f); /* expect rgb 33 66 cc */
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glReadBuffer(GL_BACK);
        glReadPixels((GLint)w / 2, (GLint)h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (i == 0 || i == 299) say("frame %d back buffer centre rgba=%02x%02x%02x%02x glerr=0x%x\n", i, px[0], px[1], px[2], px[3], glGetError());
        glXSwapBuffers(dpy, win);
    }
    glFinish();
    say("RESULT presented 300 frames to window 0x%lx; check maton-present2\n", (unsigned long)win);
    return 0;
}
