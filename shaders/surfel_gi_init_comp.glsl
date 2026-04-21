#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 22, std430) buffer FreeStackBuffer {
    uint freeStack[];
};

layout(binding = 23, std430) buffer RecycleStackBuffer {
    uint recycleStack[];
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    uvec4 tileCoverage[];
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) buffer GridEntryBuffer {
    uint gridEntries[];
};

uniform vec2 uTileCount;
uniform int uGridCellCount;

void main()
{
    uint id = gl_GlobalInvocationID.x;
    uint maxSurfels = header.counts.x;

    if (id == 0u) {
        header.counts = uvec4(maxSurfels, 0u, maxSurfels, 0u);
        header.frameStats = uvec4(0u);
        header.tiling.x = uint(max(uTileCount.x, 1.0));
        header.tiling.y = uint(max(uTileCount.y, 1.0));
        header.tiling.w = uint(max(uGridCellCount, 1));
    }

    if (id < maxSurfels) {
        surfels[id].worldPositionRadius = vec4(0.0);
        surfels[id].localPositionAge = vec4(0.0);
        surfels[id].worldNormalRecycle = vec4(0.0, 1.0, 0.0, 0.0);
        surfels[id].localNormalDebug = vec4(0.0, 1.0, 0.0, 0.0);
        surfels[id].ids = uvec4(0u, 0u, id, HashUInt(id + 17u));
        surfels[id].frames = uvec4(0u);
        surfels[id].grid = uvec4(0u);
        surfels[id].metrics = vec4(0.0);
        freeStack[id] = maxSurfels - 1u - id;
        recycleStack[id] = 0u;
    }

    uint tileCount = uint(max(uTileCount.x, 1.0)) * uint(max(uTileCount.y, 1.0));
    if (id < tileCount) {
        tileCoverage[id] = uvec4(0u);
    }

    if (id < uint(max(uGridCellCount, 1))) {
        gridHeaders[id] = uvec4(0u);
    }
}
