#!/usr/bin/env python3
"""The launch supervisor waits for orphaned payload children."""
import os
import subprocess
import sys
import time

root=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
loader=os.path.abspath(sys.argv[1])
child=os.path.join(root,"build","supervise-child")
subprocess.run(["cc","-O2","-Wall","-Wextra","-Werror",
                os.path.join(root,"tests","supervise-child.c"),"-o",child],check=True)
started=time.monotonic()
result=subprocess.run([loader,"--supervise","--android-filter",child],
                      stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=10)
elapsed=time.monotonic()-started
assert result.returncode==0,(result.returncode,result.stderr.decode(errors="replace"))
assert elapsed>=0.25, f"supervisor exited before orphaned payload: {elapsed:.3f}s"
print("PASS launcher waits for forked payload descendants after direct child exits")
