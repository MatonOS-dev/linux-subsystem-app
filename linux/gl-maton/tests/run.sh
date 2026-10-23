#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:?output directory required}
mkdir -p "$OUT"
cc -shared -nostdlib -Wl,-soname,libmaton_vulkan.so -o "$OUT/libmaton_vulkan.so" -x c - <<'EOF'
void maton_test_symbol(void) {}
EOF
cc -shared -nostdlib -Wl,-soname,libEGL_maton.so -o "$OUT/libEGL_maton.so" -x c - <<'EOF'
void maton_test_symbol(void) {}
EOF
cc -shared -nostdlib -Wl,-soname,libGLX_maton.so.0 -o "$OUT/libGLX_maton.so.0" -x c - <<'EOF'
void maton_test_symbol(void) {}
EOF
rm -f "$OUT/libGLX_maton.so"
cp "$OUT/libGLX_maton.so.0" "$OUT/libGLX_maton.so"
cc -m32 -march=corei7 -shared -nostdlib -Wl,-soname,libmaton_vulkan_i386.so -o "$OUT/libmaton_vulkan_i386.so" -x c - <<'EOF'
void maton_i386_test_symbol(void) {}
EOF
cc -m32 -march=corei7 -shared -nostdlib -Wl,-soname,libEGL_maton.so -o "$OUT/libEGL_maton_i386.so" -x c - <<'EOF'
void maton_i386_egl_test_symbol(void) {}
EOF
cc -m32 -march=corei7 -shared -nostdlib -Wl,-soname,libGLX_maton.so.0 -o "$OUT/libGLX_maton_i386.so.0" -x c - <<'EOF'
void maton_i386_glx_test_symbol(void) {}
EOF
python3 "$ROOT/linux/gl-maton/build.py" "$OUT/extension" \
  "$OUT/libmaton_vulkan.so" "$OUT/libEGL_maton.so" --glx "$OUT/libGLX_maton.so.0" \
  --vulkan-i386 "$OUT/libmaton_vulkan_i386.so" --egl-i386 "$OUT/libEGL_maton_i386.so" \
  --glx-i386 "$OUT/libGLX_maton_i386.so.0" --branch 26.08
python3 "$ROOT/linux/gl-maton/tests/verify.py" "$OUT/extension"
echo 'PASS Maton Flatpak GL extension layout, metadata and OSTree pack'
