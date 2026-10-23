// SFML-style glyph atlas growth probe. Each cell is rendered and read back so
// upload layout, FBO blit, and framebuffer-to-texture copy failures are distinct.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*bind_fbo_fn)(GLenum, GLuint);
typedef void (*attach_tex_fn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*check_fbo_fn)(GLenum);
typedef void (*blit_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static void quad(float x0, float x1, float u0, float u1, float v0, float v1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, -1);
    glTexCoord2f(u1, v0); glVertex2f(x1, -1);
    glTexCoord2f(u1, v1); glVertex2f(x1, 1);
    glTexCoord2f(u0, v1); glVertex2f(x0, 1);
    glEnd();
}

static GLuint texture(int w, int h, GLenum internal, GLenum format) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_UNSIGNED_BYTE, NULL);
    return t;
}

static int pixel(const char *name, int x, int want_red) {
    unsigned char p[4] = {0};
    glReadPixels(x, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    GLenum e = glGetError();
    int ok = e == GL_NO_ERROR && (want_red ? p[0] > 220 && p[1] < 35 && p[2] < 35
                                             : p[1] > 220 && p[0] < 35 && p[2] < 35);
    printf("%s: %s rgba=%u,%u,%u,%u err=0x%x\n", name, ok ? "PASS" : "FAIL",
           p[0], p[1], p[2], p[3], e);
    return !ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fputs("XOpenDisplay failed\n", stderr); return 2; }
    int s = DefaultScreen(d);
    int attrs[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *v = glXChooseVisual(d, s, attrs);
    if (!v) { fputs("glXChooseVisual failed\n", stderr); return 3; }
    XSetWindowAttributes wa = {.colormap = XCreateColormap(d, RootWindow(d, s), v->visual, AllocNone)};
    Window w = XCreateWindow(d, RootWindow(d, s), 40, 40, 160, 32, 0, v->depth,
                             InputOutput, v->visual, CWColormap, &wa);
    XMapWindow(d, w); XSync(d, False);
    GLXContext c = glXCreateContext(d, v, NULL, True);
    if (!c || !glXMakeCurrent(d, w, c)) { fputs("context failed\n", stderr); return 4; }
    printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    bind_fbo_fn bind_fbo = (bind_fbo_fn)glXGetProcAddress((const GLubyte *)"glBindFramebuffer");
    attach_tex_fn attach = (attach_tex_fn)glXGetProcAddress((const GLubyte *)"glFramebufferTexture2D");
    check_fbo_fn check = (check_fbo_fn)glXGetProcAddress((const GLubyte *)"glCheckFramebufferStatus");
    blit_fn blit = (blit_fn)glXGetProcAddress((const GLubyte *)"glBlitFramebuffer");
    blit_fn blit_ext = (blit_fn)glXGetProcAddress((const GLubyte *)"glBlitFramebufferEXT");
    if (!bind_fbo || !attach || !check || !blit) {
        puts("FBO blit entrypoints unavailable");
        return 5;
    }

    // A 4x4 glyph page: opaque red mark at (1..2,1..2), surrounded by zero.
    GLuint a = texture(128, 128, GL_RGBA, GL_RGBA);
    unsigned char initial_page[128 * 128 * 4];
    for (size_t i = 0; i < sizeof(initial_page); i += 4) {
        initial_page[i] = initial_page[i + 1] = initial_page[i + 2] = 255;
        initial_page[i + 3] = 0;
    }
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            initial_page[(y * 128 + x) * 4 + 3] = 255;
    glBindTexture(GL_TEXTURE_2D, a);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, initial_page);
    const unsigned char glyph[4 * 4 * 4] = {
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 255,0,0,255, 255,0,0,255, 0,0,0,0,
        0,0,0,0, 255,0,0,255, 255,0,0,255, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    glBindTexture(GL_TEXTURE_2D, a);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, glyph);

    // Upload a second 2x2 glyph with padded row length, as glyph bitmaps may
    // have pitch larger than their visible width. Padding bytes are magenta.
    const unsigned char padded[2 * 6 * 4] = {
        0,0,0,0, 0,255,0,255, 255,0,255,255, 255,0,255,255, 255,0,255,255, 255,0,255,255,
        0,0,0,0, 0,255,0,255, 255,0,255,255, 255,0,255,255, 255,0,255,255, 255,0,255,255};
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 6);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, padded);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // SFML 2.x grayscale glyph pages use GL_ALPHA; upload with byte alignment 1.
    GLuint alpha = texture(3, 2, GL_ALPHA, GL_ALPHA);
    const unsigned char mask[6] = {0, 255, 0, 0, 255, 0};
    glBindTexture(GL_TEXTURE_2D, alpha);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 3, 2, GL_ALPHA, GL_UNSIGNED_BYTE, mask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // Grow A into larger B through the FBO path used by SFML on capable GL.
    GLuint b = texture(256, 256, GL_RGBA, GL_RGBA), read_fbo, draw_fbo;
    glGenFramebuffers(1, &read_fbo);
    glGenFramebuffers(1, &draw_fbo);
    bind_fbo(GL_READ_FRAMEBUFFER, read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0);
    GLenum read_status = check(GL_READ_FRAMEBUFFER);
    bind_fbo(GL_DRAW_FRAMEBUFFER, draw_fbo);
    attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b, 0);
    GLenum draw_status = check(GL_DRAW_FRAMEBUFFER);
    printf("FBO status read=0x%x draw=0x%x\n", read_status, draw_status);
    if (read_status != GL_FRAMEBUFFER_COMPLETE || draw_status != GL_FRAMEBUFFER_COMPLETE) return 6;
    blit(0, 0, 128, 128, 0, 0, 128, 128, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLenum blit_error = glGetError();
    GLenum ext_blit_error = GL_NO_ERROR;
    if (blit_ext) {
        blit_ext(0, 0, 128, 128, 0, 0, 128, 128, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        ext_blit_error = glGetError();
    }
    printf("glBlitFramebuffer=%p glBlitFramebufferEXT=%p\n", (void *)blit, (void *)blit_ext);

    // SFML adds newly rasterized glyphs to the grown atlas after copying the
    // old page. Upload at nonzero x/y with its exact RGBA byte format.
    const unsigned char fresh_glyph[2 * 2 * 4] = {
        0, 255, 0, 255, 0, 255, 0, 255,
        0, 255, 0, 255, 0, 255, 0, 255};
    glBindTexture(GL_TEXTURE_2D, b);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, fresh_glyph);
    GLenum grown_upload_error = glGetError();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 160, 32);
    glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, a); quad(-1, -0.5f, 2.25f/128, 2.75f/128, 1.25f/128, 2.75f/128);
    glBindTexture(GL_TEXTURE_2D, b); quad(-0.5f, 0, 2.25f/256, 2.75f/256, 1.25f/256, 2.75f/256);
    glBindTexture(GL_TEXTURE_2D, a); quad(0, 0.5f, 1.25f/128, 1.75f/128, 0.0f, 1.0f/128);
    glBindTexture(GL_TEXTURE_2D, b); quad(0.5f, 1, 4.5f/256, 5.5f/256, 1.5f/256, 2.5f/256);
    glFinish();
    int failures = pixel("original upload A", 16, 1);
    failures += pixel("FBO blit A to B", 48, 1);
    failures += pixel("padded-row GL_RGBA upload", 80, 0);
    failures += pixel("post-growth glyph upload", 144, 0);
    printf("post-growth upload error=0x%x\n", grown_upload_error);
    if (grown_upload_error != GL_NO_ERROR) failures++;
    // GL_ALPHA glyph coverage is white at its opaque texel; enable blending
    // over blue and ensure the alpha upload is visible at the sampled center.
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, alpha);
    glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    quad(-1, 0, 0.38f, 0.55f, 0.0f, 1.0f); // opaque coverage texel
    quad(0, 1, 0.0f, 0.2f, 0.0f, 1.0f);  // transparent coverage texel
    glFinish();
    unsigned char ap[4] = {0}, tp[4] = {0};
    glReadPixels(32, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ap);
    glReadPixels(96, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tp);
    GLenum alpha_error = glGetError();
    int alpha_ok = ap[0] > 220 && ap[1] > 220 && ap[2] > 220 &&
                   tp[0] < 35 && tp[1] < 35 && tp[2] > 220 && alpha_error == GL_NO_ERROR;
    printf("GL_ALPHA upload: %s opaque=%u,%u,%u,%u transparent=%u,%u,%u,%u err=0x%x\n",
           alpha_ok ? "PASS" : "FAIL", ap[0], ap[1], ap[2], ap[3],
           tp[0], tp[1], tp[2], tp[3], alpha_error);
    failures += !alpha_ok;
    printf("FBO blit error=0x%x\n", blit_error);
    printf("EXT FBO blit error=0x%x\n", ext_blit_error);
    if (blit_error != GL_NO_ERROR || ext_blit_error != GL_NO_ERROR) failures++;

    // Fallback growth path: copy rendered pixels from A into larger C using
    // glCopyTexSubImage2D. The source must be the framebuffer, not texture A.
    GLuint ctex = texture(256, 256, GL_RGBA, GL_RGBA);
    bind_fbo(GL_FRAMEBUFFER, read_fbo);
    attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, ctex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 128, 128);
    GLenum copy_error = glGetError();
    bind_fbo(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, ctex); quad(-1, -0.5f, 2.25f/256, 2.75f/256, 1.25f/256, 2.75f/256);
    glFinish();
    failures += pixel("copy-subimage A to C", 16, 1);
    printf("copy-subimage error=0x%x; total failures=%d\n", copy_error, failures);
    if (copy_error != GL_NO_ERROR) failures++;
    glXMakeCurrent(d, None, NULL); glXDestroyContext(d, c); XCloseDisplay(d);
    return failures ? 1 : 0;
}
