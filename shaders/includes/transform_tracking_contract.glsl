#ifndef TRANSFORM_TRACKING_CONTRACT_GLSL
#define TRANSFORM_TRACKING_CONTRACT_GLSL

struct GpuTransformRecord {
    mat4 world;
    mat4 prevWorld;
    uvec4 metadata; // x=flags, y=skinPaletteOffset, z=generation, w=reserved
};

struct TransformHistoryRecord {
    vec4 worldPosition;
    vec4 prevWorldPosition;
    uvec4 metadata; // x=lastSeenFrame, y=visiblePixelCount, z=generation, w=flags
};

#endif
