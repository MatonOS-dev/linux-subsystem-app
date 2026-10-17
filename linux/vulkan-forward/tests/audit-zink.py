#!/usr/bin/env python3
"""Classify every pinned Zink device extension against actual resident inventory."""
import ast
import pathlib
import sys
mesa, inventory = map(pathlib.Path, sys.argv[1:3])
allow = {s.split('#', 1)[0].strip() for s in (pathlib.Path(__file__).parents[1] / 'extensions.txt').read_text().splitlines()}
reported = {s.split()[0] for s in inventory.read_text().splitlines() if s.startswith('VK_')}
source = mesa / 'src/gallium/drivers/zink/zink_device_info.py'
names = sorted({n.args[0].value for n in ast.walk(ast.parse(source.read_text())) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == 'Extension' and n.args and isinstance(n.args[0], ast.Constant)})
print('| Zink extension | Availability |')
print('| --- | --- |')
for name in names:
    status = 'forwarded' if name in allow and name in reported else 'resident driver absent' if name not in reported else 'resident driver reports; hidden by ICD allowlist'
    print(f'| {name} | {status} |')
print(f'\n{len(names)} Zink device-extension names; {sum(n in allow and n in reported for n in names)} forwarded, {sum(n in reported and n not in allow for n in names)} hidden, {sum(n not in reported for n in names)} absent in resident driver.')
