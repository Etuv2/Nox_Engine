#version 460 core
// HBAO+ step 2: horizon-based ambient occlusion on each deinterleaved layer (Bavoil & Sainz 2008;
// NVIDIA HBAO+). Dispatched as (quarter-res tiles, 16 layers). Every invocation shades one pixel of
// one layer: uDirections screen directions (default 8), uSteps steps each (default 4); the horizon angle is tracked per direction
// and the occlusion is the integral of the sine difference between the horizon and the tangent
// plane, weighted by the falloff 1 - d^2 / R^2. Normals come from the full-res gbuffer.
// Output: R16F visibility (1 = unoccluded).
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(r16f, binding = 0) uniform writeonly image2DArray uAOOut;

uniform sampler2DArray uLinearDepth;   // step 1 output
uniform sampler2D uNormals;            // gbuffer attachment 0 (packed world-space normal)

uniform mat4 uProj;
uniform mat4 uView;
uniform float uRadius;          // world-space radius
uniform float uAngleBias;       // tangent angle bias (radians)
uniform float uStrength;        // multiplies the accumulated occlusion
uniform float uPower;           // final exponent
uniform float uFocalPixels;     // proj[1][1] * fullHeight * 0.5: pixels per unit at view depth 1
uniform float uMaxRadiusPixels; // clamp of the projected radius, full-res pixels

uniform int uDirections;        // horizon directions per pixel (4..16, default 8)
uniform int uSteps;             // samples per direction (2..8, default 4)

// Fixed 4x4 random table: per layer a rotation and a step jitter, each a permutation of 0..15
// (mapped to (k + 0.5) / 16). Replaces the random-rotation noise texture of the old SSAO.
const int kRotation[16] = int[16](0, 9, 4, 13, 10, 3, 14, 7, 6, 15, 2, 11, 12, 5, 8, 1);
const int kStepJitter[16] = int[16](7, 14, 2, 11, 0, 9, 5, 12, 3, 8, 15, 1, 10, 6, 13, 4);

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    int layer = int(gl_GlobalInvocationID.z);
    ivec2 quarterSize = textureSize(uLinearDepth, 0).xy;
    if (any(greaterThanEqual(p, quarterSize))) {
        return;
    }

    ivec2 offset = ivec2(layer & 3, layer >> 2);
    ivec2 fullSize = textureSize(uNormals, 0);
    vec2 fullInv = 1.0 / vec2(fullSize);

    float z = texelFetch(uLinearDepth, ivec3(p, layer), 0).r;
    if (z <= 0.0) {
        imageStore(uAOOut, ivec3(p, layer), vec4(1.0));
        return;
    }

    ivec2 fp = min(p * 4 + offset, fullSize - 1);
    vec3 P = ViewPosFromLinearDepth((vec2(fp) + 0.5) * fullInv, z, uProj[0][0], uProj[1][1]);
    vec3 N = DecodeSceneNormalVS(texelFetch(uNormals, fp, 0).rg, uView, true);

    // Projected radius in quarter-res texels (the layer is quarter resolution).
    float radiusPixels = min(uRadius * uFocalPixels / z, uMaxRadiusPixels);
    float radiusLayer = radiusPixels * 0.25;
    float stepLayer = max(radiusLayer / float(uSteps + 1), 1.0);

    vec2 jitter = (vec2(float(kRotation[layer]), float(kStepJitter[layer])) + 0.5) / 16.0;
    float sinBias = sin(uAngleBias);
    float R2 = uRadius * uRadius;

    float occlusion = 0.0;
    for (int d = 0; d < uDirections; ++d) {
        float angle = (float(d) + jitter.x) * (6.2831853 / float(uDirections));
        vec2 dir = vec2(cos(angle), sin(angle));
        float horizon = sinBias;

        for (int s = 0; s < uSteps; ++s) {
            float t = (float(s) + jitter.y + 1.0) * stepLayer;
            ivec2 sp = ivec2(round(vec2(p) + dir * t));
            if (any(lessThan(sp, ivec2(0))) || any(greaterThanEqual(sp, quarterSize))) {
                break;   // leaving the screen: no occluder beyond the edge
            }

            float sz = texelFetch(uLinearDepth, ivec3(sp, layer), 0).r;
            if (sz <= 0.0) {
                continue;
            }
            ivec2 sfp = min(sp * 4 + offset, fullSize - 1);
            vec3 S = ViewPosFromLinearDepth((vec2(sfp) + 0.5) * fullInv, sz, uProj[0][0], uProj[1][1]);

            vec3 V = S - P;
            float dist2 = dot(V, V);
            if (dist2 >= R2) {
                continue;
            }
            float sinH = dot(V, N) * inversesqrt(max(dist2, 1e-10));
            if (sinH > horizon) {
                float falloff = 1.0 - dist2 / R2;
                occlusion += (sinH - horizon) * falloff;
                horizon = sinH;
            }
        }
    }

    occlusion *= uStrength / float(uDirections);
    float visibility = pow(clamp(1.0 - occlusion, 0.0, 1.0), uPower);
    imageStore(uAOOut, ivec3(p, layer), vec4(visibility));
}
