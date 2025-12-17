#version 460 core

/**
 * @file svgf_atrous_comp.glsl
 * @brief SVGF À-Trous Wavelet Filter Pass
 * 
 * Performs edge-aware spatial filtering using the à-trous algorithm.
 * Uses G-buffer normals, depth, and roughness for edge preservation.
 * Filter adapts based on luminance variance - less smoothing in low-variance areas.
 */

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Input: Current radiance to filter
layout (binding = 0) uniform sampler2D u_inputRadiance;

// Input: Luminance variance
layout (binding = 1) uniform sampler2D u_varianceTexture;

// G-buffer inputs for edge detection
layout (binding = 2) uniform sampler2D u_gbufferDepth;
layout (binding = 3) uniform sampler2D u_gbufferPackedNormalRM;

// Output: Filtered radiance
layout (rgba16f, binding = 4) uniform image2D u_outputRadiance;

// Uniforms
uniform vec2 u_resolution;
uniform int u_iteration;    // À-trous iteration (controls step size)
uniform float u_phiColor;   // Color weight parameter (lower = sharper)
uniform float u_phiNormal;  // Normal weight exponent (higher = sharper)
uniform float u_phiDepth;   // Depth weight parameter (lower = sharper)

// Constants
#define PI 3.1415926535897932384626433832795
#define EPSILON 1e-6

// 3x3 à-trous kernel weights (B-spline based)
const float kernel[2] = float[2](1.0, 0.5);

// Octahedron normal decoding
vec3 DecodeNormalOct8(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    if (n.z < 0.0) {
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }
    return normalize(n);
}

// Luminance calculation
float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    
    if (pixelCoord.x >= int(u_resolution.x) || pixelCoord.y >= int(u_resolution.y)) {
        return;
    }
    
    // Sample center pixel
    vec3 centerColor = texelFetch(u_inputRadiance, pixelCoord, 0).rgb;
    float centerDepth = texelFetch(u_gbufferDepth, pixelCoord, 0).r;
    vec4 centerNormalRM = texelFetch(u_gbufferPackedNormalRM, pixelCoord, 0);
    vec3 centerNormal = DecodeNormalOct8(centerNormalRM.rg);
    float centerRoughness = centerNormalRM.b;
    float centerVariance = texelFetch(u_varianceTexture, pixelCoord, 0).r;
    
    // Sky pixels don't need filtering
    if (centerDepth >= 1.0 - EPSILON) {
        imageStore(u_outputRadiance, pixelCoord, vec4(centerColor, 1.0));
        return;
    }
    
    // Calculate step size for à-trous (exponentially increases each iteration)
    int stepSize = 1 << u_iteration;  // 1, 2, 4, 8...
    
    // Variance-adaptive filtering
    // Low variance = scene is converging, less filtering needed
    // High variance = still noisy, more filtering acceptable
    float varianceFactor = clamp(sqrt(centerVariance) * 10.0, 0.0, 1.0);
    
    // Roughness-adaptive: smooth surfaces need less filtering, rough surfaces can tolerate more
    float roughnessFactor = mix(0.3, 1.0, centerRoughness);
    
    // Combined adaptive factor - reduce filtering in clean, smooth areas
    float adaptiveFactor = max(varianceFactor, 0.1) * roughnessFactor;
    
    // Adaptive phi parameters - tighter tolerances for low-variance regions
    float adaptivePhiColor = u_phiColor * adaptiveFactor;
    float adaptivePhiDepth = u_phiDepth * (1.0 + varianceFactor);
    
    // Center luminance for color weight calculation
    float centerLuma = luminance(centerColor);
    
    // Accumulate weighted samples - start with center
    vec3 colorSum = centerColor;
    float weightSum = 1.0;
    
    // 3x3 à-trous kernel (reduced from 5x5 for less blur)
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            // Skip center (already added)
            if (dx == 0 && dy == 0) continue;
            
            ivec2 sampleOffset = ivec2(dx, dy) * stepSize;
            ivec2 sampleCoord = pixelCoord + sampleOffset;
            
            // Boundary check
            if (sampleCoord.x < 0 || sampleCoord.x >= int(u_resolution.x) ||
                sampleCoord.y < 0 || sampleCoord.y >= int(u_resolution.y)) {
                continue;
            }
            
            // Sample neighbor
            vec3 sampleColor = texelFetch(u_inputRadiance, sampleCoord, 0).rgb;
            float sampleDepth = texelFetch(u_gbufferDepth, sampleCoord, 0).r;
            vec4 sampleNormalRM = texelFetch(u_gbufferPackedNormalRM, sampleCoord, 0);
            vec3 sampleNormal = DecodeNormalOct8(sampleNormalRM.rg);
            
            // Skip sky pixels
            if (sampleDepth >= 1.0 - EPSILON) {
                continue;
            }
            
            // Kernel weight (B-spline)
            float kernelWeight = kernel[abs(dx)] * kernel[abs(dy)];
            
            // === Color/Luminance Edge Weight ===
            float sampleLuma = luminance(sampleColor);
            float lumaDiff = abs(centerLuma - sampleLuma);
            // Gaussian weight based on luminance difference
            float colorWeight = exp(-lumaDiff * lumaDiff / max(adaptivePhiColor * adaptivePhiColor, 0.0001));
            
            // === Normal Edge Weight (critical for geometric edges) ===
            float normalSimilarity = max(0.0, dot(centerNormal, sampleNormal));
            // Sharp cutoff for normals - this preserves geometric edges
            float normalWeight = pow(normalSimilarity, u_phiNormal);
            // Hard cutoff: reject samples with very different normals
            if (normalSimilarity < 0.9) {
                normalWeight *= 0.05;  // Strong rejection
            }
            
            // === Depth Edge Weight ===
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff / max(adaptivePhiDepth, 0.0001));
            // Hard cutoff for depth discontinuities
            if (depthDiff > 0.005) {
                depthWeight *= 0.01;  // Strong rejection for depth edges
            }
            
            // Combined weight
            float weight = kernelWeight * colorWeight * normalWeight * depthWeight;
            
            // Reject very low weights
            if (weight < 0.0001) continue;
            
            colorSum += sampleColor * weight;
            weightSum += weight;
        }
    }
    
    // Normalize result
    vec3 filteredColor = colorSum / max(weightSum, EPSILON);
    
    // Preserve original color more in low-variance regions (detail preservation)
    float preserveOriginal = 1.0 - varianceFactor * 0.5;
    filteredColor = mix(filteredColor, centerColor, preserveOriginal * 0.3);
    
    // Store result
    imageStore(u_outputRadiance, pixelCoord, vec4(filteredColor, 1.0));
}
