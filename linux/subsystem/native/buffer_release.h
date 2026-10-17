#pragma once
#include <stdbool.h>
struct wl_event_loop;
struct MatonBufferRelease;
/* All functions except complete run on the Wayland thread. A reservation is
 * consumed by exactly one complete callback or cancel before submission. */
bool maton_buffer_release_start(struct wl_event_loop* loop, void (*release)(void*));
void maton_buffer_release_stop(void);
struct MatonBufferRelease* maton_buffer_release_reserve(void* buffer);
void maton_buffer_release_cancel(struct MatonBufferRelease* release);
/* May run on a binder thread; takes ownership of fence (-1 = no pending work).
 * Never allocates, waits for a fence, or accesses wlroots on that thread. */
void maton_buffer_release_complete(void* release, int fence);
