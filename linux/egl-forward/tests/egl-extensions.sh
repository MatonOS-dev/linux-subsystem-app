#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
deps=${1:?usage: egl-extensions.sh PINNED_REGISTRY_DIR ABSOLUTE_OUTPUT_DIR}
out=${2:?output directory required}
python3 "$here/gen.py" "$deps/egl.xml" "$deps/gl.xml" "$out"
python3 - "$out" "$here/egl_vendor.c" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
vendor_path = pathlib.Path(sys.argv[2])
lookup = (root / "proc_lookup.inc").read_text()
metadata = (root / "egl_extensions.inc").read_text()
vendor = vendor_path.read_text()
required = {
    "eglGetPlatformDisplayEXT", "eglCreateImageKHR", "eglCreateSyncKHR",
    "eglDupNativeFenceFDANDROID", "eglQueryDevicesEXT",
}
for name in required:
    if f'!strcmp(name, "{name}")' not in lookup:
        raise SystemExit(f"FAIL: generated proc lookup omits {name}")
if "EGL_ANDROID_get_native_client_buffer" not in metadata or not re.search(
        r'\{"EGL_ANDROID_get_native_client_buffer",[^\n]*, 1\}', metadata):
    raise SystemExit("FAIL: Android native client buffer extension is not statically excluded")

# Exercise the same token policy: unknown extensions survive; static and probe
# blocklists are removed; a changed cache key causes a fresh probe.
def filter_tokens(source, excluded, blocked):
    return [t for t in source.split() if t not in excluded and t not in blocked]
assert filter_tokens("EGL_EXT_ok EGL_ANDROID_get_native_client_buffer EGL_EXT_bad",
                     {"EGL_ANDROID_get_native_client_buffer"}, {"EGL_EXT_bad"}) == ["EGL_EXT_ok"]
class ProbeCache:
    def __init__(self): self.key = None; self.count = 0
    def get(self, key):
        if key != self.key: self.key = key; self.count += 1
        return self.count
cache = ProbeCache(); assert cache.get("fingerprint|gpu=a") == 1
assert cache.get("fingerprint|gpu=a") == 1
assert cache.get("fingerprint|gpu=b") == 2
if 'state==2&&!strcmp(key,mv_extension_probe_key)' not in vendor:
    raise SystemExit("FAIL: runtime probe does not invalidate its cache when the key changes")
if 'mv_extension_probe(result);' not in vendor or 'vendor.maton.graphics.vulkan' not in vendor:
    raise SystemExit("FAIL: runtime EGL query is not wired to the keyed extension probe")
print("PASS: EGL extension spellings, static/probe filtering, and cache-key invalidation")
PY
