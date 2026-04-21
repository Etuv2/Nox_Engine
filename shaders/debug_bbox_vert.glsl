#version 460 core

// Debug Bounding Box Visualization

layout(location = 0) in vec3 aPos;  // Local box vertex position (unit cube [0,1]^3)

// Per-instance data from SSBO
struct BBoxInstance {
    mat4 modelMatrix;   // World transform
    vec4 color;         // RGBA color
    vec4 boundsMin;     // AABB min (w unused)
    vec4 boundsMax;     // AABB max (w unused)
};

// Instance data SSBO (binding point 4)
layout(std430, binding = 4) readonly buffer InstanceData {
    BBoxInstance instances[];
};

uniform mat4 u_ViewProjection;

// Output to fragment shader
out vec4 v_Color;

void main()
{
    uint instanceIdx = uint(gl_InstanceID);
    
    BBoxInstance inst = instances[instanceIdx];
    
    // Transform unit cube [0,1] to actual bounding box using min/max
    vec3 localPos = mix(inst.boundsMin.xyz, inst.boundsMax.xyz, aPos);
    
    // Apply world transform and view-projection
    vec4 worldPos = inst.modelMatrix * vec4(localPos, 1.0);
    gl_Position = u_ViewProjection * worldPos;
    
    // Pass color to fragment shader
    v_Color = inst.color;
}
