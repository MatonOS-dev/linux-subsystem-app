#!/usr/bin/env python3
"""Generate x86_64 app syscall numbers using AOSP's own parser and inputs."""
import pathlib, sys, re, hashlib
root = pathlib.Path('/mnt/data/aosp/bionic/libc')
sys.path.insert(0, str(root / 'tools'))
from genseccomp import load_syscall_names_from_file, merge_names
base = load_syscall_names_from_file(root / 'SYSCALLS.TXT', 'x86_64')
a, b = set(), set()
inputs = ['SYSCALLS.TXT', 'SECCOMP_ALLOWLIST_COMMON.TXT', 'SECCOMP_ALLOWLIST_APP.TXT', 'SECCOMP_BLOCKLIST_COMMON.TXT', 'SECCOMP_BLOCKLIST_APP.TXT']
for name in inputs[1:]:
    (b if 'BLOCKLIST' in name else a).update(load_syscall_names_from_file(root / name, 'x86_64'))
table = root / 'kernel/uapi/asm-x86/asm/unistd_64.h'
numbers = {name: int(number) for name, number in re.findall(
    r'^#define __NR_(\w+)\s+(\d+)$', table.read_text(), re.M)}
names = merge_names(base, a, b)
# Handler internals must never issue a syscall rejected by the inherited filter.
required = {'write', 'openat', 'pread64', 'close', 'mmap', 'mprotect', 'munmap',
            'getrandom', 'getuid', 'geteuid', 'getgid', 'getegid', 'gettid',
            'clone', 'rt_sigaction', 'rt_sigprocmask', 'sigaltstack',
            'rt_sigreturn', 'prctl', 'seccomp', 'arch_prctl', 'exit_group'}
if required - names:
    raise SystemExit(f'Handler syscall(s) blocked by app policy: {required - names}')
missing = names - numbers.keys()
if missing:
    raise SystemExit(f'Unknown numbers: {missing}')
out = pathlib.Path(__file__).parent / 'build/app-policy.h'
out.parent.mkdir(exist_ok=True)
out.write_text('/* Generated from checked-out AOSP; never hand edit. */\nstatic const int android_allowed[] = {\n' + ','.join(str(numbers[n]) for n in sorted(names)) + '\n};\n')
(pathlib.Path(__file__).parent / 'build/policy-inputs.sha256').write_text(''.join(f'{hashlib.sha256((root/n).read_bytes()).hexdigest()}  {root/n}\n' for n in inputs + ['kernel/uapi/asm-x86/asm/unistd_64.h']))
# Exhaustive complement of the checked-out table, not a handpicked legacy list.
translations = {
 'io_pgetevents':'io_getevents + temporary signal mask',
 'lstat':'newfstatat', 'poll':'ppoll', 'access':'faccessat', 'pipe':'pipe2',
 'select':'pselect6', 'dup2':'dup3 (same-fd uses fcntl)', 'pause':'ppoll',
 'alarm':'setitimer', 'accept':'accept4', 'fork':'clone', 'getdents':'getdents64 (layout conversion)',
 'rename':'renameat', 'mkdir':'mkdirat', 'rmdir':'unlinkat', 'creat':'openat',
 'link':'linkat', 'unlink':'unlinkat', 'symlink':'symlinkat', 'chmod':'fchmodat',
 'chown':'fchownat', 'lchown':'fchownat', 'getpgrp':'getpgid', 'utime':'utimensat',
 'mknod':'mknodat', 'time':'clock_gettime', 'epoll_create':'epoll_create1',
 'epoll_wait':'epoll_pwait', 'utimes':'utimensat', 'inotify_init':'inotify_init1',
 'futimesat':'utimensat', 'signalfd':'signalfd4', 'eventfd':'eventfd2',
}
emulated = {'setuid','setgid','setreuid','setregid','setgroups','setresgid',
            'setfsuid','setfsgid','chroot','mount','umount2'}
import json
rows = []
for name, number in sorted(numbers.items(), key=lambda pair: pair[1]):
    if name in names:
        continue
    status = 'translate' if name in translations else 'virtual' if name in emulated else 'ENOSYS'
    rows.append({'number':number,'name':name,'status':status,'equivalent':translations.get(name,'')})
(out.parent / 'trapped-syscalls.json').write_text(json.dumps(rows,indent=2)+'\n')
(out.parent / 'trapped-syscalls.md').write_text(
    '# Android app-policy complement (generated)\n\n| Number | Syscall | Handler | Equivalent |\n|---:|---|---|---|\n' +
    ''.join(f"| {r['number']} | {r['name']} | {r['status']} | {r['equivalent']} |\n" for r in rows))
