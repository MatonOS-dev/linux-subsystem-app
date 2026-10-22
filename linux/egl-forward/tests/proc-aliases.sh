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
    "eglQueryString": "mv_eglQueryString",
    "eglGetProcAddress": "mv_eglGetProcAddress",
    "glBlendFuncSeparate": "mv_glBlendFuncSeparate",
    "glBlendFuncSeparateEXT": "mv_glBlendFuncSeparate",
    "glBlendEquation": "mv_glBlendEquation",
    "glBlendEquationEXT": "mv_glBlendEquation",
    "glBlendEquationSeparate": "mv_glBlendEquationSeparate",
    "glBlendEquationSeparateEXT": "mv_glBlendEquationSeparate",
}
for name, thunk in expected.items():
    line = f'else if (!strcmp(name, "{name}")) return (void *){thunk};'
    if line not in text:
        raise SystemExit(f"FAIL: {name} does not map to {thunk}")
    if name.startswith("gl"):
        desktop_line = f'else if (!strcmp(name, "{name}")) return (void *)mv_desktop_{thunk.removeprefix("mv_")};'
        if desktop_line not in desktop_text:
            raise SystemExit(f"FAIL: desktop GLX lookup {name} does not map to its desktop thunk")
print("PASS: GL proc aliases map to canonical TLS switching thunks")
print("PASS: desktop GLX lookup routes shared names to the desktop driver")
PY
