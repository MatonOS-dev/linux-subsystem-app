#!/usr/bin/env python3
"""The launch supervisor waits for orphaned payload children."""
import os
import subprocess
import sys
import time

root=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
loaders=[os.path.abspath(path) for path in sys.argv[1:]]
if not loaders:
    raise SystemExit("usage: supervise.py LOADER [LOADER ...]")
child=os.path.join(root,"build","supervise-child")
subprocess.run(["cc","-O2","-Wall","-Wextra","-Werror",
                os.path.join(root,"tests","supervise-child.c"),"-o",child],check=True)
for loader in loaders:
    started=time.monotonic()
    # --supervise immediately follows the executable, before ordinary loader
    # options and the Flatpak command, as it does in V3Launcher.
    result=subprocess.run([loader,"--supervise","--android-filter",child],
                          stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=10)
    elapsed=time.monotonic()-started
    assert result.returncode==0,(loader,result.returncode,result.stderr.decode(errors="replace"))
    assert elapsed>=0.25, f"{loader}: supervisor exited before orphaned payload: {elapsed:.3f}s"
    print(f"PASS {os.path.basename(loader)} supervisor waits for forked payload descendants")
    assert b"lifecycle-" not in result.stderr, f"{loader}: lifecycle tracing enabled without --trace"
    traced=subprocess.run([loader,"--supervise","--trace","--android-filter",child],
                          stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=10)
    trace=traced.stderr
    assert traced.returncode==0,(loader,traced.returncode,trace.decode(errors="replace"))
    for event in (b"supervisor-launch",b"supervisor-wait-main",b"supervisor-main-status",
                  b"supervisor-descendant-status",b"lifecycle-clone",b"lifecycle-exit-group"):
        assert event in trace, f"{loader}: missing {event!r} lifecycle event: {trace.decode(errors='replace')}"
    print(f"PASS {os.path.basename(loader)} v3-trace records supervisor and payload lifecycle")
