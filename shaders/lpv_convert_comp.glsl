#version 460 core

// Converts the fixed-point injection accumulators into the first propagation wave (the C++ side
// copies it into the accumulated volume that lighting reads; GL only guarantees 8 image units).
layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

// Inputs: signed fixed-point accumulators, 4x wider in X (one block per SH coefficient)
layout(r32i, binding = 0) readonly uniform iimage3D u_lpvInputR;
layout(r32i, binding = 1) readonly uniform iimage3D u_lpvInputG;
layout(r32i, binding = 2) readonly uniform iimage3D u_lpvInputB;

// Output: first propagation wave, 4 SH coefficients per texel
layout(rgba16f, binding = 3) writeonly uniform image3D u_waveR;
layout(rgba16f, binding = 4) writeonly uniform image3D u_waveG;
layout(rgba16f, binding = 5) writeonly uniform image3D u_waveB;

uniform int u_gridResolution;
uniform float u_fixedPointScale; // must match injection

vec4 LoadBands(int channel, ivec3 cell) {
    vec4 sh;
    for (int band = 0; band < 4; ++band) {
        ivec3 coord = cell + ivec3(u_gridResolution * band, 0, 0);
        int value = channel == 0 ? imageLoad(u_lpvInputR, coord).r
            : (channel == 1 ? imageLoad(u_lpvInputG, coord).r : imageLoad(u_lpvInputB, coord).r);
        sh[band] = float(value) / u_fixedPointScale;
    }
    return sh;
}

void main() {
    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(p, ivec3(u_gridResolution)))) {
        return;
    }

    vec4 shR = LoadBands(0, p);
    vec4 shG = LoadBands(1, p);
    vec4 shB = LoadBands(2, p);

    imageStore(u_waveR, p, shR);
    imageStore(u_waveG, p, shG);
    imageStore(u_waveB, p, shB);
}
