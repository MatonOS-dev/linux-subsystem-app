#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

static void rect(float x0, float x1, float z) {
    glBegin(GL_QUADS);
    glVertex3f(x0, -1, z); glVertex3f(x1, -1, z);
    glVertex3f(x1, 1, z); glVertex3f(x0, 1, z);
    glEnd();
}

static int pixel(int x, int expect_red, const char *what) {
    GLubyte p[4] = {0};
    glReadPixels(x, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    int pass = expect_red ? p[0] > 220 && p[1] < 20 : p[1] > 220 && p[0] < 20;
    printf("%s: rgba=%u,%u,%u,%u %s\n", what, p[0], p[1], p[2], p[3], pass ? "PASS" : "FAIL");
    return pass;
}

int main(void) {
    Display *d = XOpenDisplay(NULL);
    CHECK(d, "XOpenDisplay failed");
    int s = DefaultScreen(d);
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, None};
    XVisualInfo *v = glXChooseVisual(d, s, attrs);
    CHECK(v, "no depth/stencil GLX visual");
    int depth = -1, stencil = -1;
    CHECK(glXGetConfig(d, v, GLX_DEPTH_SIZE, &depth) == 0 &&
          glXGetConfig(d, v, GLX_STENCIL_SIZE, &stencil) == 0 && depth >= 24 && stencil >= 8,
          "GLX reported depth/stencil %d/%d", depth, stencil);
    XSetWindowAttributes wa = {.colormap = XCreateColormap(d, RootWindow(d, s), v->visual, AllocNone)};
    Window w = XCreateWindow(d, RootWindow(d, s), 0, 0, 128, 64, 0, v->depth, InputOutput,
                             v->visual, CWColormap, &wa);
    XMapWindow(d, w); XSync(d, False);
    GLXContext c = glXCreateContext(d, v, NULL, True);
    CHECK(c && glXMakeCurrent(d, w, c), "GLX context setup failed");
    GLint db = -1, sb = -1;
    glGetIntegerv(GL_DEPTH_BITS, &db); glGetIntegerv(GL_STENCIL_BITS, &sb);
    printf("GLX requested=24/8, reported=%d/%d, actual framebuffer=%d/%d\n", depth, stencil, db, sb);
    CHECK(db >= 24 && sb >= 8, "default framebuffer lacks depth/stencil");

    glViewport(0, 0, 128, 64); glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1); glClearDepth(1); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glColor3f(0, 1, 0); rect(-1, 0, -0.5f);
    glColor3f(1, 0, 0); rect(-1, 0, 0.5f); /* farther fragment must fail */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST); glStencilFunc(GL_EQUAL, 0, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 1, 0); rect(0, 1, 0);
    glStencilFunc(GL_EQUAL, 1, 0xff); glColor3f(1, 0, 0); rect(0, 1, 0);
    glFinish();
    CHECK(pixel(32, 0, "depth test") && pixel(96, 0, "stencil test"), "depth/stencil pixel test failed");
    puts("PASS depth and stencil tests on GLX default framebuffer");
    glXMakeCurrent(d, None, NULL); glXDestroyContext(d, c); XDestroyWindow(d, w); XFreeColormap(d, wa.colormap); XFree(v); XCloseDisplay(d);
    return 0;
}
