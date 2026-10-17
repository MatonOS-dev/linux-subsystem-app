#pragma once
#include <stdbool.h>
#include <wayland-server-core.h>
#include <android/hardware_buffer.h>
#include <wlr/types/wlr_buffer.h>

bool maton_ahb_buffer_global_create(struct wl_display *display);
AHardwareBuffer *maton_ahb_buffer_get(struct wlr_buffer *buffer);
