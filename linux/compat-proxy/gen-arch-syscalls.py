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
(root/'arch/syscalls.h').write_text('/* Internal operation IDs. These are never kernel syscall numbers. */\nenum proxy_syscall {\n    PROXY_SC_unknown = -1,\n' + ''.join('    PROXY_SC_'+n+',\n' for n in names) + '    PROXY_SC_count\n};\n')
text = '/* x86_64 kernel number mapping; kept in the selected backend. */\n#include <sys/syscall.h>\n'
for function, source, target in [('encode','PROXY_SC_','SYS_'), ('decode','SYS_','PROXY_SC_')]:
    text += 'static inline long arch_'+function+'_syscall(long nr) {\n    switch(nr) {\n'
    for n in names:
        text += '#ifdef SYS_'+n+'\n    case '+source+n+': return '+target+n+';\n#endif\n'
    text += '    default: return '+('-1' if function == 'encode' else 'PROXY_SC_unknown')+';\n    }\n}\n'
(root/'arch/x86_64/syscalls.h').write_text(text)
