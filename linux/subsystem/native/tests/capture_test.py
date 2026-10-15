#!/usr/bin/env python3
"""Exercise constraint activation and destructive one-shot deactivation on host."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'compositor_core.c').read_text()
def function(name):
    start = source.index('static void ' + name + '(void)')
    end = source.index('\n}', start) + 2
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#define MATON_STATE_CAPTURE_OFF 5
#define MATON_STATE_CAPTURE_ON 4
#define WLR_POINTER_CONSTRAINT_V1_LOCKED 0
struct wlr_pointer_constraint_v1 {
  int type, region; void* surface;
  struct { struct { bool enabled; double x,y; } cursor_hint; } current;
};
static struct { struct wlr_pointer_constraint_v1* constraint;
  void* constraints; int pointer_window; double pointer_x,pointer_y;
  struct { struct { void* focused_surface; } pointer_state; } *seat;
} server;
static struct wlr_pointer_constraint_v1* candidate;
static int on,off,activated,deactivated,motions;
static void maton_java_window_state(int id,int state){assert(id==9);if(state==4)on++;else off++;}
static void wlr_seat_pointer_notify_motion(void* seat,int time,double x,double y){assert(seat==server.seat);assert(x==7&&y==8);motions++;}
static void wlr_pointer_constraint_v1_send_deactivated(struct wlr_pointer_constraint_v1* c){assert(server.constraint==NULL);deactivated++;free(c);}
static void wlr_pointer_constraint_v1_send_activated(struct wlr_pointer_constraint_v1* c){assert(server.constraint==c);activated++;}
static struct wlr_pointer_constraint_v1* wlr_pointer_constraints_v1_constraint_for_surface(void* manager,void* surface,void* seat){return candidate;}
static bool pixman_region32_contains_point(int* region,int x,int y,void* box){return *region&&x>=0&&y>=0&&x<10&&y<10;}
'''
code += function('deactivate_constraint') + '\n' + function('activate_constraint')
code += r'''
int main(void){
  server.seat=calloc(1,sizeof(*server.seat));server.constraints=&on;server.pointer_window=9;
  candidate=calloc(1,sizeof(*candidate));candidate->region=1;
  server.pointer_x=11;activate_constraint();assert(!activated);
  server.pointer_x=2;activate_constraint();activate_constraint();assert(activated==1&&on==1);
  candidate->current.cursor_hint.enabled=true;candidate->current.cursor_hint.x=7;candidate->current.cursor_hint.y=8;
  deactivate_constraint();deactivate_constraint();assert(off==1&&deactivated==1&&motions==1);assert(server.pointer_x==7&&server.pointer_y==8);
  candidate=calloc(1,sizeof(*candidate));candidate->type=1;candidate->region=1;
  activate_constraint();deactivate_constraint();assert(motions==1&&activated==2&&deactivated==2);
  free(server.seat);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'capture.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', str(path / 'capture.c'), '-lm', '-o', str(path / 'capture')], check=True)
    subprocess.run([str(path / 'capture')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    # LeakSanitizer cannot run under the workspace runner's ptrace sandbox.
print('activation bounds, duplicate activation, one-shot destruction, unlock hint: PASS')
