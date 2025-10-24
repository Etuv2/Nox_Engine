#version 460 core

// Generate stochastic cosine-weighted hemisphere directions for SSGI
// This creates per-pixel random directions that will be used for ray marching

layout (local_size_x = 8, local_size_y = 8) in;

// Output: RG texture storing random values for direction generation
layout (rg16f, binding = 0) writeonly uniform image2D outDirs;

// Uniforms
uniform vec2 invScreen; // 1.0 / screen dimensions
uniform int frameIndex; // NEW: animate noise over time

// Interleaved Gradient Noise - temporally stable noise function
float ign(ivec2 p, int t) {
    // Hash by pixel and frame to get good temporal variation
    uint x = uint(p.x);
    uint y = uint(p.y);
    uint f = uint(t);
    uint h = x * 0x27d4eb2dU ^ y * 0x165667b1U ^ f * 0x9e3779b9U;
    h ^= (h >> 15);
    h *= 0x85ebca6bU;
    h ^= (h >> 13);
    h *= 0xc2b2ae35U;
    h ^= (h >> 16);
    return float(h & 0x00ffffffu) / float(0x01000000u);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(id) + 0.5) * invScreen;

    // Generate two random values that will be used to create
    // cosine-weighted hemisphere samples in the raymarch stage
    float r1 = ign(id, frameIndex);
    float r2 = ign(id.yx + ivec2(17, 59), frameIndex + 13);
    
    // Store random values in RG channels
    imageStore(outDirs, id, vec4(r1, r2, 0.0, 0.0));
}
