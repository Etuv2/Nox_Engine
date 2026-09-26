#version 460 core

// Directional geometry volume for LPV occlusion. Each cell keeps 6 bits: for every axis, whether
// a surface facing that axis lies in the lower or the upper half of the cell along it. Light
// propagating from cell q to its neighbour p along an axis crosses q's upper half and p's lower
// half (for the + direction), so propagation can block exactly the surfaces it passes through
// while light travelling along a surface (floor, wall) keeps going.

in vec3 gWorldPosition;
flat in vec3 gNormal;

layout(r32ui, binding = 0) uniform uimage3D u_geometryVolume;

uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

void main() {
    vec3 gridPos = (gWorldPosition - u_gridCenter) / u_voxelSize + vec3(float(u_gridResolution) * 0.5);
    ivec3 cell = ivec3(floor(gridPos));
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(u_gridResolution)))) {
        discard;
    }

    vec3 cellFraction = gridPos - vec3(cell);
    vec3 facing = abs(gNormal);
    uint bits = 0u;
    for (int axis = 0; axis < 3; ++axis) {
        // Surfaces tilted up to ~70 degrees away from an axis still block light along it.
        if (facing[axis] > 0.35) {
            bits |= 1u << uint(axis * 2 + (cellFraction[axis] >= 0.5 ? 1 : 0));
        }
    }
    if (bits != 0u) {
        imageAtomicOr(u_geometryVolume, cell, bits);
    }
}
