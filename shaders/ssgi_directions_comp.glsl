#version 460 core

layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba16f, binding = 0) writeonly uniform image2D outDirs;

uniform vec2 invScreen;
uniform int frameIndex;

uint pcg_hash(uint inputValue) {
    uint state = inputValue * 747796405u + 2891336453u;
    uint word = ((state >> 27u) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec2 r2Sequence(int n) {
    const float g = 1.32471795724474602596;
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return fract(vec2(a1, a2) * float(n));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outDirs);
    if (any(greaterThanEqual(id, size))) {
        return;
    }

    int seed = id.x + id.y * size.x + frameIndex * size.x * size.y;
    vec2 base = r2Sequence(seed);

    uint hx = pcg_hash(uint(seed) ^ 0x68bc21ebu);
    uint hy = pcg_hash(uint(seed) ^ 0x02e5be93u);
    vec2 hashNoise = vec2(float(hx & 1023u), float(hy & 1023u)) / 1024.0;

    vec2 r = fract(base + hashNoise * 0.25);
    imageStore(outDirs, id, vec4(r, 0.0, 0.0));
}
