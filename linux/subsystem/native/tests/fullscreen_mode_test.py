#!/usr/bin/env python3
"""Exercise actual X11 sizing handlers with simulated wlroots endpoints."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'compositor_core.c').read_text()
def function(name):
    start=source.index('static void '+name+'(')
    # Skip forward declarations.
    while source.index(';',start)<source.index('{',start):
        start=source.index('static void '+name+'(',start+1)
    begin=source.index('{',start);end=begin+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct wl_listener { int unused; };
struct Hints { int min_width,max_width,min_height,max_height;bool fixed; };
struct Base { bool initialized; };
struct Top { struct Base *base; };
struct XS { int x,y,width,height;bool fullscreen; };
struct Window { struct Hints hints;int width,height;struct XS *xsurface;struct Top *toplevel; };
struct XTrack { struct wl_listener configure;struct XS *xs;struct Window *window; };
struct wlr_xwayland_surface_configure_event {int x,y,width,height;};
static int configurations;
static bool maton_size_fixed(struct Hints h){return h.fixed;}
static int maton_size_clamp(int n,int min,int max){return n<min?min:(max>0&&n>max?max:n);}
static void wlr_xdg_toplevel_set_size(struct Top *t,int w,int h){(void)t;(void)w;(void)h;assert(0);}
static void wlr_xwayland_surface_configure(struct XS *xs,int x,int y,int w,int h){configurations++;xs->x=x;xs->y=y;xs->width=w;xs->height=h;}
static struct Window *xtrack_window(struct XTrack *t){return t->window;}
'''
code+=function('configure_window')+'\n'+function('on_xtrack_configure')
code+=r'''
int main(void){
  struct XS xs={.width=1600,.height=900,.fullscreen=true};
  struct Window w={.width=1600,.height=900,.xsurface=&xs};
  struct XTrack t={.xs=&xs,.window=&w};
  struct wlr_xwayland_surface_configure_event e={.width=1280,.height=720};
  on_xtrack_configure(&t.configure,&e);assert(xs.width==1280&&xs.height==720);
  configure_window(&w);assert(xs.width==1280&&xs.height==720&&configurations==1);
  xs.fullscreen=false;configure_window(&w);assert(xs.width==1600&&xs.height==900);
  on_xtrack_configure(&t.configure,&e);assert(xs.width==1600&&xs.height==900);
  t.window=NULL;on_xtrack_configure(&t.configure,&e);assert(xs.width==1280&&xs.height==720);
}
'''
with tempfile.TemporaryDirectory(prefix='maton-mode-') as tmp:
    c=Path(tmp)/'mode.c';c.write_text(code);binary=Path(tmp)/'mode'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS fullscreen mode acceptance/preservation, windowed size enforcement and unmapped configure')
