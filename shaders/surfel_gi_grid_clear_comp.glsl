#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

uniform int uGridCellCount;

void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= uint(max(uGridCellCount, 0))) {
        return;
    }
    gridHeaders[id] = uvec4(0u);
}
