#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A wlroots scene node can be rebuilt while its wl_surface survives. Keep the
 * Android SurfaceControl keyed to that stable protocol object so node churn
 * cannot create a new Binder-backed SurfaceControl for every frame. */
static inline bool maton_presenter_identity_matches(
    const void* candidate_surface, const void* candidate_node,
    const void* surface, const void* node) {
  if (surface) return candidate_surface == surface;
  return candidate_surface == NULL && candidate_node == node;
}
