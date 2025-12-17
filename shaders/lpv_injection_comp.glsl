#version 460 core

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// RSM textures (input)
uniform sampler2D u_rsmPosition;
uniform sampler2D u_rsmNormal;
uniform sampler2D u_rsmFlux;

// LPV 3D textures (output) - Use RGBA32UI for atomic operations
layout(r32ui, binding = 0) uniform uimage3D u_lpvR;
layout(r32ui, binding = 1) uniform uimage3D u_lpvG;
layout(r32ui, binding = 2) uniform uimage3D u_lpvB;

// Grid parameters
uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;
uniform int u_rsmResolution;
uniform int u_sampleCount;
uniform vec4 u_gridOrientation; // Quaternion (x, y, z, w)

// Scaling factor for float-to-int conversion
// We use fixed-point arithmetic: float_value * SCALE_FACTOR = int_value
// Increase precision to avoid truncation to zero for small contributions
const uint SCALE_FACTOR = 100000u;

// Convert float to scaled unsigned integer for atomic operations
uint floatToScaledUInt(float value) {
    // Clamp to prevent overflow (max value ~4294967295 for uint)
    // 4000 * 100000 = 400,000,000 (safe headroom for accumulation)
    float clampedValue = clamp(value, 0.0, 4000.0);
    return uint(clampedValue * float(SCALE_FACTOR));
}

// Quaternion rotation helper
vec3 rotateVector(vec3 v, vec4 q) {
    vec3 qxyz = q.xyz;
    float qw = q.w;
    vec3 t = 2.0 * cross(qxyz, v);
    return v + qw * t + cross(qxyz, t);
}

// Inverse quaternion rotation
vec3 rotateVectorInverse(vec3 v, vec4 q) {
    vec4 qConj = vec4(-q.x, -q.y, -q.z, q.w);
    return rotateVector(v, qConj);
}

// Spherical Harmonics basis functions for cosine lobe
vec4 SH_CosineLobe(vec3 normal) {
    // Y0,0 (constant term)
    float Y00 = 0.282095; // sqrt(1/(4*PI))
    
    // Y1,-1, Y1,0, Y1,1 (linear terms for cosine lobe)
    float Y1_1 = 0.488603 * normal.x; // sqrt(3/(4*PI)) * x
    float Y10 = 0.488603 * normal.y;  // sqrt(3/(4*PI)) * y
    float Y11 = 0.488603 * normal.z;  // sqrt(3/(4*PI)) * z
    
    // Pack SH coefficients (4-band approximation)
    // Multiply by PI to convert from cosine-weighted to hemispherical integration
    const float PI = 3.14159265359;
    return vec4(Y00, Y1_1, Y10, Y11) * PI;
}

// Convert world position to voxel coordinates (accounting for rotation)
ivec3 WorldToVoxel(vec3 worldPos) {
    // Transform to local grid space
    vec3 localPos = worldPos - u_gridCenter;
    
    // Apply inverse rotation to align with grid axes
    localPos = rotateVectorInverse(localPos, u_gridOrientation);
    
    // Convert to voxel coordinates
    vec3 voxelPos = (localPos / u_voxelSize) + vec3(u_gridResolution * 0.5);
    return ivec3(voxelPos);
}

void main() {
    uint threadID = gl_GlobalInvocationID.x;
    
    // Check if thread is within sample count
    if (threadID >= u_sampleCount) {
        return;
    }
    
    // Generate pseudo-random RSM sample position using thread ID
    // Use a simple LCG (Linear Congruential Generator)
    uint seed = threadID * 1664525u + 1013904223u;
    seed = seed * 1664525u + 1013904223u;
    
    float u = float(seed & 0xFFFFu) / 65536.0;
    seed = seed * 1664525u + 1013904223u;
    float v = float(seed & 0xFFFFu) / 65536.0;
    
    vec2 rsmTexCoord = vec2(u, v);
    
    // Sample RSM
    vec3 vplPosition = textureLod(u_rsmPosition, rsmTexCoord, 0).xyz;
    vec3 vplNormal = normalize(textureLod(u_rsmNormal, rsmTexCoord, 0).xyz);
    vec3 vplFlux = textureLod(u_rsmFlux, rsmTexCoord, 0).xyz;
    
    // Check if VPL is valid (flux > 0)
    if (dot(vplFlux, vplFlux) < 0.0001) {
        return;
    }
    
    // Convert VPL position to voxel coordinates (handles rotation)
    ivec3 voxelCoords = WorldToVoxel(vplPosition);
    
    // Check if voxel is inside grid
    if (any(lessThan(voxelCoords, ivec3(0))) || 
        any(greaterThanEqual(voxelCoords, ivec3(u_gridResolution)))) {
        return;
    }
    
    // Transform normal to grid-local space for proper SH evaluation
    vec3 localNormal = rotateVectorInverse(vplNormal, u_gridOrientation);
    
    // Calculate SH coefficients for this VPL's cosine lobe (in grid-local space)
    vec4 shCoeffs = SH_CosineLobe(localNormal);
    
    // Inject flux into LPV grid (split RGB channels)
    // Scale flux by solid angle to properly distribute energy
    const float PI = 3.14159265359;
    float solidAngle = (4.0 * PI) / float(u_sampleCount);
    
    vec4 contributionR = shCoeffs * vplFlux.r * solidAngle;
    vec4 contributionG = shCoeffs * vplFlux.g * solidAngle;
    vec4 contributionB = shCoeffs * vplFlux.b * solidAngle;
    
    // Convert float contributions to scaled unsigned integers
    uvec4 scaledR = uvec4(
        floatToScaledUInt(contributionR.x),
        floatToScaledUInt(contributionR.y),
        floatToScaledUInt(contributionR.z),
        floatToScaledUInt(contributionR.w)
    );
    
    uvec4 scaledG = uvec4(
        floatToScaledUInt(contributionG.x),
        floatToScaledUInt(contributionG.y),
        floatToScaledUInt(contributionG.z),
        floatToScaledUInt(contributionG.w)
    );
    
    uvec4 scaledB = uvec4(
        floatToScaledUInt(contributionB.x),
        floatToScaledUInt(contributionB.y),
        floatToScaledUInt(contributionB.z),
        floatToScaledUInt(contributionB.w)
    );
    
    // ATOMIC OPERATIONS: Guaranteed thread-safe accumulation
    // imageAtomicAdd only works on single scalar components (uint, not uvec4)
    // We need to store each SH coefficient in separate voxels along a 4th dimension
    // OR use 4 separate 3D textures (one per SH coefficient)
    // OR pack all 4 coefficients into a single uint and use bit operations
    
    // SOLUTION: Use offset indexing to store 4 SH coefficients in adjacent voxels
    // This requires grid to be 4x wider in X dimension
    // Alternative: Create 4 separate 3D textures (cleaner but more memory)
    
    // Store each SH band in separate voxel locations with offset
    ivec3 offset0 = voxelCoords; // Y0,0
    ivec3 offset1 = voxelCoords + ivec3(u_gridResolution, 0, 0); // Y1,-1
    ivec3 offset2 = voxelCoords + ivec3(u_gridResolution * 2, 0, 0); // Y1,0
    ivec3 offset3 = voxelCoords + ivec3(u_gridResolution * 3, 0, 0); // Y1,1
    
    // Atomic accumulation for R channel (all 4 SH coefficients)
    imageAtomicAdd(u_lpvR, offset0, scaledR.x);
    imageAtomicAdd(u_lpvR, offset1, scaledR.y);
    imageAtomicAdd(u_lpvR, offset2, scaledR.z);
    imageAtomicAdd(u_lpvR, offset3, scaledR.w);
    
    // Atomic accumulation for G channel (all 4 SH coefficients)
    imageAtomicAdd(u_lpvG, offset0, scaledG.x);
    imageAtomicAdd(u_lpvG, offset1, scaledG.y);
    imageAtomicAdd(u_lpvG, offset2, scaledG.z);
    imageAtomicAdd(u_lpvG, offset3, scaledG.w);
    
    // Atomic accumulation for B channel (all 4 SH coefficients)
    imageAtomicAdd(u_lpvB, offset0, scaledB.x);
    imageAtomicAdd(u_lpvB, offset1, scaledB.y);
    imageAtomicAdd(u_lpvB, offset2, scaledB.z);
    imageAtomicAdd(u_lpvB, offset3, scaledB.w);
}
