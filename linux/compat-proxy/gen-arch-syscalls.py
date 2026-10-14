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
native = {n:int(v) for n,v in re.findall(r'^#define __NR_(\w+)\s+(\d+)$',spec.read_text(),re.M)}
text = '/* Fixed x86_64 ABI numbers, independent of the compiling architecture. */\n'
for function in ['encode', 'decode']:
    text += 'static inline long arch_'+function+'_syscall(long nr) {\n    switch(nr) {\n'
    for name in names:
        source = 'PROXY_SC_'+name if function=='encode' else str(native[name])
        target = str(native[name]) if function=='encode' else 'PROXY_SC_'+name
        text += f'    case {source}: return {target};\n'
    text += '    default: return -1;\n    }\n}\n'
(root/'arch/x86_64/syscalls.h').write_text(text)

compat_spec = spec.with_name('unistd_32.h')
compat = {n:int(v) for n,v in re.findall(r'^#define __NR_(\w+)\s+(\d+)$',compat_spec.read_text(),re.M)}
alias = {'mmap':'mmap2','newfstatat':'fstatat64','stat':'stat64','lstat':'lstat64','fstat':'fstat64',
 'fcntl':'fcntl64','select':'_newselect'}
for name in ['getuid','geteuid','getgid','getegid','setuid','setgid','setreuid','setregid',
 'setresuid','setresgid','setfsuid','setfsgid','setgroups','getgroups','getresuid','getresgid',
 'chown','fchown','lchown']:
    alias[name] = name+'32'
text = '/* i386 kernel numbers from the checked-out UAPI specification. */\n'
for fn in ['encode','decode']:
    text += 'static inline long arch_'+fn+'_syscall(long nr) { switch(nr) {\n'
    for name in names + ['semtimedop_time64']:
        wire = alias.get(name,name)
        if wire not in compat: continue
        number = compat[wire]
        source = 'PROXY_SC_'+name if fn=='encode' else str(number)
        target = str(number) if fn=='encode' else 'PROXY_SC_'+name
        text += f'case {source}: return {target};\n'
    text += 'default: return -1; }}\n'
(root/'arch/i386/syscalls.h').write_text(text)
