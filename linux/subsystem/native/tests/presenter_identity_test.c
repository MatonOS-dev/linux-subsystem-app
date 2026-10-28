#include "../presenter_identity.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  const void* stable_surface = (const void*)(uintptr_t)0x1000;
  const void* current_node = NULL;
  unsigned fake_surface_control_binders = 0;

  for (uintptr_t frame = 1; frame <= 10000; ++frame) {
    const void* replacement_node = (const void*)(uintptr_t)(frame + 0x2000);
    if (!maton_presenter_identity_matches(stable_surface, current_node,
                                         stable_surface, replacement_node)) {
      ++fake_surface_control_binders; /* ASurfaceControl_create() call site */
    }
    current_node = replacement_node;
  }
  assert(fake_surface_control_binders == 0);

  /* A different Wayland surface still gets a distinct Binder-backed object. */
  const void* other_surface = (const void*)(uintptr_t)0x3000;
  if (!maton_presenter_identity_matches(stable_surface, current_node,
                                       other_surface, current_node))
    ++fake_surface_control_binders;
  assert(fake_surface_control_binders == 1);

  /* Non-surface scene nodes retain node identity semantics. */
  const void* node_a = (const void*)(uintptr_t)0x4000;
  const void* node_b = (const void*)(uintptr_t)0x5000;
  assert(maton_presenter_identity_matches(NULL, node_a, NULL, node_a));
  assert(!maton_presenter_identity_matches(NULL, node_a, NULL, node_b));

  puts("PASS presenter cache reuses Binder-backed SurfaceControl across 10k scene-node replacements");
  return 0;
}
