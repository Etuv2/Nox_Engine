#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    SurfelTileMeta tileCoverage[];
};

uniform int uTileCount;

void main()
{
    uint id = gl_GlobalInvocationID.x;
    uint tileCount = uint(max(uTileCount, 0));
    if (id >= tileCount) {
        return;
    }

    SurfelTileMeta tile = tileCoverage[id];
    tile.coverage.x = 0u;
    tile.geom.w = 0u;
    tile.state.z &= ~(SURFEL_TILE_FLAG_HIGH_MOTION |
        SURFEL_TILE_FLAG_NEWLY_EXPOSED |
        SURFEL_TILE_FLAG_NEAR_CAMERA);
    tileCoverage[id] = tile;
}
