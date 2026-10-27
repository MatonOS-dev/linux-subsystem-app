#!/usr/bin/env python3
"""Guard the compositor's SHM backpressure path against timer-driven retries."""

from pathlib import Path


source = Path(__file__).resolve().parents[1] / "compositor_core.c"
text = source.read_text()

assert "struct wl_event_source* retry" not in text
assert "on_present_retry" not in text
assert "wl_event_source_timer_update" not in text
assert "w->present_pending=maton_presenter_present" in text

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

print("presenter backpressure retry contract: PASS")
