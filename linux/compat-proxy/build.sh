#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
backend=arch/x86_64
python3 gen-policy.py
chmod 0755 build
cc -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c "$backend/raw.S" "$backend/entry.S" -o build/linux-layer-v3
cc -DPROXY_LIBRARY -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c loader.c -o build/loader.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c proxy.c -o build/proxy.o
cc -c "$backend/raw.S" -o build/raw.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c vfs.c -o build/vfs.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c net.c -o build/net.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c links.c -o build/links.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c sysvipc.c -o build/sysvipc.o
cc -mcmodel=large -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c state.c -o build/state.o
ar rcs build/liblinux-layer-v3.a build/state.o build/sysvipc.o build/links.o build/net.o build/vfs.o build/loader.o build/proxy.o build/raw.o

bash ./build-i386.sh

chmod 0755 build/linux-layer-v3
