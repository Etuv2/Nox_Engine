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
uniform mat4 view;  // CRITICAL: Need view matrix to transform normals

// Normal space configuration (match deferred lighting)
uniform int normalsInWorldSpace = 1;

// SSAO kernel
uniform vec3 samples[64];  // Match actual kernel size
const int kernelSize = 64;

// Tunables (view-space units)
uniform float radius = 1.5;   // Increased from 0.5 for better coverage
uniform float bias   = 0.025; // push off the surface
uniform float intensity = 1.0;
uniform float aoMin = 0.0;   // Changed from 0.25 - allow full occlusion

// --- Oct normal decode (FIXED: proper remapping from [0,1] to [-1,1])
vec3 DecodeNormalOct8(vec2 e) {
    // Remap from [0,1] (texture storage) to [-1,1]
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

// --- Reconstruct view-space position from depth
vec3 ReconstructViewPos(vec2 uv, float depth01) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    vec4 viewPos = invProj * clip;
    return viewPos.xyz / max(viewPos.w, 1e-6);
}

void main() {
    vec2 uv = TexCoord;
    float d = texture(gDepth, uv).r;
    if (d >= 0.9999) { FragColor = 1.0; return; }

    // UNPACK NORMAL FROM G-BUFFER (world space)
    vec2 encNormal = texture(gPackedNormalRM, uv).rg;
    vec3 worldNormal = DecodeNormalOct8(encNormal);
    vec3 N = GetNormalInViewSpace(worldNormal);  // Convert to view space
    
    vec3 P = ReconstructViewPos(uv, d);     // view-space pos

    // Build per-pixel TBN using noise rotation
    vec3 rand = texture(noiseTex, TexCoord * (screenSize / 4.0)).xyz;
    vec3 T = normalize(rand - N * dot(rand, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occl = 0.0;
    float wsum = 0.0;

    for (int i = 0; i < kernelSize; ++i) {
        // Sample point in view space (local hemisphere)
        vec3 Svs = P + (TBN * samples[i]) * radius;

        // Project to UV to fetch scene depth there
        vec4 clip = proj * vec4(Svs, 1.0);
        vec2 sampleUV = clip.xy / clip.w * 0.5 + 0.5;

        // Off-screen → ignore
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) 
            continue;

        float sd = texture(gDepth, sampleUV).r;
        if (sd >= 0.9999) continue; // sky

        vec3 Q = ReconstructViewPos(sampleUV, sd);   // view-space at sample UV
        vec3 dir = normalize(Q - P);

        // Normal weighting (stops uniform darkening on large flats)
        float nDot = max(dot(N, dir), 0.0);

        // Distance falloff (attenuate far samples)
        float dist = length(Q - P);
        float fall = smoothstep(radius, 0.0, dist);

        float w = nDot * fall;
        if (w < 1e-4) continue;

        // FIXED: View-space Z test (OpenGL convention: forward is -Z, so more negative = farther)
        // If scene depth (Q.z) is MORE NEGATIVE (farther) than sample point → occluded
        if (Q.z <= Svs.z - bias) {
            occl += w;
        }

        wsum += w;
    }

    // Normalize, invert to AO factor
    float ao = 1.0 - (occl / max(wsum, 1e-6));
    ao = mix(1.0, ao, intensity);
    ao = clamp(ao, aoMin, 1.0);

    FragColor = ao;
}
