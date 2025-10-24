#version 450 core

in  vec2 TexCoord;
out float FragColor;

// Inputs
uniform sampler2D gDepth;    // depth
uniform sampler2D gNormal;   // oct-encoded normal
uniform sampler2D noiseTex;  // 4×4 rotation vectors
uniform vec2      screenSize;

// Matrices
uniform mat4 proj;
uniform mat4 invProj;

// SSAO kernel
uniform vec3 samples[192];

// Parameters
const int   kernelSize = 64;
uniform float radius = 0.5; // radius of occlusion
uniform float bias = 0.025; // depth bias

// Decode oct-encoded normal
vec3 DecodeNormalOct8(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 s = vec2(sign(e.x), sign(e.y));
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

// Reconstruct view-space position
vec3 reconstructViewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 view = invProj * clip;
    return view.xyz / view.w;
}

void main() {
    float d = texture(gDepth, TexCoord).r;
    if (d >= 1.0) {
        FragColor = 1.0;
        return;
    }

    // View-space pos & normal
    vec3 fragPos = reconstructViewPos(TexCoord, d);
    vec3 N = DecodeNormalOct8(texture(gNormal, TexCoord).rg);

    // Rotate samples in tangent space
    vec3 rand = texture(noiseTex, TexCoord * (screenSize / 4.0)).xyz;
    vec3 tangent = normalize(rand - N * dot(rand, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN = mat3(tangent, bitangent, N);

    // Occlusion accumulation
    float occlusion = 0.0;
    float weightSum = 0.0;
    for (int i = 0; i < kernelSize; ++i) {
        // Sample in view-space
        vec3 sampleVS = TBN * samples[i];
        sampleVS = fragPos + sampleVS * radius;

        // Project to screen
        vec4 offset = proj * vec4(sampleVS, 1.0);
        offset.xyz /= offset.w;
        vec2 uv = offset.xy * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        // Fetch sample depth and pos
        float sampleDepth = texture(gDepth, uv).r;
        vec3 samplePos = reconstructViewPos(uv, sampleDepth);

        // Range check
        float range = length(samplePos - fragPos);
        if (range > radius) continue;

        // Weight by angle and distance
        float NdotS = max(dot(N, normalize(samplePos - fragPos)), 0.0);
        float rangeWeight = smoothstep(radius, 0.0, range);
        float weight = NdotS * rangeWeight;

        // Depth test
        if (samplePos.z >= sampleVS.z + bias) {
            occlusion += weight;
        }
        weightSum += weight;
    }

    // Normalize and invert
    occlusion = 1.0 - (occlusion / max(weightSum, 0.0001));
    FragColor = occlusion;
}
