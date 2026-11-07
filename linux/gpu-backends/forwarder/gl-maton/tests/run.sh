#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../../../.." && pwd)
OUT=${1:?output directory required}
mkdir -p "$OUT"
cc -shared -nostdlib -Wl,-soname,libmaton_vulkan.so -o "$OUT/libmaton_vulkan.so" -x c - <<'EOF'
void maton_test_symbol(void) {}
EOF
cc -m32 -march=corei7 -shared -nostdlib -Wl,-soname,libmaton_vulkan_i386.so -o "$OUT/libmaton_vulkan_i386.so" -x c - <<'EOF'
void maton_i386_test_symbol(void) {}
EOF
# Stand-in Zink install tree: the layout build-zink.sh produces.
fake_mesa() {
  local dir=$1; shift
  mkdir -p "$dir/lib" "$dir/share/drirc.d" "$dir/share/glvnd/egl_vendor.d"
  for lib in libgallium-0.0.0-test.so libGLX_mesa.so.0 libEGL_mesa.so.0; do
    cc "$@" -shared -nostdlib -Wl,-soname,$lib -o "$dir/lib/$lib" -x c - <<<'void maton_mesa_test_symbol(void) {}'
  done
  echo '<driconf/>' > "$dir/share/drirc.d/00-zink-defaults.conf"
  echo '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_mesa.so.0"}}' > "$dir/share/glvnd/egl_vendor.d/50_mesa.json"
}
MESA="$OUT/mesa"; fake_mesa "$MESA"
MESA_I386="$OUT/mesa-i386"; fake_mesa "$MESA_I386" -m32 -march=corei7
python3 "$ROOT/linux/gpu-backends/forwarder/gl-maton/build.py" "$OUT/extension" "$OUT/libmaton_vulkan.so" \
  --vulkan-i386 "$OUT/libmaton_vulkan_i386.so" --branch 26.08 --mesa "$MESA" --mesa-i386 "$MESA_I386"
python3 "$ROOT/linux/gpu-backends/forwarder/gl-maton/tests/verify.py" "$OUT/extension"
echo 'PASS Maton Flatpak GL extension layout, metadata and OSTree pack'
