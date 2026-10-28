#!/usr/bin/env python3
"""Guard SHM backpressure and bound unchanged-frame Binder traffic."""

from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1] / "compositor_core.c"
text = source.read_text()
presenter = (source.parent / "presenter.c").read_text()
android_output = (source.parent / "android_output.c").read_text()
compositor_build = source.parents[1] / "build-compositor.sh"

assert "struct wl_event_source* retry" not in text
assert "on_present_retry" not in text
assert "wl_event_source_timer_update" not in text
assert "w->present_pending=maton_presenter_present" in text
assert "if(!w->present_pending)" in text

release_fence = text.split("static int on_release_fence", 1)[1].split("static void release_buffer", 1)[0]
assert "wlr_buffer_unlock(release->buffer)" in release_fence
assert "schedule_pending_presentations()" in release_fence

schedule_pending = text.split("static void schedule_pending_presentations", 1)[1].split("static int on_release_fence", 1)[0]
assert "w->present_pending&&w->output&&!w->stopped" in schedule_pending
assert "wlr_output_schedule_frame(w->output)" in schedule_pending

immediate_release = text.split("static void track_release", 1)[1].split("static void on_commit", 1)[0]
assert "if(fd<0)" in immediate_release
assert "wlr_buffer_unlock(buffer)" in immediate_release
assert "schedule_pending_presentations()" in immediate_release

# The presenter and window output own one native SurfaceTransaction for their
# lifetime. Re-applying identical presenter state must not call SurfaceFlinger.
present_fn = presenter.split("bool maton_presenter_present(", 1)[1].split("void maton_presenter_destroy(", 1)[0]
assert "ASurfaceTransaction* t = p->transaction" in present_fn
assert "if (walk.changed) maton_transaction_apply(t)" in present_fn
assert "ASurfaceTransaction_create()" not in present_fn
assert "p->transaction = ASurfaceTransaction_create()" in presenter
assert "output->transaction = output->control ? ASurfaceTransaction_create() : NULL" in android_output
surface_present = android_output.split("bool maton_surface_output_present(", 1)[1]
assert "ASurfaceTransaction_create()" not in surface_present
assert "ASurfaceTransaction_setBufferWithRelease(transaction, control, buffer, acquire_fence_fd" in android_output
assert "ASurfaceTransaction_setOnComplete" not in android_output
assert "ASurfaceTransactionStats_getPreviousReleaseFenceFd" not in android_output
assert "maton_transaction_track_replaced" not in android_output
# API 36 is required for per-buffer release callbacks. No frame transaction
# can register an IRemoteCallback with system_server, including startup bursts.
assert "API=36" in compositor_build.read_text()
assert android_output.count("ASurfaceTransaction_setBufferWithRelease(") == 1

assert "maton_presenter_layer_state_update" in presenter

# A scene-buffer node can be rebuilt while the Wayland surface survives. Keep
# that SurfaceControl reusable, then retire it on the underlying surface's
# destroy event; generic scene buffers retire with their node.
destroy_fn = presenter.split("static void on_node_destroy(", 1)[1].split(
    "static void on_surface_destroy(", 1)[0]
surface_destroy_fn = presenter.split("static void on_surface_destroy(", 1)[1].split(
    "static void bind_layer_node(", 1)[0]
retire_fn = presenter.split("static void retire_layer(", 1)[1].split(
    "static void on_node_destroy(", 1)[0]
assert "l->node = NULL" in destroy_fn
assert "if (!l->surface) retire_layer(l->presenter, l)" in destroy_fn
assert "retire_layer(l->presenter, l)" in surface_destroy_fn
assert "wl_signal_add(&surface->events.destroy, &l->surface_destroy)" in presenter
orphan_fn = presenter.split("/* Hide layers not drawn this frame", 1)[1].split(
    "if (!l->seen && l->visible)", 1)[0]
assert "if (l->surface)" in orphan_fn
assert "ASURFACE_TRANSACTION_VISIBILITY_HIDE" in orphan_fn
assert "link = &l->next" in orphan_fn
assert "*link = l->next" in retire_fn
assert "ASurfaceTransaction_reparent(p->transaction, l->control, NULL)" in retire_fn
assert "maton_transaction_apply(p->transaction)" in retire_fn
assert "layer_free(l)" in retire_fn
assert "remove_listener(&l->surface_destroy)" in presenter.split(
    "void maton_presenter_destroy(", 1)[1]

# Pool selection scans all entries for an unlocked, size-matched buffer before
# trying allocation, and grows on demand instead of imposing a four-buffer cap.
pool_fn = presenter.split("static struct wlr_buffer* free_pool_buffer(", 1)[1].split(
    "/* Copies shared-memory content", 1)[0]
assert "SHM_POOL" not in presenter
assert pool_fn.index("if (b->width == width && b->height == height) return b") < pool_fn.index(
    "wlr_allocator_create_buffer")
assert "entry->next = l->pool" in pool_fn

# SurfaceControl owns a Binder-backed surface. Simulate node replacement over
# 10k frames and count new surfaces through the same production identity helper.
test_source = Path(__file__).with_name("presenter_identity_test.c")
with tempfile.TemporaryDirectory(prefix="maton-presenter-test-") as temp:
    binary = Path(temp) / "presenter-identity-test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(test_source),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

print("presenter backpressure and 10k-frame transaction bound: PASS")
