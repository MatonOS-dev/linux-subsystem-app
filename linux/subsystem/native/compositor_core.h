#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "android_output.h"

#ifdef __cplusplus
extern "C" {
#endif

bool maton_core_start(const char* socket_name, const char* runtime_dir);
void maton_core_close(int id);
void maton_core_stopped(int id, bool stopped);
bool maton_core_add_session(int session, const char* socket_path);
bool maton_core_xwayland_init(const char* socket_dir, const char* xwayland_path);
bool maton_core_add_xwayland(int session, int uid, char* display, size_t display_size);
void maton_core_stop(void);
void maton_core_launch_demo(void);
void maton_core_attach(int id, struct MatonSurfaceOutput* output, int width, int height);
void maton_core_detach(int id);
void maton_core_resize(int id, int width, int height);
void maton_core_set_fullscreen(int id, int on);
void maton_core_key(int id, int keycode, int scan_code, int action, int meta_state, int64_t event_time_ns);
void maton_core_motion(int id, float x, float y, float vertical_scroll,
                       float horizontal_scroll, int action, int buttons, int64_t event_time_ns);

void maton_java_request_window(int session, int id, int width, int height);
void maton_java_close_window(int id);
/* Window-state requests from the app (X11 _NET_WM_STATE, xdg_toplevel):
 * MATON_STATE_* below. Java answers fullscreen with maton_core_set_fullscreen. */
#define MATON_STATE_FULLSCREEN_ON 1
#define MATON_STATE_FULLSCREEN_OFF 2
#define MATON_STATE_MINIMIZE 3
#define MATON_STATE_CAPTURE_ON 4
#define MATON_STATE_CAPTURE_OFF 5
#define MATON_STATE_CLIENT_DECORATED 6
#define MATON_STATE_SERVER_DECORATED 7
/* 16 + wlroots edge mask; 16 alone requests a move. */
#define MATON_STATE_GEOMETRY_BASE 16
void maton_core_capture_lost(int id);
void maton_core_relative(int id,float x,float y,float vs,float hs,int buttons,int64_t time);
void maton_java_window_state(int id, int state);

#ifdef __cplusplus
}
#endif
