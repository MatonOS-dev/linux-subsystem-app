#!/usr/bin/env python3
"""Run fsck extraction under an app-like filter that traps utimensat."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MKFS = ROOT / "prebuilt/x86_64/matonos-mkfs-erofs"
FSCK = pathlib.Path(os.environ.get("FSCK_EROFS", ROOT / "prebuilt/x86_64/matonos-fsck-erofs"))
if not FSCK.is_file():
    raise SystemExit("Set FSCK_EROFS to the rebuilt static fsck.erofs")

launcher = r'''#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
  struct sock_filter code[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_utimensat, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog filter = { .len = sizeof(code) / sizeof(code[0]), .filter = code };
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter)) return 90;
  execv(argv[1], &argv[1]);
  perror("execv");
  return 91;
}
'''

with tempfile.TemporaryDirectory(prefix="matonos-fsck-seccomp-") as temp:
    base = pathlib.Path(temp)
    source = base / "files"
    (source / "app/extra").mkdir(parents=True)
    (source / "app/bin").mkdir(parents=True)
    probe = source / "app/bin/probe"
    probe.write_text("#!/bin/sh\nexit 0\n")
    probe.chmod(0o755)
    probe.touch()
    os.utime(probe, (1, 1))
    image = base / "probe.erofs"
    subprocess.run([str(MKFS), "-zlz4hc,12", "--all-root", "--mkfs-time", "-T", "0",
                    str(image), str(source)], check=True, stdout=subprocess.DEVNULL)
    (base / "launcher.c").write_text(launcher)
    binary = base / "seccomp-exec"
    subprocess.run(["cc", "-O2", "-Wall", "-Werror", str(base / "launcher.c"), "-o", str(binary)], check=True)
    extracted = base / "extracted"
    extracted.mkdir()
    result = subprocess.run([str(binary), str(FSCK), "--extract=" + str(extracted), str(image)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"fsck under utimensat-trapping app filter failed ({result.returncode}):\n{result.stdout}")
    extracted_probe = extracted / "app/bin/probe"
    if not extracted_probe.is_file() or extracted_probe.stat().st_mode & 0o777 != 0o755:
        raise SystemExit("fsck extraction did not preserve probe contents and executable mode")
    print("PASS fsck extraction succeeds under a seccomp filter that traps utimensat; executable mode is preserved")
