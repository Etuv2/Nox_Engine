#version 460 core
#include "includes/lpv_common.glsl"

// One light propagation step (Kaplanyan & Dachsbacher 2010), in gather form: every cell collects
// the light its six neighbours send through its faces. From a source cell, a destination cell's
// front face subtends LPV_FRONT_FACE_SOLID_ANGLE and each of its four side faces
// LPV_SIDE_FACE_SOLID_ANGLE; the flux I(w_face) * solid angle crossing a face is re-emitted from
// the destination as a Lambertian lobe facing out of that face. The 30 faces around a source
// cover the full sphere, so free-space propagation conserves energy. The new wave is also added
// to the accumulated volume that lighting reads.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// Current wave (texelFetch, 4 SH coefficients per texel)
uniform sampler3D u_waveR;
uniform sampler3D u_waveG;
uniform sampler3D u_waveB;

// Next wave
layout(binding = 0, rgba16f) writeonly uniform image3D u_nextR;
layout(binding = 1, rgba16f) writeonly uniform image3D u_nextG;
layout(binding = 2, rgba16f) writeonly uniform image3D u_nextB;

// Accumulated volume (each invocation only touches its own texel)
layout(binding = 3, rgba16f) uniform image3D u_accumR;
layout(binding = 4, rgba16f) uniform image3D u_accumG;
layout(binding = 5, rgba16f) uniform image3D u_accumB;

// Directional geometry volume (see lpv_voxelize_frag.glsl): bit (axis * 2 + half) marks a surface
// facing that axis in the lower (0) or upper (1) half of the cell.
uniform usampler3D u_geometryVolume;
uniform bool u_enableOcclusion = false;
uniform int u_gridResolution;

const ivec3 NEIGHBOR_OFFSETS[6] = ivec3[](
    ivec3( 1,  0,  0),
    ivec3(-1,  0,  0),
    ivec3( 0,  1,  0),
    ivec3( 0, -1,  0),
    ivec3( 0,  0,  1),
    ivec3( 0,  0, -1)
);

void Transfer(vec4 srcR, vec4 srcG, vec4 srcB, vec3 evalDir, vec3 faceNormal, float solidAngle,
              inout vec4 accR, inout vec4 accG, inout vec4 accB) {
    vec4 basis = LPVSHBasis(evalDir);
    vec3 intensity = max(vec3(dot(srcR, basis), dot(srcG, basis), dot(srcB, basis)), vec3(0.0));
    vec3 flux = intensity * solidAngle;
    vec4 lobe = LPVCosineLobe(faceNormal) * LPV_INV_PI;
    accR += lobe * flux.r;
    accG += lobe * flux.g;
    accB += lobe * flux.b;
}

void main() {
    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(p, ivec3(u_gridResolution)))) {
        return;
    }

    vec4 accR = vec4(0.0);
    vec4 accG = vec4(0.0);
    vec4 accB = vec4(0.0);

    for (int i = 0; i < 6; ++i) {
        ivec3 d = NEIGHBOR_OFFSETS[i];
        ivec3 source = p - d;
        if (any(lessThan(source, ivec3(0))) || any(greaterThanEqual(source, ivec3(u_gridResolution)))) {
            continue;
        }

        vec4 srcR = texelFetch(u_waveR, source, 0);
        vec4 srcG = texelFetch(u_waveG, source, 0);
        vec4 srcB = texelFetch(u_waveB, source, 0);

        // Stop light that would cross a surface between the two cell centers: the far half of the
        // source cell or the near half of the destination cell along the propagation axis.
        if (u_enableOcclusion) {
            int axis = d.x != 0 ? 0 : (d.y != 0 ? 1 : 2);
            bool positive = (d.x + d.y + d.z) > 0;
            uint sourceBit = 1u << uint(axis * 2 + (positive ? 1 : 0));
            uint destinationBit = 1u << uint(axis * 2 + (positive ? 0 : 1));
            uint sourceBits = texelFetch(u_geometryVolume, source, 0).r;
            uint destinationBits = texelFetch(u_geometryVolume, p, 0).r;
            if ((sourceBits & sourceBit) != 0u || (destinationBits & destinationBit) != 0u) {
                continue;
            }
        }

        vec3 dir = vec3(d);
        Transfer(srcR, srcG, srcB, dir, dir, LPV_FRONT_FACE_SOLID_ANGLE, accR, accG, accB);

        // Side faces: the two axes perpendicular to d, both signs.
        vec3 sideA = (d.x != 0) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 sideB = cross(dir, sideA);
        Transfer(srcR, srcG, srcB, normalize(dir + 0.5 * sideA), sideA, LPV_SIDE_FACE_SOLID_ANGLE, accR, accG, accB);
        Transfer(srcR, srcG, srcB, normalize(dir - 0.5 * sideA), -sideA, LPV_SIDE_FACE_SOLID_ANGLE, accR, accG, accB);
        Transfer(srcR, srcG, srcB, normalize(dir + 0.5 * sideB), sideB, LPV_SIDE_FACE_SOLID_ANGLE, accR, accG, accB);
        Transfer(srcR, srcG, srcB, normalize(dir - 0.5 * sideB), -sideB, LPV_SIDE_FACE_SOLID_ANGLE, accR, accG, accB);
    }

    imageStore(u_nextR, p, accR);
    imageStore(u_nextG, p, accG);
    imageStore(u_nextB, p, accB);
    imageStore(u_accumR, p, imageLoad(u_accumR, p) + accR);
    imageStore(u_accumG, p, imageLoad(u_accumG, p) + accG);
    imageStore(u_accumB, p, imageLoad(u_accumB, p) + accB);
}
