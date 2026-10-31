#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/trap-cost
cc -O2 -Wall -Wextra -Werror tests/trap-cost.c -o build/trap-cost/bench64
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/matonos-android-sdk/ndk/28.2.13676358}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android35-clang
"$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror tests/trap-cost.c -o build/trap-cost/bench32.o
cc -m32 -c tests/i386-entry.S -o build/trap-cost/entry32.o
ld -m elf_i386 -o build/trap-cost/bench32 --dynamic-linker /lib/ld-linux.so.2 \
  build/trap-cost/entry32.o build/trap-cost/bench32.o /usr/lib32/libc.so.6
fixture=$(mktemp -d "$PWD/build/trap-cost/fs.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
printf x >"$fixture/file"
ln -s file "$fixture/symlink"
if [[ ${MATON_TRAP_COST_STORE:-0} == 1 ]]; then touch "$fixture/.v3-store-root"; fi
printf 'arch,route,op,us_per_call\n'
for arch in 64 32; do
  bench=build/trap-cost/bench$arch
  loader=build/linux-layer-v3; [[ $arch == 32 ]] && loader=build/linux-layer-v3-i386
  for op in getcwd close access stat openat readlink unlinkat linkat mkdirat; do
    case $op in getcwd|close) src=-; dst=-; iterations=10000;;
      access|stat|openat) src="$fixture/file"; dst=/bench/file; iterations=5000;;
      readlink) src="$fixture/symlink"; dst=/bench/symlink; iterations=5000;;
      unlinkat) src="$fixture/unlink"; dst=/bench/unlink; iterations=1000; printf x >"$fixture/unlink";;
      linkat) src="$fixture/file"; dst=/bench/file; iterations=1000; [[ ${MATON_TRAP_COST_STORE:-0} == 1 ]] && iterations=10;;
      mkdirat) src="$fixture/dir"; dst=/bench/dir; iterations=1000;; esac
    for route in native proxied; do
      if [[ $route == native ]]; then out=$("$bench" "$op" "$iterations" "$src" "$fixture/target");
      else out=$(MATON_PROXY_REWRITE=0 "$loader" --android-filter --map "$fixture" /bench "$bench" "$op" "$iterations" "$dst" /bench/target); fi
      printf '%s,%s,%s,%s\n' "$arch" "$route" "$op" "${out##*us_per_call=}" | sed 's/ checksum=.*//'
    done
  done
done
