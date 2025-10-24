#version 460 core

in vec3 gWorldPosition;

// Geometry volume (write-only)
layout(r8, binding = 0) uniform image3D u_geometryVolume;

uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

ivec3 WorldToVoxel(vec3 worldPos) {
    vec3 localPos = worldPos - u_gridCenter;
    vec3 voxelPos = (localPos / u_voxelSize) + vec3(u_gridResolution * 0.5);
    return ivec3(voxelPos);
}

void main() {
    ivec3 voxelCoords = WorldToVoxel(gWorldPosition);
    
    // Check bounds
    if (any(lessThan(voxelCoords, ivec3(0))) || 
        any(greaterThanEqual(voxelCoords, ivec3(u_gridResolution)))) {
        discard;
    }
    
    // Mark voxel as occupied (value = 1.0)
    imageStore(u_geometryVolume, voxelCoords, vec4(1.0));
}
