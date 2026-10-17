#ifndef MATON_VK_WSI_WAYLAND_H
#define MATON_VK_WSI_WAYLAND_H

#include <stdint.h>

/* Deliberately small ABI declarations: the ICD is freestanding and calls the
 * application's already-loaded libwayland-client through resolved symbols. */
struct wl_proxy;
struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wl_interface;

struct mv_wl_dmabuf;
struct mv_wl_buffer;

struct mv_wl_plane {
    int fd;
    uint32_t index, offset, stride;
    uint64_t modifier;
};

struct mv_wl_dmabuf *mv_wl_dmabuf_create(struct wl_display *display);
void mv_wl_dmabuf_destroy(struct mv_wl_dmabuf *dmabuf);
int mv_wl_dmabuf_version(const struct mv_wl_dmabuf *dmabuf);
struct mv_wl_buffer *mv_wl_dmabuf_create_buffer(struct mv_wl_dmabuf *dmabuf,
        uint32_t width, uint32_t height, uint32_t format,
        const struct mv_wl_plane *planes, uint32_t plane_count);
void mv_wl_buffer_destroy(struct mv_wl_buffer *buffer);
int mv_wl_buffer_released(const struct mv_wl_buffer *buffer);
void mv_wl_buffer_clear_released(struct mv_wl_buffer *buffer);

#endif
