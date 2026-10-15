#!/usr/bin/env python3
"""Invert Generic.kl numeric scan mappings; first occurrence wins.

Usage: generate_android_keymap.py AOSP_ROOT > native/android_keymap.c
HID usage mappings have no evdev code and are deliberately excluded.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
layout = root / 'frameworks/base/data/keyboards/Generic.kl'
keycodes = root / 'frameworks/native/include/android/keycodes.h'
ids = dict((name, int(value)) for name, value in re.findall(
    r'AKEYCODE_(\w+)\s*=\s*(\d+)', keycodes.read_text()))
entries = {}
duplicates = []
for line in layout.read_text().splitlines():
    match = re.match(r'^key\s+(\d+)\s+(\w+)\b', line)
    if not match:
        continue
    scan, name = match.groups()
    code = ids[name]
    if code in entries:
        duplicates.append(f'{name}: keep {entries[code][0]}, ignore {scan}')
    else:
        entries[code] = (int(scan), name)
print('''/* Generated from AOSP Generic.kl and android/keycodes.h.
 * Generic.kl is Copyright (C) 2010 The Android Open Source Project,
 * licensed under Apache-2.0 (https://www.apache.org/licenses/LICENSE-2.0).
 * Regenerate: python3 native/tools/generate_android_keymap.py AOSP_ROOT
 * Numeric evdev mappings only; first mapping wins for duplicate keycodes.
 * HID usage entries cannot supply an evdev code and are excluded.
 */
#include "android_keymap.h"
#include <stddef.h>

static const struct { int android; int evdev; } keymap[] = {''')
for code, (scan, name) in sorted(entries.items()):
    print(f'  {{ {code}, {scan} }}, /* KEYCODE_{name} */')
print('''};

int maton_android_keycode_to_evdev(int keycode) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); ++i)
    if (keymap[i].android == keycode) return keymap[i].evdev;
  return -1;
}

/* Duplicate choices in Generic.kl file order:''')
for choice in duplicates:
    print(f' * {choice}')
print(' */')
