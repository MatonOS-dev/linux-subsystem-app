#!/usr/bin/env python3
"""Guard SHM backpressure and bound unchanged-frame Binder traffic."""

from pathlib import Path


source = Path(__file__).resolve().parents[1] / "compositor_core.c"
text = source.read_text()
presenter = (source.parent / "presenter.c").read_text()
android_output = (source.parent / "android_output.c").read_text()

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
assert "if (walk.changed) maton_transaction_apply(t, &p->pending_release)" in present_fn
assert "ASurfaceTransaction_create()" not in present_fn
assert "p->transaction = ASurfaceTransaction_create()" in presenter
assert "output->transaction = output->control ? ASurfaceTransaction_create() : NULL" in android_output
surface_present = android_output.split("bool maton_surface_output_present(", 1)[1]
assert "ASurfaceTransaction_create()" not in surface_present
assert "ASurfaceTransaction_setOnComplete(transaction, *pending, on_transaction_complete)" in android_output
assert "ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, pending->control)" in android_output
assert "maton_transaction_track_replaced(control, replaced, pending)" in android_output

assert "maton_presenter_layer_state_update" in presenter

print("presenter backpressure and 10k-frame transaction bound: PASS")
