#version 460 core

// Debug visualization vertex shader for LPV grid
// Generates voxel positions on-the-fly

uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

// LPV textures for energy sampling
uniform sampler3D u_lpvTextureR;
uniform sampler3D u_lpvTextureG;
uniform sampler3D u_lpvTextureB;
uniform sampler3D u_geometryVolume;

out vec4 v_color;
out float v_size;

void main() {
    // Convert vertex ID to 3D grid coordinates (subsampled)
    int sampleStep = 4; // Match CPU side
    int gridSize = u_gridResolution / sampleStep;
    
    int z = gl_VertexID / (gridSize * gridSize);
    int y = (gl_VertexID / gridSize) % gridSize;
    int x = gl_VertexID % gridSize;
    
    // Convert to actual grid coordinates
    ivec3 voxelCoords = ivec3(x, y, z) * sampleStep + ivec3(sampleStep / 2);
    
    // Convert to world position
    vec3 localPos = (vec3(voxelCoords) - vec3(u_gridResolution) * 0.5) * u_voxelSize;
    vec3 worldPos = u_gridCenter + localPos;
    
    gl_Position = u_projection * u_view * vec4(worldPos, 1.0);
    
    // Sample LPV energy at this voxel
    vec3 uvw = (vec3(voxelCoords) + 0.5) / float(u_gridResolution);
    vec4 energyR = texture(u_lpvTextureR, uvw);
    vec4 energyG = texture(u_lpvTextureG, uvw);
    vec4 energyB = texture(u_lpvTextureB, uvw);
    
    // Reconstruct total energy (simplified - just use L0 coefficient)
    float totalR = energyR.x; // L0 coefficient
    float totalG = energyG.x;
    float totalB = energyB.x;
    
    vec3 energy = vec3(totalR, totalG, totalB) * 5.0; // Boost for visibility
    
    // Sample geometry volume
    float occlusion = texture(u_geometryVolume, uvw).r;
    
    // Color: energy color if lit, red if occluded
    if (occlusion > 0.5) {
        v_color = vec4(1.0, 0.0, 0.0, 0.8); // Red for geometry
    } else if (length(energy) > 0.01) {
        v_color = vec4(energy, 0.7); // Energy color
    } else {
        v_color = vec4(0.1, 0.1, 0.1, 0.2); // Dark gray for empty
    }
    
    // Point size based on energy
    v_size = max(3.0, length(energy) * 20.0);
    gl_PointSize = v_size;
}
