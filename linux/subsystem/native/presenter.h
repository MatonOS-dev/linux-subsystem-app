#pragma once
#include <android/surface_control.h>
#include <stdbool.h>
#include "window_fit.h"

struct wlr_scene;
struct wlr_allocator;
struct MatonEglUploader;

/* Presents a window's scene to SurfaceFlinger as one child layer per
 * visible buffer, instead of compositing it: client dma-bufs are shown
 * without a copy; shared-memory buffers are copied once into an
 * AHardwareBuffer. Layers sit under `parent` (the window's surface). */
struct MatonPresenter;

struct MatonPresenter* maton_presenter_create(ASurfaceControl* parent, struct wlr_allocator* allocator,
                                              struct MatonEglUploader* uploader);
/* Removes all layers from the screen and frees them. */
void maton_presenter_destroy(struct MatonPresenter* presenter);
/* Synchronises the layers with the scene in one transaction. Returns true
 * if some content could not be shown yet (all of a layer's buffers busy). */
bool maton_presenter_present(struct MatonPresenter* presenter, struct wlr_scene* scene, struct MatonFit fit, int width, int height);
