#version 460 core

// Edge-aware bilateral blur for SSGI denoising
// Preserves edges using depth and normal information

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D inTex;       // Raw SSGI to denoise (rgba: rgb=indirect, a=mask)
layout (binding = 1) uniform sampler2D depthTex;    // Depth for edge detection
layout (binding = 2) uniform sampler2D normalTex;   // Normals for edge detection (oct-encoded)

// Output texture - rgba16f
layout (binding = 3, rgba16f) writeonly uniform image2D outTex;

// Uniforms
uniform float depthSigma;    // Depth difference threshold (view-space)
uniform float normalThresh;  // Normal difference threshold
uniform vec2 invWork;        // 1.0 / working resolution

// Proper octahedral normal decoding with fold
vec3 octDecode(vec2 e) {
    vec2 f = e * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0 ? -t : t);
    n.y += (n.y >= 0.0 ? -t : t);
    return normalize(n);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(id) + 0.5) * invWork;

    // Sample center pixel properties
    float d0 = textureLod(depthTex, uv, 0).r;
    vec3 n0 = octDecode(textureLod(normalTex, uv, 0).rg);

    // Bilateral filter: 5x5 kernel
    vec3 acc = vec3(0.0);
    float am = 0.0; // alpha accumulation with same weights
    float wsum = 0.0;

    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            vec2 uvN = uv + vec2(dx, dy) * invWork;
            
            // Sample neighbor
            vec4 c = textureLod(inTex, uvN, 0);
            float d = textureLod(depthTex, uvN, 0).r;
            vec3 n = octDecode(textureLod(normalTex, uvN, 0).rg);

            // Depth weight: exponential falloff based on depth difference
            float wd = exp(-abs(d - d0) / max(depthSigma, 1e-4));

            // Normal weight: sharp cutoff for different orientations
            float nd = max(dot(n, n0), 0.0);
            float wn = pow(nd, 32.0);
            if (nd < 1.0 - normalThresh) {
                wn = 0.0; // Reject samples with very different normals
            }

            // Spatial weight (optional small Gaussian)
            float ws = exp(-float(dx * dx + dy * dy) / 8.0);

            // Combined weight
            float w = wd * wn * ws;
            acc += c.rgb * w;
            am += c.a * w;
            wsum += w;
        }
    }

    // Output filtered result (fallback to center if no valid samples)
    vec4 center = textureLod(inTex, uv, 0);
    vec3 outRGB = (wsum > 0.0) ? (acc / wsum) : center.rgb;
    float outA = (wsum > 0.0) ? (am / wsum) : center.a;
    imageStore(outTex, id, vec4(outRGB, outA));
}
