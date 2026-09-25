#!/usr/bin/env python3
"""White-furnace and unit checks for the shared BRDF in shaders/includes/pbr_common.glsl.

The engine's own shaders are executed on a real OpenGL driver:
  * the split-sum BRDF LUT is rendered with shaders/brdf_{vert,frag}.glsl,
  * EvaluatePrincipledIBL is run against a uniform white environment
    (irradiance map and every prefiltered mip == 1), where a surface with
    white albedo must reflect ~all energy and never more than it receives,
  * the direct lobes are integrated over the hemisphere and checked for
    energy gain and for the metallic weighting of the diffuse lobe,
  * SpecularOcclusion is checked at its boundary values.

Run it the same way as validate_shader_compile.py (see that file's docstring).
Exit code is non-zero when any check fails.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import moderngl
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_shader_compile import INCLUDE_RE, SHADER_DIR, load_with_includes  # noqa: E402

LUT_SIZE = 128

COMPUTE_SRC = """
#version 460 core
layout(local_size_x = 64) in;
#include "includes/pbr_common.glsl"

struct Case {
    vec4 baseColorMetallic;       // rgb baseColor, a metallic
    vec4 roughClearNdotV;         // x roughness, y clearcoat, z clearcoat roughness, w NdotV
};
layout(std430, binding = 0) readonly buffer Cases { Case cases[]; };
layout(std430, binding = 1) writeonly buffer Results {
    vec4 results[];               // [3*i+0] IBL, [3*i+1] direct albedo, [3*i+2] misc
};
uniform int caseCount;
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;

void main() {
    int i = int(gl_GlobalInvocationID.x);
    if (i >= caseCount) return;
    Case c = cases[i];
    PrincipledSurface s = BuildPrincipledSurface(
        c.baseColorMetallic.rgb, c.baseColorMetallic.a, c.roughClearNdotV.x,
        1.5, 1.0, vec3(1.0), 0.0,
        c.roughClearNdotV.y, c.roughClearNdotV.z,
        0.0, 0.0, 0.0, 0.0, vec3(1.0), 1.0);

    vec3 N = vec3(0.0, 0.0, 1.0);
    float NdotV = c.roughClearNdotV.w;
    vec3 V = vec3(sqrt(max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);

    vec3 ibl = EvaluatePrincipledIBL(s, N, V, reflect(-V, N), 1.0, 1.0,
        irradianceMap, prefilteredMap, brdfLUT, 4.0, 1.0, 1.0, 1.0);

    // Hemispherical-directional albedo of the direct lobes (already cosine weighted).
    const int THETA = 96;
    const int PHI = 192;
    vec3 albedo = vec3(0.0);
    for (int t = 0; t < THETA; ++t) {
        float theta = (float(t) + 0.5) / float(THETA) * 0.5 * PI;
        for (int p = 0; p < PHI; ++p) {
            float phi = (float(p) + 0.5) / float(PHI) * TAU;
            vec3 L = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            albedo += EvaluatePrincipledBRDF(s, N, V, L) * sin(theta);
        }
    }
    albedo *= (0.5 * PI / float(THETA)) * (TAU / float(PHI));

    // Diffuse lobe with Fresnel forced to zero: isolates the metallic weighting.
    PrincipledLobeContext ctx = BuildPrincipledLobeContext(N, V, normalize(V + N));
    vec3 diffuseNoFresnel = EvaluatePrincipledDiffuseLobe(s, ctx, vec3(0.0), 0.0);

    results[3 * i + 0] = vec4(ibl, 0.0);
    results[3 * i + 1] = vec4(albedo, 0.0);
    results[3 * i + 2] = vec4(diffuseNoFresnel, 0.0);
}
"""

SPEC_OCC_SRC = """
#version 460 core
layout(local_size_x = 1) in;
#include "includes/pbr_common.glsl"
layout(std430, binding = 0) writeonly buffer Out { float values[]; };
void main() {
    int k = 0;
    for (int r = 0; r <= 4; ++r) {
        float roughness = float(r) / 4.0;
        for (int n = 1; n <= 4; ++n) {
            float NdotV = float(n) / 4.0;
            values[k++] = SpecularOcclusion(NdotV, 1.0, roughness);
            values[k++] = SpecularOcclusion(NdotV, 0.0, roughness);
            values[k++] = SpecularOcclusion(NdotV, 0.5, roughness);
        }
    }
}
"""


def make_context() -> moderngl.Context:
    kwargs = {"libgl": "libGL.so.1", "libx11": "libX11.so.6"} if sys.platform.startswith("linux") else {}
    return moderngl.create_standalone_context(require=460, **kwargs)


def expand(source: str) -> str:
    """Resolve #include lines of an in-memory shader relative to shaders/."""
    seen: set[Path] = set()
    out = []
    for line in source.strip().splitlines():
        match = INCLUDE_RE.match(line)
        out.append(load_with_includes(SHADER_DIR / match.group(1), seen) if match else line)
    return "\n".join(out) + "\n"


def render_brdf_lut(ctx: moderngl.Context) -> moderngl.Texture:
    prog = ctx.program(
        vertex_shader=load_with_includes(SHADER_DIR / "brdf_vert.glsl"),
        fragment_shader=load_with_includes(SHADER_DIR / "brdf_frag.glsl"),
    )
    quad = np.array([
        -1, 1, 0, 0, 1,
        -1, -1, 0, 0, 0,
        1, 1, 0, 1, 1,
        1, -1, 0, 1, 0,
    ], dtype="f4")
    vbo = ctx.buffer(quad.tobytes())
    vao = ctx.vertex_array(prog, [(vbo, "3f 2f", "aPos", "aTexCoords")])
    lut = ctx.texture((LUT_SIZE, LUT_SIZE), 2, dtype="f4")
    lut.filter = (moderngl.LINEAR, moderngl.LINEAR)
    lut.repeat_x = lut.repeat_y = False
    fbo = ctx.framebuffer(color_attachments=[lut])
    fbo.use()
    ctx.viewport = (0, 0, LUT_SIZE, LUT_SIZE)
    vao.render(moderngl.TRIANGLE_STRIP)
    ctx.finish()
    return lut


def white_cube(ctx: moderngl.Context, size: int, mips: int) -> moderngl.TextureCube:
    face = np.ones((size, size, 3), dtype="f4").tobytes()
    cube = ctx.texture_cube((size, size), 3, face * 6, dtype="f4")
    if mips > 1:
        cube.build_mipmaps(0, mips - 1)
        cube.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
    else:
        cube.filter = (moderngl.LINEAR, moderngl.LINEAR)
    return cube


def main() -> int:
    ctx = make_context()
    print(f"Driver: {ctx.info['GL_RENDERER']}")

    lut = render_brdf_lut(ctx)
    lut_data = np.frombuffer(lut.read(), dtype="f4").reshape(LUT_SIZE, LUT_SIZE, 2)
    irradiance = white_cube(ctx, 4, 1)
    prefiltered = white_cube(ctx, 16, 5)

    cases = []
    labels = []
    for rough in (0.05, 0.25, 0.5, 0.75, 1.0):
        for ndv in (1.0, 0.5, 0.15):
            cases.append(((1, 1, 1), 0.0, rough, 0.0, 0.0, ndv)); labels.append(("white dielectric", rough, ndv))
            cases.append(((1, 1, 1), 1.0, rough, 0.0, 0.0, ndv)); labels.append(("white metal", rough, ndv))
            cases.append(((1, 1, 1), 0.5, rough, 0.0, 0.0, ndv)); labels.append(("white half-metal", rough, ndv))
            cases.append(((0.9, 0.6, 0.3), 1.0, rough, 0.0, 0.0, ndv)); labels.append(("colored metal", rough, ndv))
            cases.append(((1, 1, 1), 0.0, rough, 1.0, 0.05, ndv)); labels.append(("clearcoat white", rough, ndv))

    case_bytes = b"".join(
        struct.pack("8f", *base, metal, rough, cc, ccr, ndv) for base, metal, rough, cc, ccr, ndv in cases
    )
    in_buf = ctx.buffer(case_bytes)
    out_buf = ctx.buffer(reserve=len(cases) * 3 * 16)

    shader = ctx.compute_shader(expand(COMPUTE_SRC))
    shader["caseCount"] = len(cases)
    irradiance.use(0)
    prefiltered.use(1)
    lut.use(2)
    shader["irradianceMap"] = 0
    shader["prefilteredMap"] = 1
    shader["brdfLUT"] = 2
    in_buf.bind_to_storage_buffer(0)
    out_buf.bind_to_storage_buffer(1)
    shader.run((len(cases) + 63) // 64)
    ctx.finish()
    res = np.frombuffer(out_buf.read(), dtype="f4").reshape(len(cases), 3, 4)[:, :, :3]

    failures: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)

    # LUT sanity: A+B (single-scatter directional albedo) must stay in (0, 1].
    ess = lut_data[..., 0] + lut_data[..., 1]
    check(float(ess.max()) <= 1.01, f"BRDF LUT energy > 1 (max A+B = {ess.max():.4f})")
    check(float(ess.min()) > 0.2, f"BRDF LUT energy collapsed (min A+B = {ess.min():.4f})")

    print(f"\n{'material':18} {'rough':>5} {'NdotV':>5} | {'IBL furnace (rgb)':>24} | {'direct albedo (rgb)':>24}")
    for (label, rough, ndv), (ibl, direct, diffuse0) in zip(labels, res):
        print(f"{label:18} {rough:5.2f} {ndv:5.2f} | {ibl[0]:7.3f} {ibl[1]:7.3f} {ibl[2]:7.3f}  | "
              f"{direct[0]:7.3f} {direct[1]:7.3f} {direct[2]:7.3f}")
        tag = f"{label} r={rough} NdotV={ndv}"
        check(float(ibl.max()) <= 1.02, f"{tag}: IBL gains energy {ibl}")
        # Burley diffuse retro-reflects above 1 at grazing view on rough surfaces by design
        # (Burley 2012); away from grazing the direct lobes must not create energy.
        direct_limit = 1.05 if ndv >= 0.5 else 1.30
        check(float(direct.max()) <= direct_limit, f"{tag}: direct lobes gain energy {direct}")
        if label in ("white dielectric", "white metal"):
            check(float(ibl.min()) >= 0.95, f"{tag}: IBL furnace should be ~1, got {ibl}")
        if label == "white half-metal":
            # (1 - metallic) must be applied once: diffuse lobe with F=0 is 0.5 * Lambert * Burley.
            full = res[labels.index(("white dielectric", rough, ndv))][2]
            ratio = float(diffuse0[0] / max(full[0], 1e-6))
            check(abs(ratio - 0.5) < 0.02, f"{tag}: diffuse metallic weighting ratio {ratio:.3f}, expected 0.5")
        if label == "colored metal" and rough <= 0.25 and ndv == 1.0:
            # A smooth metal viewed head-on must reflect its F0 colour, not a washed-out clamp.
            target = np.array([0.9, 0.6, 0.3])
            check(bool(np.all(np.abs(ibl - target) < 0.06)), f"{tag}: IBL {ibl} should match F0 {target}")
        if label == "clearcoat white":
            check(float(ibl.min()) >= 0.9, f"{tag}: clearcoat layering lost energy {ibl}")

    so_shader = ctx.compute_shader(expand(SPEC_OCC_SRC))
    so_buf = ctx.buffer(reserve=5 * 4 * 3 * 4)
    so_buf.bind_to_storage_buffer(0)
    so_shader.run(1)
    ctx.finish()
    so = np.frombuffer(so_buf.read(), dtype="f4").reshape(5, 4, 3)
    check(bool(np.allclose(so[..., 0], 1.0, atol=1e-4)), f"SpecularOcclusion(ao=1) must be 1: {so[..., 0]}")
    check(bool(np.allclose(so[..., 1], 0.0, atol=1e-4)), f"SpecularOcclusion(ao=0) must be 0: {so[..., 1]}")
    check(bool(np.allclose(so[4, :, 2], 0.5, atol=0.02)),
          f"SpecularOcclusion at roughness 1 should converge to diffuse AO: {so[4, :, 2]}")
    check(bool(np.all(np.diff(so[0, :, 2]) > 0.0)),
          f"SpecularOcclusion of a smooth lobe should open up towards normal incidence: {so[0, :, 2]}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed:")
        for f in failures:
            print("  - " + f)
        return 1
    print("All furnace checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
