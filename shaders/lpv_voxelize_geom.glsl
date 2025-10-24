#version 460 core

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

in vec3 worldPosition[];

uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

out vec3 gWorldPosition;

void main() {
    // Choose dominant axis for projection
    vec3 normal = normalize(cross(
        worldPosition[1] - worldPosition[0],
        worldPosition[2] - worldPosition[0]
    ));
    
    vec3 absNormal = abs(normal);
    int dominantAxis = 0;
    if (absNormal.y > absNormal.x && absNormal.y > absNormal.z) dominantAxis = 1;
    else if (absNormal.z > absNormal.x && absNormal.z > absNormal.y) dominantAxis = 2;
    
    // Project triangle onto dominant axis and emit vertices
    for (int i = 0; i < 3; ++i) {
        gWorldPosition = worldPosition[i];
        
        // Convert world position to voxel coordinates
        vec3 localPos = worldPosition[i] - u_gridCenter;
        vec3 voxelPos = (localPos / u_voxelSize) + vec3(u_gridResolution * 0.5);
        vec3 ndc = (voxelPos / float(u_gridResolution)) * 2.0 - 1.0;
        
        // Project based on dominant axis
        if (dominantAxis == 0) {
            gl_Position = vec4(ndc.y, ndc.z, 0.0, 1.0);
        } else if (dominantAxis == 1) {
            gl_Position = vec4(ndc.x, ndc.z, 0.0, 1.0);
        } else {
            gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
        }
        
        EmitVertex();
    }
    
    EndPrimitive();
}
