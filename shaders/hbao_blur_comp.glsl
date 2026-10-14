#version 460 core
// HBAO+ step 4: depth-aware separable blur of the reinterleaved AO (run once with uAxis = (1,0),
// once with (0,1)). Input rg = (ao, view depth); the depth travels with the AO so the vertical pass
// reads the horizontal result without touching the depth buffer. Weights: Gaussian in the tap
// distance times exp(-sharpness * relative depth difference), so AO does not bleed across depth edges.
// Output rg = (filtered ao, view depth); the lighting pass reads .r.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rg16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2D uInput;
uniform ivec2 uAxis;
uniform int uRadius;        // 1..8 taps on each side
uniform float uSharpness;   // depth-edge sharpness

void main() {
    ivec2 q = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uOut);
    if (any(greaterThanEqual(q, size))) {
        return;
    }

    vec2 center = texelFetch(uInput, q, 0).rg;
    if (center.g <= 0.0) {
        imageStore(uOut, q, vec4(center, 0.0, 0.0));   // sky: nothing to filter
        return;
    }

    float sigma = max(float(uRadius) * 0.5, 0.5);
    float sum = 0.0;
    float weightSum = 0.0;
    for (int i = -uRadius; i <= uRadius; ++i) {
        ivec2 sq = q + uAxis * i;
        if (any(lessThan(sq, ivec2(0))) || any(greaterThanEqual(sq, size))) {
            continue;
        }
        vec2 s = texelFetch(uInput, sq, 0).rg;
        if (s.g <= 0.0) {
            continue;
        }
        float relDepth = abs(s.g - center.g) / center.g;
        float weight = exp(-0.5 * float(i * i) / (sigma * sigma)) * exp(-uSharpness * relDepth);
        sum += s.r * weight;
        weightSum += weight;
    }

    float ao = weightSum > 0.0 ? sum / weightSum : center.r;
    imageStore(uOut, q, vec4(ao, center.g, 0.0, 0.0));
}
