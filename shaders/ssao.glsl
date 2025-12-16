/*
    Screen-Space Ambient Occlusion (SSAO) Shader
    Technique pulled from LearnOpenGL: https://learnopengl.com/Advanced-Lighting/SSAO
*/
#version 460 core

in vec2 TexCoord;
out float FragColor;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
uniform sampler2D gPackedNormalRM;
uniform sampler2D gDepth;
uniform sampler2D noiseTex;   // 4x4 random rotations
uniform vec2 screenSize;

// Matrices
uniform mat4 proj;
uniform mat4 invProj;
uniform mat4 view;

// Normal space configuration (match deferred lighting)
uniform int normalsInWorldSpace = 1;

// SSAO kernel
uniform vec3 samples[64];  // Match actual kernel size
const int kernelSize = 64;

// Tunables (view-space units)
uniform float radius = 0.75;   // Reduced for more localized AO
uniform float bias   = 0.015;  // Reduced bias for better contact shadows
uniform float intensity = 1.2; // Slightly increased intensity
uniform float aoMin = 0.0;     // Allow full occlusion

vec3 DecodeNormalOct8(vec2 e) {
    // Remap from [0,1] to [-1,1]
    e = e * 2.0 - 1.0;
    
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    
    if (n.z < 0.0) {
        // Handle lower hemisphere fold
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }
    
    return normalize(n);
}

// Transform normal to view space
vec3 GetNormalInViewSpace(vec3 worldNormal) {
    if (normalsInWorldSpace == 1) {
        return normalize(mat3(view) * worldNormal);
    } else {
        return worldNormal; // Already in view space
    }
}
// Reconstruct view-space position from depth
vec3 ReconstructViewPos(vec2 uv, float depth01) {
    // Convert depth from [0,1] to NDC [-1,1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    
    // Transform to view space
    vec4 viewPos = invProj * ndc;
    
    // Perspective divide - ensure we handle near-zero w
    viewPos.xyz /= max(abs(viewPos.w), 1e-6);
    
    return viewPos.xyz;
}

// Convert linear depth to a more meaningful comparison value
float LinearizeDepth(float depth) {
    float near = 0.1;
    float far = 1000.0;
    float z = depth * 2.0 - 1.0; // Back to NDC
    return (2.0 * near * far) / (far + near - z * (far - near));
}

void main() {
    vec2 uv = TexCoord;
    float d = texture(gDepth, uv).r;
    
    // Skip skybox
    if (d >= 0.9999) { 
        FragColor = 1.0; 
        return; 
    }

    // Unpacking the normal, converting to view space
    vec2 encNormal = texture(gPackedNormalRM, uv).rg;
    vec3 worldNormal = DecodeNormalOct8(encNormal);
    vec3 N = GetNormalInViewSpace(worldNormal);  // Convert to view space

    // Reconstruct view-space position from depth
    vec3 P = ReconstructViewPos(uv, d);
    
    // Validate reconstructed position
    if (any(isnan(P)) || any(isinf(P))) {
        FragColor = 1.0;
        return;
    }

    // Build per-pixel TBN using noise rotation
    vec3 rand = texture(noiseTex, TexCoord * (screenSize / 4.0)).xyz;
    vec3 T = normalize(rand - N * dot(rand, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occl = 0.0;
    float wsum = 0.0;

    for (int i = 0; i < kernelSize; ++i) {
        // Sample point in view space (hemisphere oriented by TBN)
        vec3 Svs = P + (TBN * samples[i]) * radius;

        // Project to screen UV to fetch scene depth
        vec4 clip = proj * vec4(Svs, 1.0);
        vec2 sampleUV = clip.xy / max(abs(clip.w), 1e-6) * 0.5 + 0.5;

        // Skip off-screen samples
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) 
            continue;

        float sd = texture(gDepth, sampleUV).r;
        
        // Skip skybox samples
        if (sd >= 0.9999) 
            continue;

        // Reconstruct scene position at sample location
        vec3 Q = ReconstructViewPos(sampleUV, sd);
        
        // Validate sample position
        if (any(isnan(Q)) || any(isinf(Q))) 
            continue;
        
        // Direction from center to sample
        vec3 dir = normalize(Q - P);

        // Normal weighting to prevent uniform darkening on flat surfaces
        float nDot = max(dot(N, dir), 0.0);
        
        // Early out for samples pointing away from normal
        if (nDot < 0.01) 
            continue;

        // Distance-based falloff with smoother curve
        float dist = length(Q - P);
        float rangeFalloff = 1.0 - smoothstep(0.0, radius, dist);

        // Combined weight
        float w = nDot * rangeFalloff;
        
        if (w < 1e-4) 
            continue;

        // Depth comparison in view space Z (more negative = farther)
        // Check if scene surface Q is closer (less negative Z) than sample point Svs
        // This indicates the sample point is inside geometry (occluded)
        float depthDiff = Q.z - Svs.z;
        
        if (depthDiff > bias && depthDiff < radius) {
            occl += w;
        }

        wsum += w;
    }

    // Normalize and invert to AO factor
    float ao = 1.0;
    if (wsum > 1e-6) {
        ao = 1.0 - (occl / wsum);
    }
    
    // Apply intensity and clamp
    ao = pow(ao, intensity);
    ao = clamp(ao, aoMin, 1.0);

    FragColor = ao;
}
