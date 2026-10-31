#!/usr/bin/env python3
"""Exercise override-redirect commit placement and frame scheduling."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'compositor_core.c').read_text()
start = source.index('static void on_xoverlay_commit(struct wl_listener* l,void* data) {')
callback = source[start:source.index('\n}', start) + 2]
code = r'''
#include <assert.h>
#include <stddef.h>
struct wl_listener { int unused; };
struct wlr_scene_node { int x,y; };
struct wlr_scene_tree { struct wlr_scene_node node; };
struct wlr_output { int schedules; };
struct wlr_xwayland_surface { int x,y; };
struct Window { struct wlr_xwayland_surface *xsurface; struct wlr_scene_tree *scene; struct wlr_output *output; int stopped; };
struct XOverlay { struct wlr_xwayland_surface *xs; struct wlr_scene_tree *tree; int session; struct wl_listener associate,dissociate,geometry,commit,destroy,tree_destroy; };
static struct Window *owner;
static struct Window *xoverlay_owner(struct XOverlay *o) { (void)o; return owner; }
static void wlr_scene_node_set_position(struct wlr_scene_node *node,int x,int y) { node->x=x; node->y=y; }
static void wlr_output_schedule_frame(struct wlr_output *output) { output->schedules++; }
'''
code += callback + r'''
int main(void) {
  struct wlr_xwayland_surface xs={.x=317,.y=129}, parent={.x=40,.y=20};
  struct wlr_scene_tree tree={0}; struct wlr_output output={0};
  struct Window window={.xsurface=&parent,.output=&output};
  struct XOverlay overlay={.xs=&xs,.tree=&tree}; owner=&window;
  on_xoverlay_commit(&overlay.commit,0);
  assert(tree.node.x==277 && tree.node.y==109 && output.schedules==1);
  window.stopped=1; xs.x=400;
  on_xoverlay_commit(&overlay.commit,0);
  assert(tree.node.x==360 && output.schedules==1);
  overlay.tree=0; xs.x=500;
  on_xoverlay_commit(&overlay.commit,0);
  assert(output.schedules==1);
}
'''
with tempfile.TemporaryDirectory(prefix='maton-xoverlay-') as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
print('override-redirect commit placement and frame scheduling passed (ASan/UBSan)')
