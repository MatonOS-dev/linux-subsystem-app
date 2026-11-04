#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:?output directory required}
mkdir -p "$OUT"
cc -shared -nostdlib -Wl,-soname,libmaton_vulkan.so -o "$OUT/libmaton_vulkan.so" -x c - <<'EOF'
void maton_test_symbol(void) {}
EOF
cc -m32 -march=corei7 -shared -nostdlib -Wl,-soname,libmaton_vulkan_i386.so -o "$OUT/libmaton_vulkan_i386.so" -x c - <<'EOF'
void maton_i386_test_symbol(void) {}
EOF
python3 "$ROOT/linux/gl-maton/build.py" "$OUT/extension" "$OUT/libmaton_vulkan.so" \
  --vulkan-i386 "$OUT/libmaton_vulkan_i386.so" --branch 26.08
python3 "$ROOT/linux/gl-maton/tests/verify.py" "$OUT/extension"
echo 'PASS Maton Flatpak GL extension layout, metadata and OSTree pack'
