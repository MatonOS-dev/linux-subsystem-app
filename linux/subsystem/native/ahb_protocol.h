#pragma once
#include <stdbool.h>
#include <android/hardware_buffer.h>
struct wl_display;
struct wlr_buffer;
/* Transport only: enabling it does not enable any ICD WSI extension. */
bool maton_ahb_protocol_create(struct wl_display* display);
/* Includes the wlroots client-buffer source wrapper; borrowed AHB reference. */
AHardwareBuffer* maton_ahb_protocol_buffer(struct wlr_buffer* buffer);
