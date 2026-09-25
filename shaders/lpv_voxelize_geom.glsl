#version 460 core

layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

in vec3 worldPosition[];

uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

out vec3 gWorldPosition;
flat out vec3 gNormal;

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
    
    // Project triangle onto dominant axis
    vec4 projected[3];
    for (int i = 0; i < 3; ++i) {
        vec3 localPos = worldPosition[i] - u_gridCenter;
        vec3 voxelPos = (localPos / u_voxelSize) + vec3(u_gridResolution * 0.5);
        vec3 ndc = (voxelPos / float(u_gridResolution)) * 2.0 - 1.0;
        if (dominantAxis == 0) {
            projected[i] = vec4(ndc.y, ndc.z, 0.0, 1.0);
        } else if (dominantAxis == 1) {
            projected[i] = vec4(ndc.x, ndc.z, 0.0, 1.0);
        } else {
            projected[i] = vec4(ndc.x, ndc.y, 0.0, 1.0);
        }
    }

    // The projection flips the winding of half the triangles and the mesh's own face-culling
    // state stays active, so emit both windings: whichever survives culling voxelizes it.
    const int order[6] = int[](0, 1, 2, 0, 2, 1);
    for (int k = 0; k < 6; ++k) {
        int i = order[k];
        gWorldPosition = worldPosition[i];
        gNormal = normal;
        gl_Position = projected[i];
        EmitVertex();
        if (k == 2) {
            EndPrimitive();
        }
    }
    EndPrimitive();
}
