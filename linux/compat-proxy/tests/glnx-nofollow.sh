#!/usr/bin/env bash
# Optional integration with an existing upstream libglnx source tree.
set -euo pipefail
cd "$(dirname "$0")/.."
src=${MATON_TEST_GLNX_SOURCE:?Set MATON_TEST_GLNX_SOURCE to libglnx sources}
mkdir -p build/glnx-host
cat > build/glnx-host/libglnx-config.h <<'CONFIG'
#pragma once
#define HAVE_DECL_COPY_FILE_RANGE 1
#define HAVE_DECL_MEMFD_CREATE 1
#define HAVE_DECL_NAME_TO_HANDLE_AT 1
#define HAVE_DECL_RENAMEAT2 1
CONFIG
for unit in chase fdio errors; do
    cc -D_GNU_SOURCE -O2 -ffunction-sections -fdata-sections \
        -Ibuild/glnx-host -I"$src" $(pkg-config --cflags gio-unix-2.0) \
        -c "$src/glnx-$unit.c" -o "build/glnx-host/$unit.o"
done
cc -no-pie -O2 -Wl,--gc-sections -Ibuild/glnx-host -I"$src" $(pkg-config --cflags gio-unix-2.0) \
    tests/glnx-nofollow.c build/glnx-host/{chase,fdio,errors}.o \
    $(pkg-config --libs gio-unix-2.0) -o build/glnx-nofollow
fixture=$(mktemp -d "$PWD/build/glnx-fixture.XXXXXX")
trap 'rm -r "$fixture"' EXIT
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/glnx-nofollow" "$fixture"
