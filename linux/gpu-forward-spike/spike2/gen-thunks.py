#!/usr/bin/env python3
"""Generate typed, TLS-bracketed Android-loader calls, NOT an ICD adapter.

vk.xml is the sole command list. Protect macros come from registry platforms
and extension metadata. Output intentionally keeps a separate symbol namespace:
returning these functions to a desktop ICD loader would corrupt dispatch handles.
Resolve proc addresses in a separate Android-FS scope before invoking a thunk.
"""
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
platforms = {p.get('name'): p.get('protect') for p in root.findall('./platforms/platform')}
protect = {}
core = {c.get('name') for f in root.findall('feature') if 'vulkan' in f.get('api','').split(',')
        for c in f.findall('./require/command')}
for e in root.findall('./extensions/extension'):
    if 'vulkan' not in e.get('supported','').split(','):
        continue
    guard = platforms.get(e.get('platform'))
    if e.get('provisional') == 'true':
        guard = 'VK_ENABLE_BETA_EXTENSIONS'
    for c in e.findall('./require/command'):
        protect.setdefault(c.get('name'), set()).add(guard)
commands = {c.findtext('proto/name'): c for c in root.findall('./commands/command') if c.find('proto') is not None}
print('/* Generated from vk.xml; do not edit. */\n#include <vulkan/vulkan.h>\n#include "thunks.h"')
count = 0
for node in root.findall('./commands/command'):
    name = node.get('name') or node.findtext('proto/name')
    if name not in core and name not in protect:
        continue
    c = commands[node.get('alias')] if node.get('alias') else node
    if c.get('api') and 'vulkan' not in c.get('api').split(','):
        continue
    guards = protect.get(name, {None})
    guard = None if name in core or None in guards else ' || '.join('defined('+g+')' for g in sorted(guards))
    if guard: print('#if '+guard)
    params = [p for p in c.findall('param') if not p.get('api') or 'vulkan' in p.get('api').split(',')]
    declaration = ', '.join(''.join(p.itertext()) for p in params) or 'void'
    args = ', '.join(p.findtext('name') for p in params)
    ret = c.findtext('proto/type')
    print(f'{ret} maton_{name}({declaration}) {{')
    print(f'  struct maton_call call = maton_enter("{name}");')
    prefix = '' if ret == 'void' else f'{ret} result = '
    print(f'  {prefix}((PFN_{name})call.target)({args});')
    print('  maton_leave(&call);')
    if ret != 'void': print('  return result;')
    print('}')
    if guard: print('#endif')
    count += 1
print(f'/* {count} registry commands emitted (including guarded platforms). */')
