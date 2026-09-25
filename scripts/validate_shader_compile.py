#!/usr/bin/env python3
"""Compile and link every engine shader against a real OpenGL driver.

Mirrors ShaderLoader.h: `#include "..."` is resolved relative to the including
file and each file is included at most once per program source.

Every stage is compiled on its own, then the vertex/fragment (and geometry)
pairs the engine actually creates are linked, so interface mismatches between
stages are reported too.

Requirements: `pip install moderngl`. On a headless Linux box run it under
Xvfb with Mesa's software rasterizer, which only advertises GL 4.5 by default:

    Xvfb :97 -screen 0 64x64x24 +extension GLX &
    DISPLAY=:97 LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.6 \
        MESA_GLSL_VERSION_OVERRIDE=460 python3 scripts/validate_shader_compile.py

Exit code is non-zero when any shader fails.
"""

from __future__ import annotations

import argparse
import ctypes
import re
import sys
from pathlib import Path

import moderngl

REPO_ROOT = Path(__file__).resolve().parent.parent
SHADER_DIR = REPO_ROOT / "shaders"

GL_FRAGMENT_SHADER = 0x8B30
GL_VERTEX_SHADER = 0x8B31
GL_GEOMETRY_SHADER = 0x8DD9
GL_COMPUTE_SHADER = 0x91B9
GL_COMPILE_STATUS = 0x8B81
GL_LINK_STATUS = 0x8B82
GL_INFO_LOG_LENGTH = 0x8B84

# Programs created with CreateShaderProgram(vert, frag[, geom]) in src/.
LINKED_PROGRAMS = [
    ("brdf_vert.glsl", "brdf_frag.glsl"),
    ("debug_bbox_vert.glsl", "debug_bbox_frag.glsl"),
    ("deferred_lighting_vert.glsl", "deferred_lighting_frag.glsl"),
    ("equirect2cube_vert.glsl", "equirect2cube_frag.glsl"),
    ("skybox_vert.glsl", "skybox_frag.glsl"),
    ("forward_transparent_vert.glsl", "forward_transparent_frag.glsl"),
    ("fullscreen_vert.glsl", "bloom_extract.glsl"),
    ("fullscreen_vert.glsl", "bloom_upsample.glsl"),
    ("fullscreen_vert.glsl", "debug_view_frag.glsl"),
    ("fullscreen_vert.glsl", "indirect_diffuse_debug_present_frag.glsl"),
    ("fullscreen_vert.glsl", "kawase_blur.glsl"),
    ("fullscreen_vert.glsl", "postprocess_frag.glsl"),
    ("fullscreen_vert.glsl", "ssao.glsl"),
    ("fullscreen_vert.glsl", "ssao_blur.glsl"),
    ("fullscreen_vert.glsl", "taa_resolve.glsl"),
    ("gbuffer_vert.glsl", "gbuffer_frag.glsl"),
    ("gui_unified_vert.glsl", "gui_unified_frag.glsl"),
    ("gui_vert.glsl", "gui_frag.glsl"),
    ("irradiance_convolution_vert.glsl", "irradiance_convolution_frag.glsl"),
    ("lpv_debug_vert.glsl", "lpv_debug_frag.glsl"),
    ("lpv_rsm_vert.glsl", "lpv_rsm_frag.glsl"),
    ("lpv_voxelize_vert.glsl", "lpv_voxelize_frag.glsl", "lpv_voxelize_geom.glsl"),
    ("prefilter_vert.glsl", "prefilter_frag.glsl"),
    ("shadow_vert.glsl", "shadow_frag.glsl"),
    ("surfel_gi/debug_surfels.vert", "surfel_gi/debug_surfels.frag"),
    ("velocity_vert.glsl", "velocity_frag.glsl"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')


def load_with_includes(path: Path, seen: set[Path] | None = None) -> str:
    seen = set() if seen is None else seen
    path = path.resolve()
    if path in seen:
        return ""
    seen.add(path)
    out = []
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    # latin-1 round-trips arbitrary bytes, so the driver sees exactly what the engine sends.
    for line in raw.decode("latin-1").splitlines():
        match = INCLUDE_RE.match(line)
        if match:
            out.append(load_with_includes(path.parent / match.group(1), seen))
        else:
            out.append(line)
    return "\n".join(out) + "\n"


def stage_for(path: Path) -> int | None:
    name = path.name
    if name.endswith((".comp",)) or name.endswith("_comp.glsl"):
        return GL_COMPUTE_SHADER
    if name.endswith(".vert") or name.endswith("_vert.glsl"):
        return GL_VERTEX_SHADER
    if name.endswith("_geom.glsl") or name.endswith(".geom"):
        return GL_GEOMETRY_SHADER
    if name.endswith(".frag") or name.endswith("_frag.glsl"):
        return GL_FRAGMENT_SHADER
    return None


class GL:
    """Minimal raw GL entry points; moderngl cannot compile lone stages."""

    def __init__(self) -> None:
        lib = ctypes.CDLL("libGL.so.1")
        get_proc = lib.glXGetProcAddressARB
        get_proc.restype = ctypes.c_void_p
        get_proc.argtypes = [ctypes.c_char_p]

        def fn(name, restype, *argtypes):
            ptr = get_proc(name.encode())
            if not ptr:
                raise RuntimeError(f"missing GL entry point {name}")
            return ctypes.CFUNCTYPE(restype, *argtypes)(ptr)

        u, i, p = ctypes.c_uint, ctypes.c_int, ctypes.c_void_p
        self.CreateShader = fn("glCreateShader", u, u)
        self.ShaderSource = fn("glShaderSource", None, u, i, ctypes.POINTER(ctypes.c_char_p), p)
        self.CompileShader = fn("glCompileShader", None, u)
        self.GetShaderiv = fn("glGetShaderiv", None, u, u, ctypes.POINTER(i))
        self.GetShaderInfoLog = fn("glGetShaderInfoLog", None, u, i, p, ctypes.c_char_p)
        self.DeleteShader = fn("glDeleteShader", None, u)
        self.CreateProgram = fn("glCreateProgram", u)
        self.AttachShader = fn("glAttachShader", None, u, u)
        self.LinkProgram = fn("glLinkProgram", None, u)
        self.GetProgramiv = fn("glGetProgramiv", None, u, u, ctypes.POINTER(i))
        self.GetProgramInfoLog = fn("glGetProgramInfoLog", None, u, i, p, ctypes.c_char_p)
        self.DeleteProgram = fn("glDeleteProgram", None, u)

    def compile(self, stage: int, source: str) -> tuple[int, str]:
        shader = self.CreateShader(stage)
        src = ctypes.c_char_p(source.encode("latin-1"))
        self.ShaderSource(shader, 1, ctypes.byref(src), None)
        self.CompileShader(shader)
        ok = ctypes.c_int(0)
        self.GetShaderiv(shader, GL_COMPILE_STATUS, ctypes.byref(ok))
        log_len = ctypes.c_int(0)
        self.GetShaderiv(shader, GL_INFO_LOG_LENGTH, ctypes.byref(log_len))
        buf = ctypes.create_string_buffer(max(log_len.value, 1))
        self.GetShaderInfoLog(shader, len(buf), None, buf)
        log = buf.value.decode(errors="replace").strip()
        if not ok.value:
            self.DeleteShader(shader)
            return 0, log
        return shader, log

    def link(self, shaders: list[int]) -> tuple[bool, str]:
        program = self.CreateProgram()
        for shader in shaders:
            self.AttachShader(program, shader)
        self.LinkProgram(program)
        ok = ctypes.c_int(0)
        self.GetProgramiv(program, GL_LINK_STATUS, ctypes.byref(ok))
        log_len = ctypes.c_int(0)
        self.GetProgramiv(program, GL_INFO_LOG_LENGTH, ctypes.byref(log_len))
        buf = ctypes.create_string_buffer(max(log_len.value, 1))
        self.GetProgramInfoLog(program, len(buf), None, buf)
        self.DeleteProgram(program)
        return bool(ok.value), buf.value.decode(errors="replace").strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--verbose", action="store_true", help="print driver warnings too")
    args = parser.parse_args()

    kwargs = {}
    if sys.platform.startswith("linux"):
        kwargs = {"libgl": "libGL.so.1", "libx11": "libX11.so.6"}
    ctx = moderngl.create_standalone_context(require=460, **kwargs)
    print(f"Driver: {ctx.info['GL_RENDERER']} / {ctx.info['GL_VERSION']}")
    gl = GL()

    failures: list[str] = []
    compiled: dict[str, int] = {}

    stage_files = sorted(
        p for p in SHADER_DIR.rglob("*")
        if p.is_file() and "includes" not in p.parts and p.suffix in {".glsl", ".comp", ".vert", ".frag", ".geom"}
    )
    paired_frags = {prog[1] for prog in LINKED_PROGRAMS}
    for path in stage_files:
        rel = path.relative_to(SHADER_DIR).as_posix()
        stage = stage_for(path)
        if stage is None:
            stage = GL_FRAGMENT_SHADER if rel in paired_frags else None
        source = load_with_includes(path)
        if not re.search(r"^\s*#\s*version\b", source, re.MULTILINE):
            continue  # include-only file (e.g. surfel_gi/common.glsl, msme.comp)
        if stage is None:
            print(f"  skip  {rel} (unknown stage)")
            continue
        shader, log = gl.compile(stage, source)
        if shader:
            compiled[rel] = shader
            print(f"  ok    {rel}")
            if args.verbose and log:
                print("        " + log.replace("\n", "\n        "))
        else:
            failures.append(rel)
            print(f"  FAIL  {rel}\n        " + log.replace("\n", "\n        "))

    for program in LINKED_PROGRAMS:
        name = " + ".join(program)
        if not all(stage in compiled for stage in program):
            continue
        ok, log = gl.link([compiled[stage] for stage in program])
        if ok:
            print(f"  link  {name}")
        else:
            failures.append(name)
            print(f"  FAIL  link {name}\n        " + log.replace("\n", "\n        "))

    print(f"\n{len(compiled)} stages compiled, {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
