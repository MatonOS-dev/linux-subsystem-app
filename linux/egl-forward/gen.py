#!/usr/bin/env python3
"""Generate typed EGL 1.5 and GLES 2.0–3.2 TLS-switching thunks."""
import hashlib
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

egl_xml, gl_xml, outdir = map(pathlib.Path, sys.argv[1:4])
outdir.mkdir(parents=True, exist_ok=True)
pins = json.loads((pathlib.Path(__file__).parent / "pins.json").read_text())
for path, key in ((egl_xml, "EGL-Registry"), (gl_xml, "OpenGL-Registry")):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != pins[key]["sha256"]:
        raise SystemExit(f"{path}: SHA-256 {actual} does not match pins.json")

def decl(node):
    return "".join(node.itertext()).strip()

def commands(path, api, max_version):
    root = ET.parse(path).getroot()
    byname = {}
    for c in root.findall("./commands/command"):
        if c.get("alias"):
            continue
        name = c.findtext("./proto/name")
        if not name:
            continue
        byname[name] = c
    selected = set()
    for feature in root.findall("feature"):
        if api not in feature.get("api", "").split(","):
            continue
        if tuple(map(int, feature.get("number", "0").split("."))) <= max_version:
            for req in feature.findall("require"):
                if req.get("api") and api not in req.get("api").split(","):
                    continue
                selected.update(x.get("name") for x in req.findall("command"))
    aliases = {}
    for c in root.findall("./commands/command"):
        if c.get("alias") and c.get("name") in selected:
            aliases[c.get("name")] = c.get("alias")
    # Promoted aliases can be listed in a feature while their canonical command
    # is listed only in an extension. The alias declaration gives its target.
    for name in list(selected):
        if name not in byname and name in aliases:
            selected.remove(name)
            selected.add(aliases[name])
    return [(name, byname[name]) for name in sorted(selected) if name in byname]

egl = [(n, c) for n, c in commands(egl_xml, "egl", (1, 5)) if n not in ("eglQueryString", "eglGetProcAddress")]
gles = commands(gl_xml, "gles2", (3, 2))

def emit_group(path, items, prefix, library):
    lines = ["/* Generated from the pinned Khronos API registries. */"]
    names = []
    for name, cmd in items:
        ret = decl(cmd.find("proto"))
        ret = ret[:ret.rfind(name)].strip()
        params = [decl(p) for p in cmd.findall("param")]
        args = ", ".join(p.findtext("name") for p in cmd.findall("param"))
        proto = ", ".join(params) if params else "void"
        internal = prefix + name
        unsupported = name in {
            "eglCreateImage", "eglCreatePbufferFromClientBuffer",
            "eglCreatePixmapSurface", "eglCreatePlatformPixmapSurface",
            "eglCreatePlatformWindowSurface", "eglCreateWindowSurface",
            # Do not let the Android driver invoke a Linux callback on bionic TLS.
            "glDebugMessageCallback",
        }
        if unsupported:
            lines += [f"static {ret} {internal}({proto}) {{"]
            lines += [f"    (void){p.findtext('name')};" for p in cmd.findall("param")]
            lines += ["    return (" + ret + ")0;", "}"]
            names.append(name)
            continue
        lines.append(f"static uintptr_t mv_cache_{name};")
        # Function pointer declarators in Khronos parameters are uncommon but
        # retain the registry declaration as the call prototype when present.
        lines += [f"static {ret} {internal}({proto}) {{",
                  "    struct mv_scope scope;",
                  "    if (!mv_ready() || !mv_enter(&scope)) { mv_fail(); " + ("return;" if ret == "void" else f"return ({ret})0;") + " }",
                  f'    typedef {ret} (*fn_t)({proto});',
                  f"    fn_t fn = (fn_t)__atomic_load_n(&mv_cache_{name}, __ATOMIC_ACQUIRE);",
                  f'    if (!fn) {{ fn = (fn_t)mv_resolve("{name}", {library}); if (fn) __atomic_store_n(&mv_cache_{name}, (uintptr_t)fn, __ATOMIC_RELEASE); }}',
                  "    if (!fn) { switch_back(&scope); mv_fail(); " + ("return;" if ret == "void" else f"return ({ret})0;") + " }",
                  *([f"    {ret} ret = fn({args});"] if ret != "void" else [f"    fn({args});"]),
                  "    switch_back(&scope);",
                  "    " + ("return;" if ret == "void" else f"return ({ret})ret;"), "}"]
        names.append(name)
    pathlib.Path(path).write_text("\n".join(lines) + "\n")
    return names

eglnames = emit_group(outdir / "egl_thunks.inc", egl, "mv_", "MV_EGL")
glesnames = emit_group(outdir / "gles_thunks.inc", gles, "mv_", "MV_GLES")
(outdir / "proc_lookup.inc").write_text(
    "/* Registry generated proc-address map. */\n" +
    "static void *mv_lookup_proc(const char *name) {\n" +
    "    if (!name) return 0;\n" +
    "    if (0) return 0;\n" +
    "".join(f'    else if (!strcmp(name, "{n}")) return (void *)mv_{n};\n' for n in eglnames + ["eglQueryString", "eglGetProcAddress"] + glesnames) +
    "    return 0;\n}\n")
(outdir / "coverage.txt").write_text(
    f"{len(eglnames)} EGL 1.5 commands; {len(glesnames)} GLES 2.0–3.2 commands\n" +
    "EGL: " + ", ".join(eglnames) + "\nGLES: " + ", ".join(glesnames) + "\n")
print(f"generated {len(eglnames)} EGL and {len(glesnames)} GLES thunks")
