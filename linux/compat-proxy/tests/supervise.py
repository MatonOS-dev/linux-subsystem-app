#!/usr/bin/env python3
"""The launch supervisor waits for orphaned payload children."""
import os
import subprocess
import sys
import tempfile
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

    with tempfile.TemporaryDirectory(prefix="v3-supervise-window-") as directory:
        window=os.path.join(directory,"window-open")
        session=subprocess.Popen([loader,"--supervise","--android-filter",child,"detached",window],
                                 stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+3
        while not os.path.exists(window) and session.poll() is None and time.monotonic()<deadline:
            time.sleep(0.01)
        assert os.path.exists(window), f"{loader}: detached child did not open its window"
        assert session.poll() is None, f"{loader}: session ended when its parent exited"
        while os.path.exists(window) and session.poll() is None and time.monotonic()<deadline:
            time.sleep(0.01)
        assert not os.path.exists(window), f"{loader}: detached window owner did not exit"
        assert session.wait(timeout=3)==0, f"{loader}: final payload tree status was not preserved"
    print(f"PASS {os.path.basename(loader)} keeps the session until its detached window owner exits")

    live=subprocess.run([loader,"--supervise","--trace","--android-filter",child,"live"],
                        stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=10)
    assert live.returncode==23,(loader,live.returncode,live.stderr.decode(errors="replace"))
    trace=live.stderr
    assert trace.index(b"supervisor-descendant-status")<trace.index(b"supervisor-main-status"),trace
    assert b" 9472" in trace,trace # adopted child's exit 37, in wait-status encoding
    print(f"PASS {os.path.basename(loader)} reaps adopted child while main lives, preserves main exit 23")

    def stray_session(directory,stub):
        pids=os.path.join(directory,"pids")
        command=[loader,"--supervise","--android-filter",child,"stray",pids]
        if stub:
            # Stand-in for the stub process; the supervisor's PDEATHSIG fires when it dies.
            command=[sys.executable,"-c","import os,subprocess,sys,signal;subprocess.Popen([sys.argv[1],'--supervise','--parent-pid',str(os.getpid())]+sys.argv[3:]);signal.pause()",*command]
        parent=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+5
        while not os.path.exists(pids) and time.monotonic()<deadline:time.sleep(0.01)
        time.sleep(0.05)
        tree=[int(x) for x in open(pids).read().split()]
        assert len(tree)==2,tree
        return parent,tree
    def gone(pids):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            alive=[p for p in pids if os.path.exists(f"/proc/{p}") and open(f"/proc/{p}/stat").read().rsplit(")",1)[1].split()[0]!="Z"]
            if not alive:return True
            time.sleep(0.02)
        return False
    with tempfile.TemporaryDirectory(prefix="v3-supervise-stray-") as directory:
        session,tree=stray_session(directory,False)
        session.terminate()
        assert session.wait(timeout=5)==128+15,(loader,session.returncode)
        survivors=[p for p in tree if not gone([p])]
        for p in survivors:os.kill(p,9)
        assert not survivors,f"{loader}: adopted descendants survived SIGTERM: {survivors}"
        assert b"supervisor-terminate" in session.stderr.read()
    print(f"PASS {os.path.basename(loader)} SIGTERM kills the adopted descendant tree")
    with tempfile.TemporaryDirectory(prefix="v3-supervise-stub-") as directory:
        stub,tree=stray_session(directory,True)
        stub.kill();stub.wait(timeout=5)
        survivors=[p for p in tree if not gone([p])]
        for p in survivors:os.kill(p,9)
        assert not survivors,f"{loader}: adopted descendants survived the stub's death: {survivors}"
    print(f"PASS {os.path.basename(loader)} stub death (PDEATHSIG) kills the adopted descendant tree")
