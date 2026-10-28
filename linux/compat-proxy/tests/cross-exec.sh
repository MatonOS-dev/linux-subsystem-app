#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
cc -no-pie -O2 -Wall -Wextra -Werror tests/cross-exec.c -o build/cross64
"$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror tests/cross-exec.c -o build/cross32.o
ld -m elf_i386 -o build/cross32 --dynamic-linker "$interpreter" build/i386-p1/entry.o build/cross32.o "$libc"
printf 'cross\n' >build/cross-file
mkdir -p build/cross-dir
chmod 0700 build/cross-dir
for arch in 64 32; do
    if [[ $arch == 64 ]]; then loader=linux-layer-v3; peer=32; else loader=linux-layer-v3-i386; peer=64; fi
    timeout 20 "build/$loader" --android-filter --trace --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
        "$PWD/build/cross$arch" "$PWD/build/cross$peer" "$PWD/build/cross$arch" \
        >"build/cross$arch.out" 2>"build/cross$arch.log"
    cat "build/cross$arch.out"
    MATON_TEST_CROSS_FILTER=1 timeout 20 "build/$loader" --android-filter \
        --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
        "$PWD/build/cross$arch" "$PWD/build/cross$peer" "$PWD/build/cross$arch" \
        >"build/cross$arch-filter.out" 2>"build/cross$arch-filter.log"
    cat "build/cross$arch-filter.out"
done

if command -v bwrap >/dev/null; then
    timeout 20 build/linux-layer-v3 --android-filter --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
        "$(command -v bwrap)" --ro-bind / / --proc /proc --dev /dev \
        "$PWD/build/cross32" "$PWD/build/cross64" "$PWD/build/cross32" >build/cross-bwrap.out 2>build/cross-bwrap.log
    cat build/cross-bwrap.out
fi

python3 tests/state-reject.py

# Inspect native startup before importing state. Ordinary static resident
# tests only observe the restored payload environment, hiding LD_* leakage.
peer_dir=$(mktemp -d "$PWD/build/resident-env.XXXXXX")
trap 'rm -rf "$peer_dir"' EXIT
cp build/linux-layer-v3-i386 "$peer_dir/linux-layer-v3-i386"
cc -nostdlib -static -no-pie -mcmodel=large -fno-stack-protector -fno-builtin \
 -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
 -O2 -Wall -Wextra -Werror tests/resident-env-peer.c arch/x86_64/entry.S \
 -o "$peer_dir/linux-layer-v3"
chmod 0755 "$peer_dir/linux-layer-v3"
MATON_TEST_REAL_RESIDENT="$PWD/build/linux-layer-v3" MATON_TEST_RESIDENT_ENV=1 \
 LD_LIBRARY_PATH=/virtual-resident-libraries LD_BIND_NOW=1 \
 timeout 20 "$peer_dir/linux-layer-v3-i386" --android-filter --trace \
 --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
 "$PWD/build/cross32" "$PWD/build/cross64" "$PWD/build/cross32" \
 >build/cross-native-env.out 2>build/cross-native-env.log
cat build/cross-native-env.out
printf 'PASS native resident excludes LD_* while both payload ABIs retain them\n'

# A static x86_64 peer must not trap the next i386 Android linker's startup.
# This models pressure-vessel's ELF64 -> ELF32 helper after an earlier peer exec.
cp build/linux-layer-v3 "$peer_dir/linux-layer-v3"
"$compiler" -nostdlib -static -Wl,-no-pie \
 -DREAL_PEER=\"$PWD/build/linux-layer-v3-i386\" tests/resident-start-peer.S \
 -o "$peer_dir/linux-layer-v3-i386"
chmod 0755 "$peer_dir/linux-layer-v3-i386"
timeout 20 "$peer_dir/linux-layer-v3" --android-filter --trace \
 --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
 "$PWD/build/cross64" "$PWD/build/cross32" "$PWD/build/cross64" \
 >build/cross-peer-start.out 2>build/cross-peer-start.log
cat build/cross-peer-start.out
printf 'PASS peer linker mmap before state import survives static resident filter\n'

# Filters from the ORIGINAL ABI survive an ABI round trip. Merely allowing
# the peer branch cannot make a bionic i386 linker safe on the return leg.
cp build/linux-layer-v3-i386 "$peer_dir/linux-layer-v3-i386"
"$compiler" -nostdlib -static -Wl,-no-pie \
 -DREAL_PEER=\"$PWD/build/linux-layer-v3-i386\" tests/resident-start-peer.S \
 -o "$peer_dir/return-linker"
chmod 0755 "$peer_dir/return-linker"
# Neither invocation installs a Flatpak/payload filter. The plain run also
# omits the synthetic Android app filter: a failure there isolates the proxy's
# own irreversible kernel routing filter from both other policies.
for mode in plain android; do
    cp build/linux-layer-v3-i386 "$peer_dir/linux-layer-v3-i386"
    "$compiler" -nostdlib -static -Wl,-no-pie \
     -DREAL_PEER=\"$PWD/build/linux-layer-v3-i386\" tests/resident-start-peer.S \
     -o "$peer_dir/return-linker"
    chmod 0755 "$peer_dir/return-linker"
    flags=()
    expected=sigsys
    if [[ $mode == android ]]; then flags+=(--android-filter); expected=sigsys; fi
    MATON_TEST_NO_APP_FILTER=$([[ $mode == plain ]] && echo 1) \
    MATON_TEST_RETURN_PEER_EXPECT="$expected" \
    MATON_TEST_RETURN_PEER="$peer_dir/return-linker" \
     MATON_TEST_RETURN_SIBLING="$peer_dir/linux-layer-v3-i386" \
     timeout 20 "$peer_dir/linux-layer-v3-i386" "${flags[@]}" --trace \
     --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
     "$PWD/build/cross32" "$PWD/build/cross64" "$PWD/build/cross32" \
     >"build/cross-return-linker-$mode.out" 2>"build/cross-return-linker-$mode.log"
    # The fixture parent checks WIFSIGNALED/SIGSYS, rather than mistaking a
    # timeout, startup error, or payload assertion for the reproduced failure.
    printf 'PASS return startup without payload filter: %s -> %s\n' "$mode" "$expected"
done
cp build/linux-layer-v3-i386 "$peer_dir/linux-layer-v3-i386"
timeout 20 "$peer_dir/linux-layer-v3-i386" --android-filter --trace \
 --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
 "$PWD/build/cross32" "$PWD/build/cross64" "$PWD/build/cross32" \
 >build/cross-return-static.out 2>build/cross-return-static.log
cat build/cross-return-static.out
printf 'PASS freestanding i386 round-trip control\n'
