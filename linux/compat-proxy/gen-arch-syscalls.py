#!/usr/bin/env python3
"""Regenerate common operation IDs and the x86_64 kernel number adapter.

Read the checked-out UAPI as an ABI specification; no kernel code is copied.
Existing IDs are process-local, never serialized or sent to another loader.
"""
from pathlib import Path
import re
root = Path(__file__).resolve().parent
spec = Path('/mnt/data/aosp/bionic/libc/kernel/uapi/asm-x86/asm/unistd_64.h')
names = sorted(set(re.findall(r'^#define __NR_(\w+)\s+\d+$', spec.read_text(), re.M)))
(root/'arch/syscalls.h').write_text('/* Internal operation IDs. These are never kernel syscall numbers. */\nenum proxy_syscall {\n    PROXY_SC_unknown = -1,\n' + ''.join('    PROXY_SC_'+n+',\n' for n in names) + '    PROXY_SC_semtimedop_time64,\n    PROXY_SC_count\n};\n')

def adapter(comment, table, aliases=None, extra_names=(), compact=False):
    """Render encode/decode from one ABI number table and the shared op IDs."""
    aliases = aliases or {}
    text = comment + '\n'
    for function in ('encode', 'decode'):
        if compact:
            text += f'static inline long arch_{function}_syscall(long nr) {{ switch(nr) {{\n'
        else:
            text += f'static inline long arch_{function}_syscall(long nr) {{\n    switch(nr) {{\n'
        for name in names + list(extra_names):
            wire = aliases.get(name, name)
            if wire not in table:
                continue
            number = table[wire]
            source = f'PROXY_SC_{name}' if function == 'encode' else str(number)
            target = str(number) if function == 'encode' else f'PROXY_SC_{name}'
            text += f'{"" if compact else "    "}case {source}: return {target};\n'
        if compact:
            text += 'default: return -1; }}\n'
        else:
            text += '    default: return -1;\n    }\n}\n'
    return text


def read_table(path):
    return {name: int(number) for name, number in re.findall(
        r'^#define __NR_(\w+)\s+(\d+)$', path.read_text(), re.M)}


native = read_table(spec)
(root/'arch/x86_64/syscalls.h').write_text(adapter(
    '/* Fixed x86_64 ABI numbers, independent of the compiling architecture. */', native))

compat = read_table(spec.with_name('unistd_32.h'))
alias = {'mmap':'mmap2','newfstatat':'fstatat64','stat':'stat64','lstat':'lstat64','fstat':'fstat64',
 'fcntl':'fcntl64','select':'_newselect','getrlimit':'ugetrlimit'}
for name in ['getuid','geteuid','getgid','getegid','setuid','setgid','setreuid','setregid',
 'setresuid','setresgid','setfsuid','setfsgid','setgroups','getgroups','getresuid','getresgid',
 'chown','fchown','lchown']:
    alias[name] = name+'32'
(root/'arch/i386/syscalls.h').write_text(adapter(
    '/* i386 kernel numbers from the checked-out UAPI specification. */', compat,
    aliases=alias, extra_names=('semtimedop_time64',), compact=True))
