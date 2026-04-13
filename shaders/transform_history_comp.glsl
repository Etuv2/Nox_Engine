#version 460 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "includes/transform_tracking_contract.glsl"

layout(binding = 6, std430) readonly buffer TransformBuffer {
    GpuTransformRecord transforms[];
};

layout(binding = 7, std430) buffer TransformHistoryBuffer {
    TransformHistoryRecord history[];
};

layout(binding = 8, std430) buffer VisibleTransformListBuffer {
    uint visibleList[];
};

uniform usampler2D uTransformIDBuffer;
uniform vec2 uResolution;
uniform int uFrameIndex;
uniform int uMaxTransformID;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= int(uResolution.x) || pixel.y >= int(uResolution.y)) {
        return;
    }

    uint transformID = texelFetch(uTransformIDBuffer, pixel, 0).r;
    if (transformID == 0u || transformID >= uint(uMaxTransformID)) {
        return;
    }

    GpuTransformRecord gpuRecord = transforms[transformID];
    history[transformID].worldPosition = gpuRecord.world[3];
    history[transformID].prevWorldPosition = gpuRecord.prevWorld[3];
    history[transformID].metadata.x = uint(uFrameIndex);
    history[transformID].metadata.z = gpuRecord.metadata.z;
    history[transformID].metadata.w = gpuRecord.metadata.x;
    atomicAdd(history[transformID].metadata.y, 1u);

    uint slot = atomicAdd(visibleList[0], 1u) + 1u;
    visibleList[slot] = transformID;
}
