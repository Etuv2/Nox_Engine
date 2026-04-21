#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) buffer GridEntryBuffer {
    uint gridEntries[];
};

uniform mat4 uView;

void InsertIntoCell(uint cellIndex, uint surfelID)
{
    if (cellIndex >= header.tiling.w) {
        return;
    }
    uint slot = atomicAdd(gridHeaders[cellIndex].x, 1u);
    if (slot < header.gridDims.w) {
        gridEntries[cellIndex * header.gridDims.w + slot] = surfelID;
    } else {
        atomicAdd(gridHeaders[cellIndex].y, 1u);
    }
}

void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= header.counts.x) {
        return;
    }

    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s)) {
        return;
    }

    vec3 viewPos = (uView * vec4(s.worldPositionRadius.xyz, 1.0)).xyz;
    vec3 gridCoord = NonLinearGridCoord(viewPos, header);
    uvec3 baseCell = uvec3(floor(gridCoord));
    uint primaryCell = GridCellIndexFromCoord(baseCell, header);

    surfels[id].grid.y = surfels[id].grid.x;
    surfels[id].grid.x = primaryCell;
    surfels[id].grid.z = 1u;

    InsertIntoCell(primaryCell, id);

    vec3 fracCoord = fract(gridCoord);
    float approxCellRadius = clamp(s.worldPositionRadius.w / max(header.gridParams.w, 0.05), 0.05, 1.0);
    uvec3 dims = max(header.gridDims.xyz, uvec3(1u));

    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec3 offset = ivec3(x, y, z);
                if (all(equal(offset, ivec3(0)))) {
                    continue;
                }

                bool overlaps = true;
                if (offset.x < 0) overlaps = overlaps && fracCoord.x < approxCellRadius;
                if (offset.x > 0) overlaps = overlaps && (1.0 - fracCoord.x) < approxCellRadius;
                if (offset.y < 0) overlaps = overlaps && fracCoord.y < approxCellRadius;
                if (offset.y > 0) overlaps = overlaps && (1.0 - fracCoord.y) < approxCellRadius;
                if (offset.z < 0) overlaps = overlaps && fracCoord.z < approxCellRadius;
                if (offset.z > 0) overlaps = overlaps && (1.0 - fracCoord.z) < approxCellRadius;
                if (!overlaps) {
                    continue;
                }

                ivec3 neighbor = ivec3(baseCell) + offset;
                if (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, ivec3(dims)))) {
                    continue;
                }

                InsertIntoCell(GridCellIndexFromCoord(uvec3(neighbor), header), id);
                surfels[id].grid.z += 1u;
            }
        }
    }
}
