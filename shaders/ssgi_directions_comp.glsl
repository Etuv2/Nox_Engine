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

float u01(uint value) {
    return float(value) * (1.0 / 4294967296.0);
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

    int linearIndex = id.x + id.y * size.x;
    uint seed = uint(linearIndex);
    uint scrambleX = pcg_hash(seed ^ 0x68bc21ebu);
    uint scrambleY = pcg_hash(seed ^ 0x02e5be93u);
    vec2 pixelScramble = vec2(u01(scrambleX), u01(scrambleY));

    // Use a low-discrepancy temporal sequence with stable per-pixel scrambling.
    vec2 temporal = r2Sequence(frameIndex & 255);
    vec2 r = fract(pixelScramble + temporal);
    imageStore(outDirs, id, vec4(r, 0.0, 0.0));
}
