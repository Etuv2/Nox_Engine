#version 460 core

// 2x upsample with bilateral filter using depth and normals to prevent edge bleeding
// Input: sampler2D inLow (quarter), sampler2D inHigh (half current - guidance), depthTex, normalTex
// Output: image2D outTex (half)

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 0) uniform sampler2D inLow;     // quarter-res blurred
layout (binding = 1) uniform sampler2D inHigh;    // half-res guidance (raw SSGI)
layout (binding = 2) uniform sampler2D depthTex;  // full-res depth
layout (binding = 3) uniform sampler2D normalTex; // full-res normals
layout (binding = 4, rgba16f) writeonly uniform image2D outTex; // half-res output

uniform vec2 invDst;        // 1.0 / half-res
uniform vec2 invFull;       // 1.0 / full-res
uniform float depthSigma;   // same as bilateral
uniform float normalThresh; // same as bilateral

vec3 decodeNormal(vec2 enc) {
    vec3 n = vec3(enc * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy)));
    return normalize(n);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(outTex);
    if (any(greaterThanEqual(id, dstSize))) return;

    vec2 uvHalf = (vec2(id) + 0.5) * invDst;
    vec2 uvFull = uvHalf; // mapping half->full UVs assume same viewport

    float d0 = textureLod(depthTex, uvFull, 0).r;
    vec3 n0 = decodeNormal(textureLod(normalTex, uvFull, 0).rg);

    // Fetch nearest quarter texel
    vec3 centerLow = textureLod(inLow, uvHalf, 0).rgb;

    // Bilateral upsample: 3x3 around half-res pixel, sampling low-res with offsets and weighting by guidance
    vec3 acc = vec3(0.0);
    float wsum = 0.0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 uvN = uvHalf + vec2(dx, dy) * invDst;

            // Sample low-res (quarter) - reuse same uv since normalized space maps fine
            vec3 c = textureLod(inLow, uvN, 0).rgb;

            // Guidance from high-res depth/normal
            float d = textureLod(depthTex, uvN, 0).r;
            vec3 n = decodeNormal(textureLod(normalTex, uvN, 0).rg);

            float wd = exp(-abs(d - d0) / max(depthSigma, 1e-4));
            float wn = pow(max(dot(n, n0), 0.0), 32.0);
            if (dot(n, n0) < 1.0 - normalThresh) wn = 0.0;

            float w = wd * wn;
            acc += c * w;
            wsum += w;
        }
    }

    vec3 outc = (wsum > 0.0) ? (acc / wsum) : centerLow;

    // Optionally blend with high-res guidance to preserve high-frequency detail
    vec3 high = textureLod(inHigh, uvHalf, 0).rgb;
    outc = mix(outc, high, 0.15);

    imageStore(outTex, id, vec4(outc, 1.0));
}
