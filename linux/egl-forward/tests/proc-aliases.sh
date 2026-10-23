#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
deps=${1:?usage: proc-aliases.sh PINNED_REGISTRY_DIR ABSOLUTE_OUTPUT_DIR}
out=${2:?usage: proc-aliases.sh PINNED_REGISTRY_DIR ABSOLUTE_OUTPUT_DIR}
python3 "$here/gen.py" "$deps/egl.xml" "$deps/gl.xml" "$out"
python3 - "$out/proc_lookup.inc" <<'PY'
import pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text()
desktop_text = pathlib.Path(sys.argv[1]).with_name("desktop_proc_lookup.inc").read_text()
expected = {
    "glBindFramebufferEXT": "mv_desktop_glBindFramebufferEXT",
    "glBindRenderbufferEXT": "mv_desktop_glBindRenderbufferEXT",
    "eglQueryString": "mv_eglQueryString",
    "eglGetProcAddress": "mv_eglGetProcAddress",
    "glBlendFuncSeparate": "mv_shared_glBlendFuncSeparate",
    "glBlendFuncSeparateEXT": "mv_shared_glBlendFuncSeparate",
    "glBlendEquation": "mv_shared_glBlendEquation",
    "glBlendEquationEXT": "mv_shared_glBlendEquation",
    "glBlendEquationSeparate": "mv_shared_glBlendEquationSeparate",
    "glBlendEquationSeparateEXT": "mv_shared_glBlendEquationSeparate",
}
for name, thunk in expected.items():
    line = f'else if (!strcmp(name, "{name}")) return (void *){thunk};'
    if line not in text:
        raise SystemExit(f"FAIL: {name} does not map to {thunk}")
    if name.startswith("gl"):
        desktop_target = {"glBlendFuncSeparateEXT":"glBlendFuncSeparate", "glBlendEquationEXT":"glBlendEquation", "glBlendEquationSeparateEXT":"glBlendEquationSeparate"}.get(name, name)
        desktop_line = f'else if (!strcmp(name, "{name}")) return (void *)mv_desktop_{desktop_target};'
        if desktop_line not in desktop_text:
            raise SystemExit(f"FAIL: desktop GLX lookup {name} does not map to its desktop thunk")
# Each API resolves against a different driver. Tentative static declarations
# with the same name silently share storage in this translation unit.
root = pathlib.Path(sys.argv[1]).parent
cache_sets = []
for filename in ("egl_thunks.inc", "gles_thunks.inc", "desktop_thunks.inc"):
    source = (root / filename).read_text()
    cache_sets.append(set(re.findall(r"static uintptr_t (mv_cache_\w+);", source)))
for i, names in enumerate(cache_sets):
    for other in cache_sets[i + 1:]:
        overlap = names & other
        if overlap:
            raise SystemExit(f"FAIL: API thunks share driver caches: {sorted(overlap)}")
print("PASS: EGL, GLES and desktop GL driver caches are disjoint")
print("PASS: GL proc aliases map to canonical TLS switching thunks")
print("PASS: desktop GLX lookup routes shared names to the desktop driver")
PY
