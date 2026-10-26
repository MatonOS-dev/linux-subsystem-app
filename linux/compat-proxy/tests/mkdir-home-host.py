#!/usr/bin/env python3
"""Check mkdir of new writable-home children through the proxy's bwrap VFS."""
import pathlib
import shutil
import subprocess

base = pathlib.Path(__file__).resolve().parents[1]
root = base / "build/mkdir-home"
shutil.rmtree(root, ignore_errors=True)
home = root / "home"
appdata = home / ".var/app/net.supertuxkart.SuperTuxKart/data/supertuxkart"
appdata.mkdir(parents=True)
source = root / "probe.c"
source.write_text(r'''#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    for (int i = 1; i < argc; ++i) {
        errno = 0;
        int result = mkdir(argv[i], 0700);
        if (result) {
            fprintf(stderr, "mkdir(%s): result=%d errno=%d (%s)\n",
                    argv[i], result, errno, strerror(errno));
            return 1;
        }
    }
    puts("PASS writable-home mkdir with and without trailing slash");
    return 0;
}''')
probe = root / "probe"
subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                str(source), "-o", str(probe)], check=True)

plain = appdata / "addons"
trailing = appdata / "screenshots"
bwrap = shutil.which("bwrap")
if not bwrap:
    print("SKIP writable-home bwrap mkdir test: bwrap unavailable")
    raise SystemExit(0)

cmd = [str(base / "build/linux-layer-v3"), "--android-filter", bwrap,
       "--ro-bind", "/", "/", "--proc", "/proc", "--dev", "/dev",
       "--bind", str(home), str(home), str(probe), str(plain), str(trailing) + "/"]
result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout + "\n" + result.stderr)
print(result.stdout.strip())
