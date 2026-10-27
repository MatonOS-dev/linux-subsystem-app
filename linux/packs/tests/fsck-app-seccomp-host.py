#!/usr/bin/env python3
"""Run a Chrome-like EROFS extraction under Bionic's generated x86_64 app policy."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FSCK = pathlib.Path(os.environ.get("FSCK_EROFS", ROOT / "prebuilt/x86_64/matonos-fsck-erofs"))
MKFS = pathlib.Path(os.environ.get("MKFS_EROFS", ROOT / "prebuilt/x86_64/matonos-mkfs-erofs"))
def find_bionic_libc():
    candidates = []
    for value in (os.environ.get("MATON_AOSP_ROOT"), os.environ.get("MATON_BUILD_ROOT"), "/mnt/data/aosp"):
        if value:
            candidate = pathlib.Path(value).resolve()
            candidates.extend((candidate, *candidate.parents))
    seen = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        libc = candidate / "bionic/libc"
        if (libc / "tools/genseccomp.py").is_file():
            return libc
    return None

LIBC = find_bionic_libc()

if not FSCK.is_file() or not MKFS.is_file():
    raise SystemExit("Build linux/packs first or set FSCK_EROFS and MKFS_EROFS")
if LIBC is None:
    print("SKIP fsck app-seccomp test: Bionic sources are absent from the configured build context and /mnt/data/aosp")
    raise SystemExit(0)

launcher = r'''
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <asm/unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "x86_64_app_policy.cpp"
int main(int argc, char **argv) {
  struct sock_filter filter[3 + 1 + 2 + x86_64_app_filter_size + 1];
  size_t count = 0;
  filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
      offsetof(struct seccomp_data, arch));
  filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
      AUDIT_ARCH_X86_64, 1, 0);
  filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
      SECCOMP_RET_KILL_PROCESS);
  filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
      offsetof(struct seccomp_data, nr));
  /* App-data SELinux denies linkat; model that denial as EACCES. */
  filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
      __NR_linkat, 0, 1);
  filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
      SECCOMP_RET_ERRNO | EACCES);
  for (size_t i = 0; i < x86_64_app_filter_size; ++i)
    filter[count++] = x86_64_app_filter[i];
  filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
      SECCOMP_RET_TRAP);
  struct sock_fprog policy = { .len = (unsigned short)count, .filter = filter };
  if (argc < 2) return 90;
  pid_t child = fork();
  if (child < 0) return 90;
  if (!child) {
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(91);
    raise(SIGSTOP);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &policy)) _exit(92);
    execv(argv[1], argv + 1);
    _exit(93);
  }
  int status;
  if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) return 94;
  if (ptrace(PTRACE_CONT, child, NULL, NULL)) return 95;
  for (;;) {
    if (waitpid(child, &status, 0) != child) return 96;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (!WIFSTOPPED(status)) continue;
    int signal = WSTOPSIG(status);
    if (signal == SIGSYS) {
      siginfo_t info;
      memset(&info, 0, sizeof(info));
      if (!ptrace(PTRACE_GETSIGINFO, child, NULL, &info))
        fprintf(stderr, "BIONIC_APP_POLICY_BLOCKED syscall=%d arch=0x%x\n",
                info.si_syscall, info.si_arch);
      ptrace(PTRACE_KILL, child, NULL, NULL);
      waitpid(child, &status, 0);
      return 97;
    }
    if (signal == SIGTRAP) signal = 0;
    if (ptrace(PTRACE_CONT, child, NULL, (void *)(intptr_t)signal)) return 98;
  }
}
'''

with tempfile.TemporaryDirectory(prefix="matonos-fsck-app-policy-") as temp:
    base = pathlib.Path(temp)
    generated = base / "generated"
    generated.mkdir()
    nr_file = generated / "libseccomp_gen_syscall_nrs_x86_64"
    with nr_file.open("wb") as out:
        subprocess.run([
            os.environ.get("CPP", "gcc"), "-E", "-dD", "-nostdinc",
            f"-I{LIBC / 'kernel/uapi/asm-x86'}", f"-I{LIBC / 'kernel/uapi'}",
            str(LIBC / "seccomp/gen_syscall_nrs_x86_64.cpp"),
        ], check=True, stdout=out)
    subprocess.run([
        "python3", str(LIBC / "tools/genseccomp.py"),
        f"--out-dir={generated}", "--name-modifier=app",
        str(LIBC / "SYSCALLS.TXT"),
        str(LIBC / "SECCOMP_ALLOWLIST_COMMON.TXT"),
        str(LIBC / "SECCOMP_ALLOWLIST_APP.TXT"),
        str(LIBC / "SECCOMP_BLOCKLIST_COMMON.TXT"),
        str(LIBC / "SECCOMP_BLOCKLIST_APP.TXT"),
        str(LIBC / "SECCOMP_PRIORITY.TXT"), str(nr_file),
    ], check=True)

    source = base / "chrome-like"
    (source / "app/extra/opt/google/chrome").mkdir(parents=True)
    executable = source / "app/extra/opt/google/chrome/chrome"
    executable.write_bytes(b"\x7fELF\x00Chrome-like executable payload\n")
    executable.chmod(0o4755)
    hardlink = source / "app/extra/opt/google/chrome/chrome-hardlink"
    os.link(executable, hardlink)
    (source / "app/extra/opt/google/chrome/chrome-link").symlink_to("chrome")
    data = source / "app/extra/opt/google/chrome/resources.pak"
    data.write_bytes(b"chrome resource data" * 256)
    data.chmod(0o640)
    try:
        os.setxattr(data, "user.matonos.chrome-test", b"xattr-preserved")
    except OSError as error:
        if error.errno not in (1, 95, 524):
            raise

    image = base / "chrome-like.erofs"
    subprocess.run([
        str(MKFS), "-zlz4hc,12", "--all-root", "--mkfs-time", "-T", "0",
        str(image), str(source),
    ], check=True, stdout=subprocess.DEVNULL)

    (base / "launcher.cpp").write_text(launcher)
    runner = base / "app-policy-exec"
    subprocess.run([
        "g++", "-O2", "-Wall", "-Werror", f"-I{generated}",
        f"-I{LIBC}", str(base / "launcher.cpp"), "-o", str(runner),
    ], check=True)
    extracted = base / "extracted"
    extracted.mkdir()
    result = subprocess.run([
        str(runner), str(FSCK), "--extract=" + str(extracted),
        "--preserve-perms", "--preserve-owner", str(image),
    ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        if result.returncode < 0:
            reason = f"signal {-result.returncode}"
        else:
            reason = f"exit status {result.returncode}"
        raise SystemExit(f"fsck under generated Bionic x86_64 app policy failed ({reason}):\n{result.stdout}")

    out_chrome = extracted / "app/extra/opt/google/chrome/chrome"
    out_hardlink = extracted / "app/extra/opt/google/chrome/chrome-hardlink"
    out_link = extracted / "app/extra/opt/google/chrome/chrome-link"
    out_data = extracted / "app/extra/opt/google/chrome/resources.pak"
    assert out_chrome.read_bytes() == executable.read_bytes()
    assert out_chrome.stat().st_ino != out_hardlink.stat().st_ino, "app-data extraction must materialize hardlinks as copies"
    assert out_hardlink.read_bytes() == executable.read_bytes(), "hardlink copy content differs"
    assert out_hardlink.stat().st_mode & 0o7777 == 0o4755, oct(out_hardlink.stat().st_mode)
    assert os.readlink(out_link) == "chrome"
    assert out_chrome.stat().st_mode & 0o7777 == 0o4755, oct(out_chrome.stat().st_mode)
    assert out_data.read_bytes() == data.read_bytes()
    try:
        assert os.getxattr(out_data, "user.matonos.chrome-test") == b"xattr-preserved"
    except OSError as error:
        if error.errno not in (1, 95, 61, 524):
            raise
    print("PASS full Chrome-like EROFS extraction under Bionic's generated x86_64 app policy with linkat denied (EACCES)")
