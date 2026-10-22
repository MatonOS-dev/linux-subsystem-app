// Alpha probe: which alpha paths survive the GL passthrough? Reads the back
// buffer before swap, so presentation is not involved.
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

static void quad(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y0);
    glTexCoord2f(1, 1); glVertex2f(x1, y1);
    glTexCoord2f(0, 1); glVertex2f(x0, y1);
    glEnd();
}

static void probe(const char *name, int x, int y, const char *expect) {
    unsigned char p[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    printf("%-28s rgba=%3u,%3u,%3u,%3u  expect %s  err=%x\n", name, p[0], p[1], p[2], p[3], expect,
           glGetError());
}

static GLuint tex(GLint ifmt, GLenum fmt, const unsigned char *px) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 1, 1, 0, fmt, GL_UNSIGNED_BYTE, px);
    return t;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *names[] = {"glBlendFuncSeparate", "glBlendFuncSeparateEXT", "glBlendEquation",
                           "glBlendEquationEXT", "glBlendEquationSeparate", "glBlendEquationSeparateEXT"};
    void *before_current[6];
    for (int i = 0; i < 6; ++i)
        before_current[i] = (void *)glXGetProcAddress((const GLubyte *)names[i]);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fputs("XOpenDisplay failed\n", stderr); return 2; }
    int s = DefaultScreen(d);
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                   GLX_ALPHA_SIZE, 8, None};
    XVisualInfo *v = glXChooseVisual(d, s, attrs);
    if (!v) { fputs("glXChooseVisual failed\n", stderr); return 3; }
    XSetWindowAttributes wa = {.colormap = XCreateColormap(d, RootWindow(d, s), v->visual, AllocNone)};
    Window w = XCreateWindow(d, RootWindow(d, s), 40, 40, 160, 32, 0, v->depth, InputOutput, v->visual,
                             CWColormap, &wa);
    XMapWindow(d, w);
    XSync(d, False);
    GLXContext c = glXCreateContext(d, v, NULL, True);
    if (!c || !glXMakeCurrent(d, w, c)) { fputs("context failed\n", stderr); return 4; }
    printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    GLint abits = -1;
    glGetIntegerv(GL_ALPHA_BITS, &abits);
    printf("GL_ALPHA_BITS=%d\n", abits);

    // 5 cells of 32x32 across a 160x32 viewport; NDC x per cell = 0.4.
    glViewport(0, 0, 160, 32);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // 0: vertex-colour blend, no texture.
    glColor4f(1, 0, 0, 0.5f);
    quad(-1.0f, -1, -0.6f, 1);

    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    // 1: RGBA texel, alpha 0 -> background stays blue.
    const unsigned char rgba0[4] = {255, 255, 255, 0};
    tex(GL_RGBA, GL_RGBA, rgba0);
    quad(-0.6f, -1, -0.2f, 1);
    // 2: GL_ALPHA texel alpha 0 -> blue.
    const unsigned char a0[1] = {0};
    tex(GL_ALPHA, GL_ALPHA, a0);
    quad(-0.2f, -1, 0.2f, 1);
    // 3: GL_LUMINANCE_ALPHA white, alpha 0 -> blue.
    const unsigned char la0[2] = {255, 0};
    tex(GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, la0);
    quad(0.2f, -1, 0.6f, 1);
    // 4: GL_ALPHA alpha 255, colour red -> red.
    const unsigned char a1[1] = {255};
    tex(GL_ALPHA, GL_ALPHA, a1);
    glColor4f(1, 0, 0, 1);
    quad(0.6f, -1, 1.0f, 1);
    glFinish();
    probe("vertex alpha 0.5 blend", 16, 16, "~128,0,127");
    probe("RGBA tex a=0", 48, 16, "0,0,255");
    probe("GL_ALPHA tex a=0", 80, 16, "0,0,255");
    probe("GL_LUMINANCE_ALPHA a=0", 112, 16, "0,0,255");
    probe("GL_ALPHA tex a=255 red", 144, 16, "255,0,0");

    // SFML-style paths on a 2x1 texture: left texel transparent, right opaque green.
    // Each samples the left (transparent) texel, so blue must survive.
    const unsigned char two[8] = {255, 255, 255, 0, 0, 255, 0, 255};
    tex(GL_RGBA, GL_RGBA, NULL);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, two);
    glColor4f(1, 1, 1, 1);
    glViewport(0, 0, 160, 32);
    glClear(GL_COLOR_BUFFER_BIT);

    // 5: interleaved client arrays like sf::Vertex {float x,y; u8 rgba[4]; float u,v}.
    struct V { float x, y; unsigned char c[4]; float u, v; } va[4] = {
        {-1.0f, -1, {255, 255, 255, 255}, 0.1f, 0.5f}, {-0.6f, -1, {255, 255, 255, 255}, 0.4f, 0.5f},
        {-0.6f, 1, {255, 255, 255, 255}, 0.4f, 0.5f},  {-1.0f, 1, {255, 255, 255, 255}, 0.1f, 0.5f}};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(struct V), &va[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct V), va[0].c);
    glTexCoordPointer(2, GL_FLOAT, sizeof(struct V), &va[0].u);
    glDrawArrays(GL_QUADS, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // 6: texture matrix scaling pixel coords (u in [0,2) texels) like SFML Pixels mode.
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(0.5f, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0.2f, 0.5f); glVertex2f(-0.6f, -1);
    glTexCoord2f(0.8f, 0.5f); glVertex2f(-0.2f, -1);
    glTexCoord2f(0.8f, 0.5f); glVertex2f(-0.2f, 1);
    glTexCoord2f(0.2f, 0.5f); glVertex2f(-0.6f, 1);
    glEnd();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

    // 7: glTexSubImage2D replaces the right texel with transparent too; sample right.
    const unsigned char clear1[4] = {255, 255, 255, 0};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, clear1);
    glBegin(GL_QUADS);
    glTexCoord2f(0.6f, 0.5f); glVertex2f(-0.2f, -1);
    glTexCoord2f(0.9f, 0.5f); glVertex2f(0.2f, -1);
    glTexCoord2f(0.9f, 0.5f); glVertex2f(0.2f, 1);
    glTexCoord2f(0.6f, 0.5f); glVertex2f(-0.2f, 1);
    glEnd();
    glFinish();
    // 8-10: blend set only through extension entry points SFML loads by name.
    for (int i = 0; i < 6; i++)
        printf("proc %-28s before=%p after=%p\n", names[i], before_current[i],
               (void *)glXGetProcAddress((const GLubyte *)names[i]));
    typedef void (*bfs_t)(GLenum, GLenum, GLenum, GLenum);
    typedef void (*be_t)(GLenum);
    typedef void (*bes_t)(GLenum, GLenum);
    const int blend_cases[][2] = {{0, 2}, {1, 3}, {0, 4}, {1, 5}};
    int failures = 0;
    for (int i = 0; i < 4; i++) {
        bfs_t bfs = (bfs_t)glXGetProcAddress((const GLubyte *)names[blend_cases[i][0]]);
        void *eq = glXGetProcAddress((const GLubyte *)names[blend_cases[i][1]]);
        int separate = blend_cases[i][1] >= 4;
        be_t be = separate ? NULL : (be_t)eq;
        bes_t bes = separate ? (bes_t)eq : NULL;
        if (!bfs || (!be && !bes) || !before_current[blend_cases[i][0]] || !before_current[blend_cases[i][1]]) {
            printf("FAIL: missing proc pair %s / %s\n", names[blend_cases[i][0]], names[blend_cases[i][1]]);
            failures++;
            continue;
        }
        glBlendFunc(GL_ONE, GL_ZERO);
        printf("calling %s + %s\n", names[blend_cases[i][0]], names[blend_cases[i][1]]);
        bfs(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        puts("calling blend equation");
        if (bes) bes(0x8006 /* GL_FUNC_ADD */, 0x8006);
        else be(0x8006 /* GL_FUNC_ADD */);
        GLint src = 0;
        glGetIntegerv(0x80C9 /* GL_BLEND_SRC_RGB */, &src);
        float x0 = 0.2f + 0.4f * i;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                        (const unsigned char[8]){255, 255, 255, 0, 255, 255, 255, 0});
        glBegin(GL_QUADS);
        glTexCoord2f(0.1f, 0.5f); glVertex2f(x0, -1);
        glTexCoord2f(0.4f, 0.5f); glVertex2f(x0 + 0.4f, -1);
        glTexCoord2f(0.4f, 0.5f); glVertex2f(x0 + 0.4f, 1);
        glTexCoord2f(0.1f, 0.5f); glVertex2f(x0, 1);
        glEnd();
        glFinish();
        printf("%s + %s: GL_BLEND_SRC_RGB=0x%x (want 0x302)\n", names[blend_cases[i][0]],
               names[blend_cases[i][1]], src);
        probe(names[blend_cases[i][0]], 16 + 32 * i, 16, "0,0,255");
        if (src != GL_SRC_ALPHA || glGetError() != GL_NO_ERROR) failures++;
    }
    probe("client arrays sf::Vertex", 16, 16, "0,0,255");
    probe("texture matrix scale", 48, 16, "0,0,255");
    probe("glTexSubImage2D", 80, 16, "0,0,255");
    glXMakeCurrent(d, None, NULL);
    glXDestroyContext(d, c);
    XCloseDisplay(d);
    return failures ? 1 : 0;
}
