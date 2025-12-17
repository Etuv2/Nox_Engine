#version 460 core
// Generates stochastic hemisphere directions with temporal stability

layout (local_size_x = 8, local_size_y = 8) in;

// Output: stores two random values in RG of an RGBA16F image
layout (rgba16f, binding = 0) writeonly uniform image2D outDirs;

uniform vec2 invScreen;
uniform int frameIndex;

// PCG hash: https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint pcg_hash(uint input_) {
	uint state = input_ * 747796405u + 2891336453u;
	uint word = ((state >> 27u) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// R2 low-discrepancy sequence for temporally stable blue-noise distribution
vec2 r2Sequence(int n) {
    const float g = 1.32471795724474602596; // Plastic constant
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return fract(vec2(a1, a2) * float(n));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);

    // Combine pixel position with frame index for decorrelated sampling
    int seed = id.x + id.y * 8192 + frameIndex * 65536;

    // Generate base R2 sequence value
    vec2 baseRandom = r2Sequence(seed);

    // Add spatial decorrelation using hash
    vec2 spatialNoise = vec2(
        pcg_hash(uint(vec2(id) + vec2(frameIndex * 0.1))),
        pcg_hash(uint(vec2(id.y, id.x) + vec2(frameIndex * 0.1 + 0.5)))
    );

    // Blend R2 sequence with spatial hash for balanced temporal and spatial variation
    vec2 r = fract(baseRandom + spatialNoise * 0.25);

    // Clamp to valid [0, 1) range
    r = clamp(r, 0.0, 0.999999);

    imageStore(outDirs, id, vec4(r.x, r.y, 0.0, 0.0));
}
