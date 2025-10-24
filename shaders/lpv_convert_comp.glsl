#version 460 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

// Inputs: integer LPV textures (extended in X: 4*gridResolution)
layout(r32ui, binding = 0) readonly uniform uimage3D u_lpvInputR;
layout(r32ui, binding = 1) readonly uniform uimage3D u_lpvInputG;
layout(r32ui, binding = 2) readonly uniform uimage3D u_lpvInputB;

// Outputs: float LPV textures (RGBA16F) per voxel
layout(rgba16f, binding = 3) writeonly uniform image3D u_lpvOutR;
layout(rgba16f, binding = 4) writeonly uniform image3D u_lpvOutG;
layout(rgba16f, binding = 5) writeonly uniform image3D u_lpvOutB;

uniform int u_gridResolution; // base resolution per axis

const uint SCALE_FACTOR = 100000u; // must match injection/propagation

float scaledUIntToFloat(uint v) { return float(v) / float(SCALE_FACTOR); }

// Helper functions that read from global images (avoid passing images as params)
vec4 LoadBandsR(ivec3 base) {
    ivec3 o0 = base;
    ivec3 o1 = base + ivec3(u_gridResolution, 0, 0);
    ivec3 o2 = base + ivec3(u_gridResolution * 2, 0, 0);
    ivec3 o3 = base + ivec3(u_gridResolution * 3, 0, 0);
    uint b0 = imageLoad(u_lpvInputR, o0).r;
    uint b1 = imageLoad(u_lpvInputR, o1).r;
    uint b2 = imageLoad(u_lpvInputR, o2).r;
    uint b3 = imageLoad(u_lpvInputR, o3).r;
    return vec4(scaledUIntToFloat(b0), scaledUIntToFloat(b1), scaledUIntToFloat(b2), scaledUIntToFloat(b3));
}

vec4 LoadBandsG(ivec3 base) {
    ivec3 o0 = base;
    ivec3 o1 = base + ivec3(u_gridResolution, 0, 0);
    ivec3 o2 = base + ivec3(u_gridResolution * 2, 0, 0);
    ivec3 o3 = base + ivec3(u_gridResolution * 3, 0, 0);
    uint b0 = imageLoad(u_lpvInputG, o0).r;
    uint b1 = imageLoad(u_lpvInputG, o1).r;
    uint b2 = imageLoad(u_lpvInputG, o2).r;
    uint b3 = imageLoad(u_lpvInputG, o3).r;
    return vec4(scaledUIntToFloat(b0), scaledUIntToFloat(b1), scaledUIntToFloat(b2), scaledUIntToFloat(b3));
}

vec4 LoadBandsB(ivec3 base) {
    ivec3 o0 = base;
    ivec3 o1 = base + ivec3(u_gridResolution, 0, 0);
    ivec3 o2 = base + ivec3(u_gridResolution * 2, 0, 0);
    ivec3 o3 = base + ivec3(u_gridResolution * 3, 0, 0);
    uint b0 = imageLoad(u_lpvInputB, o0).r;
    uint b1 = imageLoad(u_lpvInputB, o1).r;
    uint b2 = imageLoad(u_lpvInputB, o2).r;
    uint b3 = imageLoad(u_lpvInputB, o3).r;
    return vec4(scaledUIntToFloat(b0), scaledUIntToFloat(b1), scaledUIntToFloat(b2), scaledUIntToFloat(b3));
}

void main() {
    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(p, ivec3(u_gridResolution)))) return;

    // NOTE: inputs are 4x wider in X
    vec4 shR = LoadBandsR(p);
    vec4 shG = LoadBandsG(p);
    vec4 shB = LoadBandsB(p);

    imageStore(u_lpvOutR, p, shR);
    imageStore(u_lpvOutG, p, shG);
    imageStore(u_lpvOutB, p, shB);
}
