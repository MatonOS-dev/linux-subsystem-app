#pragma once

#include <stdbool.h>

enum MatonPresenterStateChange {
  MATON_PRESENTER_GEOMETRY_CHANGED = 1u << 0,
  MATON_PRESENTER_Z_CHANGED = 1u << 1,
  MATON_PRESENTER_OPACITY_CHANGED = 1u << 2,
  MATON_PRESENTER_TRANSPARENCY_CHANGED = 1u << 3,
};

struct MatonPresenterLayerState {
  int src[4], dst[4];
  int z;
  float opacity;
  int transparency;
  bool valid;
};

/* Cache the SurfaceControl properties that remain stable across client frames.
 * The same function is used by the presenter and the 10k-frame host test. */
static inline unsigned maton_presenter_layer_state_update(
    struct MatonPresenterLayerState* state, const int src[4], const int dst[4], int z,
    float opacity, int transparency) {
  unsigned changed = 0;
  for (int i = 0; i < 4; ++i) {
    if (!state->valid || state->src[i] != src[i] || state->dst[i] != dst[i]) {
      state->src[i] = src[i];
      state->dst[i] = dst[i];
      changed |= MATON_PRESENTER_GEOMETRY_CHANGED;
    }
  }
  if (!state->valid || state->z != z) {
    state->z = z;
    changed |= MATON_PRESENTER_Z_CHANGED;
  }
  if (!state->valid || state->opacity != opacity) {
    state->opacity = opacity;
    changed |= MATON_PRESENTER_OPACITY_CHANGED;
  }
  if (!state->valid || state->transparency != transparency) {
    state->transparency = transparency;
    changed |= MATON_PRESENTER_TRANSPARENCY_CHANGED;
  }
  state->valid = true;
  return changed;
}
