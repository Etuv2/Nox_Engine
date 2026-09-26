#version 460 core
#include "includes/lpv_common.glsl"

// Injects the reflective shadow map into the LPV as virtual point lights. The RSM is sampled on
// a regular grid of u_vplGridSize^2 VPLs; each one stands for the u_vplArea (m^2, perpendicular
// to the light) around it, so its flux is RSM flux * area. A Lambertian VPL with flux F and
// normal n has intensity I(w) = F / pi * max(0, n.w).
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D u_rsmPosition;
uniform sampler2D u_rsmNormal;
uniform sampler2D u_rsmFlux;

// Signed fixed-point accumulators (image atomics are integer-only). The grid is 4x wider in X,
// one block per SH coefficient.
layout(r32i, binding = 0) uniform iimage3D u_lpvR;
layout(r32i, binding = 1) uniform iimage3D u_lpvG;
layout(r32i, binding = 2) uniform iimage3D u_lpvB;

uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;
uniform vec4 u_gridOrientation; // Quaternion (x, y, z, w)
uniform int u_vplGridSize;
uniform float u_vplArea;
uniform float u_fixedPointScale;

int ToFixedPoint(float value) {
    return int(clamp(round(value * u_fixedPointScale), -2147483520.0, 2147483520.0));
}

void AtomicAddSH(int channel, ivec3 cell, vec4 sh) {
    for (int band = 0; band < 4; ++band) {
        ivec3 coord = cell + ivec3(u_gridResolution * band, 0, 0);
        int value = ToFixedPoint(sh[band]);
        if (value == 0) {
            continue;
        }
        if (channel == 0) {
            imageAtomicAdd(u_lpvR, coord, value);
        } else if (channel == 1) {
            imageAtomicAdd(u_lpvG, coord, value);
        } else {
            imageAtomicAdd(u_lpvB, coord, value);
        }
    }
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(id, ivec2(u_vplGridSize)))) {
        return;
    }

    vec2 rsmUV = (vec2(id) + 0.5) / float(u_vplGridSize);
    vec3 flux = max(textureLod(u_rsmFlux, rsmUV, 0.0).rgb, vec3(0.0)) * u_vplArea;
    if (max(flux.r, max(flux.g, flux.b)) <= 0.0) {
        return;
    }

    vec3 vplPosition = textureLod(u_rsmPosition, rsmUV, 0.0).xyz;
    vec3 vplNormal = textureLod(u_rsmNormal, rsmUV, 0.0).xyz;
    if (dot(vplNormal, vplNormal) < 1e-6) {
        return;
    }
    vplNormal = normalize(vplNormal);

    // Move the VPL half a cell off its surface so it lights the space in front of the surface
    // rather than the cell the surface itself sits in (Kaplanyan & Dachsbacher 2010).
    vec3 gridPos = LPVWorldToGrid(vplPosition + vplNormal * (0.5 * u_voxelSize),
        u_gridCenter, u_gridOrientation, u_voxelSize, u_gridResolution);
    ivec3 cell = ivec3(floor(gridPos));
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(u_gridResolution)))) {
        return;
    }

    vec4 lobe = LPVCosineLobe(LPVRotateInverse(vplNormal, u_gridOrientation)) * LPV_INV_PI;
    AtomicAddSH(0, cell, lobe * flux.r);
    AtomicAddSH(1, cell, lobe * flux.g);
    AtomicAddSH(2, cell, lobe * flux.b);
}
