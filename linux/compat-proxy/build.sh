#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 gen-policy.py
cc -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c raw.S entry.S -o build/linux-layer-v3
cc -DPROXY_LIBRARY -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c loader.c -o build/loader.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c proxy.c -o build/proxy.o
cc -c raw.S -o build/raw.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c vfs.c -o build/vfs.o
ar rcs build/liblinux-layer-v3.a build/vfs.o build/loader.o build/proxy.o build/raw.o
