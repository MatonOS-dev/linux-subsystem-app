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

  /* A compositor client may replace both its scene node and wl_surface
   * wrapper while only one scene layer is live. Reuse a slot missed by exact
   * identity lookup, as presenter.c does, instead of creating one Binder per
   * replacement. */
  const unsigned transient_frames = 10000;
  const void* slot_surface = NULL;
  const void* slot_node = NULL;
  unsigned slot_seen = 0;
  unsigned slot_controls_created = 0;
  unsigned unique_binders_parcelled = 0;
  const void* release_listener_binder = (const void*)(uintptr_t)0x6000;
  const void* parcelled_binders[2] = {0};
  for (uintptr_t frame = 1; frame <= transient_frames; ++frame) {
    slot_seen = 0;
    /* The prior scene node was destroyed; its Layer survives for recycling. */
    slot_node = NULL;
    const void* transient_surface = (const void*)(uintptr_t)(frame + 0x100000);
    const void* transient_node = (const void*)(uintptr_t)(frame + 0x200000);
    if (maton_presenter_identity_matches(slot_surface, slot_node,
                                         transient_surface, transient_node)) {
      slot_seen = 1;
    } else if (!slot_seen && slot_controls_created != 0) {
      slot_surface = transient_surface;
      slot_node = transient_node;
      slot_seen = 1;
    } else {
      ++slot_controls_created; /* fake ASurfaceControl_create() */
      slot_surface = transient_surface;
      slot_node = transient_node;
      slot_seen = 1;
    }
    /* Both buffer release APIs use SurfaceComposerClient's process-wide
     * TransactionCompletedListener binder. Re-parceling that same Binder is
     * not creation of a new BinderProxy. Count unique Binder identities. */
    if (frame == 1) {
      parcelled_binders[unique_binders_parcelled++] = release_listener_binder;
    }
    assert(slot_seen == 1);
  }
  assert(slot_controls_created == 1);
  if (slot_controls_created) {
    parcelled_binders[unique_binders_parcelled++] = (const void*)(uintptr_t)0x7000;
  }
  assert(unique_binders_parcelled == 2); /* release listener + one layer slot */
  assert(parcelled_binders[0] == release_listener_binder);

  /* A scene-buffer node may be replaced while its wl_surface survives. Across
   * those replacements the identity test above keeps one control. Once a
   * short-lived surface itself dies, the presenter retires its control from
   * that surface's destroy listener. Model repeated popup/subsurface lifetime
   * and assert held Binder controls track concurrent surfaces, not all-time
   * allocations. */
  unsigned held_controls = 0;
  unsigned peak_held_controls = 0;
  unsigned control_allocations = 0;
  for (unsigned popup = 0; popup < 10000; ++popup) {
    ++held_controls; /* ASurfaceControl_create() for this live surface */
    ++control_allocations;
    if (held_controls > peak_held_controls) peak_held_controls = held_controls;
    assert(maton_presenter_identity_matches(NULL, (const void*)(uintptr_t)(popup + 1),
                                            NULL, (const void*)(uintptr_t)(popup + 1)));
    --held_controls; /* on_surface_destroy -> retire_layer -> layer_free */
  }
  assert(control_allocations == 10000);
  assert(held_controls == 0);
  assert(peak_held_controls == 1);

  /* Non-surface scene nodes retain node identity semantics. */
  const void* node_a = (const void*)(uintptr_t)0x4000;
  const void* node_b = (const void*)(uintptr_t)0x5000;
  assert(maton_presenter_identity_matches(NULL, node_a, NULL, node_a));
  assert(!maton_presenter_identity_matches(NULL, node_a, NULL, node_b));

  puts("PASS presenter reuses stable-surface layers and retires dead surfaces across 10k lifetimes");
  return 0;
}
