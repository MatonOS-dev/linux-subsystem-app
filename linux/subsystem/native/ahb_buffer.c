#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include "ahb_buffer.h"

#include <drm/drm_fourcc.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <types/wlr_buffer.h>

#include "maton-ahb-buffer-v1-server-protocol.h"

struct MatonAhbWlBuffer {
  struct wlr_buffer base;
  AHardwareBuffer *ahb;
  struct wl_resource *resource;
  struct wl_listener release;
};

static const struct wlr_buffer_impl ahb_buffer_impl;
static const struct wl_buffer_interface wl_buffer_impl;

static struct MatonAhbWlBuffer *from_buffer(struct wlr_buffer *buffer) {
  if (!buffer || buffer->impl != &ahb_buffer_impl) return NULL;
  return (struct MatonAhbWlBuffer *)buffer;
}

AHardwareBuffer *maton_ahb_buffer_get(struct wlr_buffer *buffer) {
  struct MatonAhbWlBuffer *wrapped = from_buffer(buffer);
  if (wrapped) return wrapped->ahb;
  struct wlr_client_buffer *client = buffer ? wlr_client_buffer_get(buffer) : NULL;
  return client && client->source ? maton_ahb_buffer_get(client->source) : NULL;
}

static void release_notify(struct wl_listener *listener, void *data) {
  (void)data;
  struct MatonAhbWlBuffer *buffer = wl_container_of(listener, buffer, release);
  if (buffer->resource) wl_buffer_send_release(buffer->resource);
}

static void buffer_destroy(struct wlr_buffer *base) {
  struct MatonAhbWlBuffer *buffer = from_buffer(base);
  wl_list_remove(&buffer->release.link);
  wlr_buffer_finish(base);
  if (buffer->resource) wl_resource_set_user_data(buffer->resource, NULL);
  if (buffer->ahb) AHardwareBuffer_release(buffer->ahb);
  free(buffer);
}

static const struct wlr_buffer_impl ahb_buffer_impl = { .destroy = buffer_destroy };

static bool buffer_resource_is_instance(struct wl_resource *resource) {
  return wl_resource_instance_of(resource, &wl_buffer_interface, &wl_buffer_impl);
}

static struct wlr_buffer *buffer_from_resource(struct wl_resource *resource) {
  struct MatonAhbWlBuffer *buffer = wl_resource_get_user_data(resource);
  return buffer ? &buffer->base : NULL;
}

static const struct wlr_buffer_resource_interface buffer_resource_interface = {
  .name = "maton_ahb_buffer",
  .is_instance = buffer_resource_is_instance,
  .from_resource = buffer_from_resource,
};

static void buffer_resource_destroy(struct wl_resource *resource) {
  struct MatonAhbWlBuffer *buffer = wl_resource_get_user_data(resource);
  if (!buffer) return;
  buffer->resource = NULL;
  wlr_buffer_drop(&buffer->base);
}

static void buffer_request_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = { .destroy = buffer_request_destroy };

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void manager_create_buffer(struct wl_client *client, struct wl_resource *manager,
    uint32_t id, int transport_fd, uint32_t width, uint32_t height, uint32_t format) {
  (void)manager;
  AHardwareBuffer *ahb = NULL;
  int rc = AHardwareBuffer_recvHandleFromUnixSocket(transport_fd, &ahb);
  close(transport_fd);
  if (rc || !ahb) {
    if (ahb) AHardwareBuffer_release(ahb);
    wl_client_post_implementation_error(client, "could not receive AHardwareBuffer");
    return;
  }
  AHardwareBuffer_Desc desc = {0};
  AHardwareBuffer_describe(ahb, &desc);
  if (format != DRM_FORMAT_ABGR8888 || desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
      desc.width != width || desc.height != height || desc.layers != 1 || !width || !height ||
      width > 16384 || height > 16384) {
    AHardwareBuffer_release(ahb);
    wl_client_post_implementation_error(client, "AHardwareBuffer description does not match request");
    return;
  }
  struct MatonAhbWlBuffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer) {
    AHardwareBuffer_release(ahb);
    wl_client_post_no_memory(client);
    return;
  }
  buffer->ahb = ahb;
  buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
  if (!buffer->resource) {
    AHardwareBuffer_release(ahb);
    free(buffer);
    wl_client_post_no_memory(client);
    return;
  }
  wlr_buffer_init(&buffer->base, &ahb_buffer_impl, (int)width, (int)height);
  buffer->release.notify = release_notify;
  wl_signal_add(&buffer->base.events.release, &buffer->release);
  wl_resource_set_implementation(buffer->resource, &wl_buffer_impl, buffer, buffer_resource_destroy);
}

static const struct maton_ahb_buffer_manager_v1_interface manager_impl = {
  .destroy = manager_destroy,
  .create_buffer = manager_create_buffer,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  (void)data;
  struct wl_resource *resource = wl_resource_create(client,
      &maton_ahb_buffer_manager_v1_interface, version > 1 ? 1 : version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wlr_buffer_register_resource_interface(&buffer_resource_interface);
  wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

bool maton_ahb_buffer_global_create(struct wl_display *display) {
  return display && wl_global_create(display, &maton_ahb_buffer_manager_v1_interface,
      1, NULL, bind_manager) != NULL;
}
