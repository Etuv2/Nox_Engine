#version 460 core

// CRITICAL FIX: Stable direction generation aligned with SSAO's noise approach
// Generates stochastic hemisphere directions with temporal stability

layout (local_size_x = 8, local_size_y = 8) in;

// Store two randoms in RG of an RGBA16F image
layout (rgba16f, binding = 0) writeonly uniform image2D outDirs;

uniform vec2 invScreen;
uniform int frameIndex;

// CRITICAL FIX: Use robust hash similar to screen-space shadows for consistency
// This ensures stable, blue-noise-like distribution across frames
float Hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

// R2 low-discrepancy sequence for blue-noise-like distribution (Sachdeva's approach)
vec2 R2Sequence(int n) {
 const float g = 1.32471795724474602596; // Plastic constant
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return fract(vec2(a1, a2) * float(n));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);

 // CRITICAL FIX: Use frame index for temporal stability (not pure random)
    // Combine pixel position with frame index for decorrelated sampling
    int seed = id.x + id.y * 8192 + frameIndex * 65536;
    
    // Use R2 sequence for excellent blue-noise distribution
    vec2 baseRandom = R2Sequence(seed);
    
    // Add spatial decorrelation using hash (prevents patterns)
    vec2 spatialNoise = vec2(
    Hash21(vec2(id) + vec2(frameIndex * 0.1)),
   Hash21(vec2(id.y, id.x) + vec2(frameIndex * 0.1 + 0.5))
    );
    
    // Blend R2 sequence with spatial hash for optimal distribution
    // R2 provides temporal stability, hash provides spatial decorrelation
    vec2 r = fract(baseRandom + spatialNoise * 0.25);
    
    // Ensure values are in [0,1) range
    r = clamp(r, 0.0, 0.999999);

    imageStore(outDirs, id, vec4(r.x, r.y, 0.0, 0.0));
}
