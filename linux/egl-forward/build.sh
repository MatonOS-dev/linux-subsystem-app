#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh ABSOLUTE_OUTPUT_DIR DEPS_DIR NDK}
deps=${2:?pinned EGL/GLES registries and libglvnd headers required}
ndk=${3:?Android NDK r30 required for EGL/GLES headers}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
case "$out/" in "$here/"*|"$(git -C "$here" rev-parse --show-toplevel)/"*) echo 'output must be outside repository' >&2; exit 2;; esac
mkdir -p "$out"
python3 "$here/gen.py" "$deps/egl.xml" "$deps/gl.xml" "$out/gen"
cc=${CC:-clang-21}
ndk_headers="$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"
mkdir -p "$deps/include"
for dir in EGL GLES2 GLES3 KHR; do cp -R "$ndk_headers/$dir" "$deps/include/"; done
mkdir -p "$deps/include/GL"
cat > "$deps/include/GL/gl.h" <<'EOF'
/* libglvnd's shared ABI header only consumes GLboolean from GL/gl.h here. */
typedef unsigned char GLboolean;
EOF
flags=(-std=c11 -O2 -g -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$deps/include" -I"$out/gen" -I"$here/arch/x86_64" -I"$here" -I"$here/../vulkan-forward" -I"$here/../wayland-client")
"$cc" --target=x86_64-linux-gnu "${flags[@]}" -shared -Wl,--no-undefined,-Bsymbolic,-z,now,-z,noexecstack "$here/runtime.c" "$here/../vulkan-forward/wayland_resolve.c" "$here/../wayland-client/wayland_client.c" "$here/egl_vendor.c" -o "$out/libEGL_maton.so"
readelf=${READELF:-llvm-readelf-21}
"$readelf" -d "$out/libEGL_maton.so" > "$out/dynamic.txt"
if rg -q NEEDED "$out/dynamic.txt"; then echo 'unexpected DT_NEEDED' >&2; exit 1; fi
"$readelf" --dyn-syms "$out/libEGL_maton.so" > "$out/symbols.txt"
exports=$(awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND" { print $8 }' "$out/symbols.txt" | sort -u)
[[ "$exports" == __egl_Main ]] || { echo "unexpected exports: $exports" >&2; exit 1; }
for arch in aarch64 riscv64; do
  target="$arch-linux-gnu"
  "$cc" --target="$target" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$deps/include" -I"$out/gen" -I"$here/arch/$arch" -I"$here" -c "$here/runtime.c" -o "$out/runtime-$arch.o"
  "$cc" --target="$target" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$deps/include" -I"$out/gen" -I"$here/arch/$arch" -I"$here" -I"$here/../vulkan-forward" -I"$here/../wayland-client" -c "$here/../wayland-client/wayland_client.c" -o "$out/wayland-$arch.o"
  "$cc" --target="$target" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$deps/include" -I"$out/gen" -I"$here/arch/$arch" -I"$here" -I"$here/../vulkan-forward" -I"$here/../wayland-client" -c "$here/../vulkan-forward/wayland_resolve.c" -o "$out/resolver-$arch.o"
  "$cc" --target="$target" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$deps/include" -I"$out/gen" -I"$here/arch/$arch" -I"$here" -I"$here/../vulkan-forward" -I"$here/../wayland-client" -c "$here/egl_vendor.c" -o "$out/vendor-$arch.o"
done
cp "$here/10_maton.json" "$out/10_maton.json"
cat "$out/gen/coverage.txt"
echo 'PASS: freestanding; no DT_NEEDED; one export; arm64/riscv64 stub compile'
