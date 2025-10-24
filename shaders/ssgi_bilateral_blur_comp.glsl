#version 460 core

// Edge-aware bilateral blur for SSGI denoising
// Preserves edges using depth and normal information

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D inTex;       // Raw SSGI to denoise
layout (binding = 1) uniform sampler2D depthTex;    // Depth for edge detection
layout (binding = 2) uniform sampler2D normalTex;   // Normals for edge detection

// Output texture - FIXED: use rgba16f instead of rgb16f
layout (binding = 3, rgba16f) writeonly uniform image2D outTex;

// Uniforms
uniform float depthSigma;    // Depth difference threshold (view-space)
uniform float normalThresh;  // Normal difference threshold
uniform vec2 invWork;        // 1.0 / working resolution

// Decode octahedron-encoded normal
vec3 decodeNormal(vec2 enc) {
    vec3 n = vec3(enc * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy)));
    return normalize(n);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(id) + 0.5) * invWork;

    // Sample center pixel properties
    float d0 = textureLod(depthTex, uv, 0).r;
    vec3 n0 = decodeNormal(textureLod(normalTex, uv, 0).rg);

    // Bilateral filter: 5x5 kernel
    vec3 acc = vec3(0.0);
    float wsum = 0.0;

    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            vec2 uvN = uv + vec2(dx, dy) * invWork;
            
            // Sample neighbor
            vec3 c = textureLod(inTex, uvN, 0).rgb;
            float d = textureLod(depthTex, uvN, 0).r;
            vec3 n = decodeNormal(textureLod(normalTex, uvN, 0).rg);

            // Depth weight: exponential falloff based on depth difference
            float wd = exp(-abs(d - d0) / max(depthSigma, 1e-4));

            // Normal weight: sharp cutoff for different orientations
            float wn = pow(max(dot(n, n0), 0.0), 32.0);
            if (dot(n, n0) < 1.0 - normalThresh) {
                wn = 0.0; // Reject samples with very different normals
            }

            // Combined weight
            float w = wd * wn;
            acc += c * w;
            wsum += w;
        }
    }

    // Output filtered result (fallback to center if no valid samples)
    vec3 outc = (wsum > 0.0) ? acc / wsum : textureLod(inTex, uv, 0).rgb;
    imageStore(outTex, id, vec4(outc, 1.0));
}
