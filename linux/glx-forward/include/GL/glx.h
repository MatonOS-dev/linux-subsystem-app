#ifndef MATON_GLX_GLX_H
#define MATON_GLX_GLX_H
#include <stdint.h>
#include <GL/gl.h>
typedef struct _XDisplay Display; typedef unsigned long XID; typedef XID GLXDrawable;
typedef XID GLXWindow; typedef XID GLXPixmap; typedef XID GLXPbuffer;
typedef struct __GLXcontextRec *GLXContext; typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef void (*__GLXextFuncPtr)(void);
typedef struct { void *visual; unsigned long visualid; int screen; int depth; int c_class; unsigned long red_mask,green_mask,blue_mask; int colormap_size,bits_per_rgb; } XVisualInfo;
typedef int Bool;
#endif
