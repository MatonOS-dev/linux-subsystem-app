#!/usr/bin/env python3
"""Compile-time comparison of Flatpak SCMP_SYS values with libseccomp's x86_64 table.
Usage: test.py NDK FLATPAK_SOURCE LIBSECCOMP_SOURCE
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ndk, flatpak, seccomp = map(Path, sys.argv[1:])
source = (flatpak / 'common/flatpak-run.c').read_text()
setup = source[source.index('setup_seccomp ('):source.index('flatpak_run_setup_usr_links (')]
names = sorted(set(re.findall(r'SCMP_SYS\s*\(\s*(\w+)\s*\)', setup)))
rows = {}
for line in (seccomp / 'src/syscalls.perf').read_text().splitlines():
    columns = line.split(',')
    if len(columns) == 17:
        rows[columns[0]] = columns[3]  # name,index,x86,x86_64,x32,...
assert names and all(name in rows for name in names)
code = '#include <seccomp.h>\n#include "flatpak-syscalls-private.h"\n'
code += '_Static_assert(sizeof(void*) == 8, "LP64 required");\n'
code += '_Static_assert(SCMP_ARCH_X86_64 == 0xc000003e, "audit architecture");\n'
for name in names:
    code += f'_Static_assert(SCMP_SYS({name}) == {rows[name]}, "{name} numbering");\n'
with tempfile.TemporaryDirectory(prefix='maton-seccomp-') as temp:
    test = Path(temp) / 'syscalls.c'
    test.write_text(code)
    subprocess.run([str(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang'),
                    '-std=c11', '-Wall', '-Wextra', '-Werror', '-march=x86-64-v2', '-fsyntax-only',
                    '-I'+str(seccomp / 'include'), '-I'+str(flatpak / 'common'), str(test)], check=True)
print(f'PASS: {len(names)} Flatpak filter syscalls match libseccomp x86_64 numbering under NDK/bionic')
