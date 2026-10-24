#!/usr/bin/env bash
# Shared baseline for every MatonOS ELF32 x86 build. Keep this in sync with
# the x86-64-v2 minimum supported by the rest of the subsystem.
MATON_I386_ISA_FLAGS=(-m32 -march=x86-64-v2)
