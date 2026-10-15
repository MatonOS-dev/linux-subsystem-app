#!/usr/bin/env python3
"""Run the actual client move/resize handlers with fake seat/Java endpoints."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'compositor_core.c').read_text()
names = ['request_geometry', 'on_toplevel_request_move', 'on_toplevel_request_resize',
         'on_xrequest_move', 'on_xrequest_resize']
functions = []
for name in names:
    start = source.index('static void ' + name + '(')
    functions.append(source[start:source.index('\n}', start) + 2])
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MATON_STATE_GEOMETRY_BASE 16
struct wl_listener { int unused; };
struct wlr_seat { int unused; };
struct wlr_seat_client { struct wlr_seat *seat; };
struct Window { int id; struct wl_listener request_move, request_resize; };
struct wlr_xdg_toplevel_move_event { struct wlr_seat_client *seat; uint32_t serial; };
struct wlr_xdg_toplevel_resize_event { struct wlr_seat_client *seat; uint32_t serial, edges; };
struct wlr_xwayland_resize_event { uint32_t edges; };
static struct { struct wlr_seat *seat; void *constraint; int pointer_window; uint32_t pointer_buttons; } server;
static int requests, last_state;
static void maton_java_window_state(int id, int state) { assert(id == 9); requests++; last_state=state; }
static struct Window *window_from_listener(struct wl_listener *l, size_t offset) { return (void *)((char *)l-offset); }
static void *window_root_surface(struct Window *w) { return w; }
static bool wlr_seat_validate_pointer_grab_serial(struct wlr_seat *seat, void *origin, uint32_t serial) {
    assert(seat==server.seat); return origin && serial==42;
}
'''
code += '\n'.join(functions)
code += r'''
int main(void) {
    struct wlr_seat seat={0}, other={0};
    struct wlr_seat_client client={&seat};
    struct Window w={.id=9};
    server.seat=&seat; server.pointer_window=9; server.pointer_buttons=1;
    struct wlr_xdg_toplevel_move_event move={&client,42};
    on_toplevel_request_move(&w.request_move,&move); assert(requests==1 && last_state==16);
    move.serial=41; on_toplevel_request_move(&w.request_move,&move); assert(requests==1);
    move.serial=42; client.seat=&other; on_toplevel_request_move(&w.request_move,&move); assert(requests==1);
    client.seat=&seat;
    struct wlr_xdg_toplevel_resize_event resize={&client,42,5};
    on_toplevel_request_resize(&w.request_resize,&resize); assert(requests==2 && last_state==21);
    for (int edge=0;edge<32;edge++) {
        int before=requests; resize.edges=edge;
        on_toplevel_request_resize(&w.request_resize,&resize);
        bool valid=edge!=0 && edge<16 && (edge&3)!=3 && (edge&12)!=12;
        assert(requests==before+(valid?1:0));
    }
    int before=requests;
    server.pointer_window=8; on_xrequest_move(&w.request_move,0); assert(requests==before);
    server.pointer_window=9; server.pointer_buttons=0; on_xrequest_move(&w.request_move,0); assert(requests==before);
    server.pointer_buttons=2; on_xrequest_move(&w.request_move,0); assert(requests==before);
    server.pointer_buttons=1; server.constraint=&seat; on_xrequest_move(&w.request_move,0); assert(requests==before);
    server.constraint=0; on_xrequest_move(&w.request_move,0); assert(requests==before+1);
    struct wlr_xwayland_resize_event xr={10};
    on_xrequest_resize(&w.request_resize,&xr); assert(requests==before+2 && last_state==26);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='maton-geometry-') as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
print('geometry request tests passed (ASan/UBSan)')
