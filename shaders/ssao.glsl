#version 460 core

in vec2 TexCoord;
out float FragColor;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
uniform sampler2D gPackedNormalRM;
uniform sampler2D gDepth;
uniform sampler2D noiseTex;   // 4x4 random rotations
uniform vec2      screenSize;

// Matrices
uniform mat4 proj;
uniform mat4 invProj;

// SSAO kernel
uniform vec3 samples[192];
const int kernelSize = 64;

// Tunables (view-space units)
uniform float radius = 0.5;   // keep local
uniform float bias   = 0.025; // push off the surface
uniform float intensity = 1.0;
uniform float aoMin = 0.25;   // prevent full black

// --- Oct normal decode
vec3 DecodeNormalOct8(vec2 e) {
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
    return normalize(n);
}

// --- Reconstruct view-space position from depth
vec3 ReconstructViewPos(vec2 uv, float depth01) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    vec4 view = invProj * clip;
    return view.xyz / max(view.w, 1e-6);
}

void main() {
    vec2 uv = TexCoord;
    float d = texture(gDepth, uv).r;
    if (d >= 1.0) { FragColor = 1.0; return; }

    // UNPACK NORMAL FROM NEW G-BUFFER LAYOUT
    vec2 encNormal = texture(gPackedNormalRM, uv).rg;  // Extract RG channels for oct-encoded normal
    vec3 N = DecodeNormalOct8(encNormal);
    vec3 P = ReconstructViewPos(uv, d);     // view-space pos

    // Build per-pixel TBN using a small rotation (blue/IGN noise)
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
        vec2 uv  = clip.xy / clip.w * 0.5 + 0.5;

        // Off-screen → ignore
        if (any(bvec2(uv.x <= 0.0 || uv.x >= 1.0 ||
                      uv.y <= 0.0 || uv.y >= 1.0))) continue;

        float sd   = texture(gDepth, uv).r;
        if (sd >= 1.0) continue; // sky

        vec3  Q    = ReconstructViewPos(uv, sd);   // view-space at sample UV
        vec3  dir  = normalize(Q - P);

        // Normal weighting (stops uniform darkening on large flats)
        float nDot = max(dot(N, dir), 0.0);

        // Distance falloff (attenuate far samples)
        float dist = length(Q - P);
        float fall = smoothstep(radius, 0.0, dist);

        float w = nDot * fall;
        if (w < 1e-4) continue;

        // View-space Z test (GL convention: forward is -Z)
        // If scene depth is closer than our sample point (plus bias) → occluded
        if (Q.z >= Svs.z + bias) occl += w;

        wsum += w;
    }

    // Normalize, invert to AO factor, clamp to avoid muddy planes
    float ao = 1.0 - (occl / max(wsum, 1e-6));
    ao = mix(1.0, ao, intensity);
    ao = clamp(ao, aoMin, 1.0);

    FragColor = ao;
}
