#!/usr/bin/env python3
"""Run the production hint/configure callbacks against repeating X properties."""
from pathlib import Path
import subprocess, tempfile, sys
native=Path(__file__).resolve().parents[1]
source=(Path(sys.argv[1]) if len(sys.argv)>1 else native/'compositor_core.c').read_text()
def function(name):
    start=source.index('static void '+name+'(')
    while source.index(';',start)<source.index('{',start):
        start=source.index('static void '+name+'(',start+1)
    end=source.index('{',start)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
code=r'''
#include <assert.h>
#include <stddef.h>
#include "window_fit.h"
#define XCB_ICCCM_SIZE_HINT_P_MIN_SIZE 1
#define XCB_ICCCM_SIZE_HINT_P_MAX_SIZE 2
#define ANDROID_LOG_DEBUG 0
struct wl_listener {int unused;};
typedef struct {int flags,min_width,min_height,max_width,max_height;} xcb_size_hints_t;
struct XS {int x,y,width,height;bool fullscreen;xcb_size_hints_t *size_hints;};
struct Base {bool initialized;};struct Top {struct Base *base;};
struct Window {int id,width,height,activity,session;struct MatonSizeHints hints;struct XS *xsurface;struct Top *toplevel;struct wl_listener size_hints;};
static struct Window *window_from_listener(struct wl_listener *l,size_t offset){return (void *)((char *)l-offset);}
static int configurations,logs,requests;
static void maton_java_request_window(int session,int id,int width,int height){(void)session;(void)id;assert(width==700&&height==330);requests++;}
static void wlr_xdg_toplevel_set_size(struct Top *t,int w,int h){(void)t;(void)w;(void)h;assert(0);}
static void wlr_xwayland_surface_configure(struct XS *xs,int x,int y,int w,int h){configurations++;xs->x=x;xs->y=y;xs->width=w;xs->height=h;}
#define __android_log_print(...) (++logs)
'''
code+='\n'.join(function(n) for n in ('configure_window','read_xsize_hints','on_xsize_hints'))
code+=r'''
int main(void){
 xcb_size_hints_t hints={3,128,66,128,66};
 struct XS xs={.x=576,.y=327,.width=128,.height=66,.size_hints=&hints};
 struct Window w={.activity=1,.width=1600,.height=900,.xsurface=&xs};
 read_xsize_hints(&w);
 hints.min_width=hints.max_width=700;hints.min_height=hints.max_height=330;
 on_xsize_hints(&w.size_hints,NULL);
 assert(configurations==1&&logs==1&&xs.width==700&&xs.height==330&&xs.x==576&&xs.y==327);
 for(int i=0;i<12908;i++)on_xsize_hints(&w.size_hints,NULL);
 assert(configurations==1&&logs==1&&requests==1);
 hints.flags=0;on_xsize_hints(&w.size_hints,NULL);
 assert(xs.width==1600&&xs.height==900&&xs.x==0&&xs.y==0&&configurations==2);
 hints=(xcb_size_hints_t){3,800,450,800,450};xs.fullscreen=true;
 on_xsize_hints(&w.size_hints,NULL);assert(configurations==2);
}
'''
with tempfile.TemporaryDirectory(prefix='maton-hints-') as tmp:
    c=Path(tmp)/'test.c';c.write_text(code);binary=Path(tmp)/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-I',str(native),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS X11 dialog resize, fixed origin, 12908 unchanged hints, resizable/fullscreen transitions')
