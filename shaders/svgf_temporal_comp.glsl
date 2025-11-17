#version 460 core

/**
 * @file svgf_temporal_comp.glsl
 * @brief SVGF Temporal Reprojection Pass
 * 
 * Performs temporal reprojection and accumulation of radiance samples.
 * Uses motion vectors and depth to reproject previous frame samples,
 * calculating variance for adaptive filtering.
 */

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Input: Current frame noisy radiance
layout (binding = 0) uniform sampler2D u_currentRadiance;

// Input: Previous frame denoised radiance
layout (binding = 1) uniform sampler2D u_prevRadiance;

// G-buffer inputs for validation
layout (binding = 2) uniform sampler2D u_gbufferDepth;
layout (binding = 3) uniform sampler2D u_gbufferPackedNormalRM;

// Input: Previous frame depth
layout (binding = 4) uniform sampler2D u_prevDepth;

// Output: Temporally accumulated radiance
layout (rgba16f, binding = 5) uniform image2D u_outputRadiance;

// Output: Moments (mean and variance)
layout (rg32f, binding = 6) uniform image2D u_momentsImage;

// Output: History length
layout (r16f, binding = 7) uniform image2D u_historyLengthImage;

// Uniforms
uniform vec2 u_resolution;
uniform mat4 u_invView;
uniform mat4 u_invProj;
uniform mat4 u_prevViewProj;
uniform float u_cameraNear;
uniform float u_cameraFar;
uniform int u_frameIndex;

// SVGF parameters
uniform float u_temporalAlpha;    // Blend factor (0.1-0.2 recommended)
uniform float u_varianceClipGamma;       // Variance clipping gamma (1.0-2.0)
uniform float u_depthThreshold;      // Depth similarity threshold
uniform float u_normalThreshold;         // Normal similarity threshold (cos angle)

// Constants
#define PI 3.1415926535897932384626433832795
#define EPSILON 1e-4

// Octahedron normal decoding
vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
  n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Reconstruct world position from depth
vec3 reconstructWorldPosition(vec2 uv, float depth) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewSpace = u_invProj * clipSpace;
    viewSpace /= viewSpace.w;
    vec4 worldSpace = u_invView * viewSpace;
    return worldSpace.xyz;
}

// Luminance calculation
float luminance(vec3 color) {
 return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pixelCoord) + 0.5) / u_resolution;
    
    if (pixelCoord.x >= int(u_resolution.x) || pixelCoord.y >= int(u_resolution.y)) {
        return;
    }
    
    // Sample current frame data
  vec3 currentRadiance = texelFetch(u_currentRadiance, pixelCoord, 0).rgb;
 float depth = texelFetch(u_gbufferDepth, pixelCoord, 0).r;
    vec4 normalRM = texelFetch(u_gbufferPackedNormalRM, pixelCoord, 0);
    vec3 normal = octDecode(normalRM.rg);
    
    // Check for sky/background pixels
    if (depth >= 1.0 - EPSILON) {
        // Sky pixel - no temporal filtering
        imageStore(u_outputRadiance, pixelCoord, vec4(currentRadiance, 1.0));
      imageStore(u_momentsImage, pixelCoord, vec4(0.0, 0.0, 0.0, 0.0));
        imageStore(u_historyLengthImage, pixelCoord, vec4(0.0));
  return;
  }
    
    // Reconstruct world position for reprojection
    vec3 worldPos = reconstructWorldPosition(uv, depth);
 
    // Reproject to previous frame
  vec4 prevClipPos = u_prevViewProj * vec4(worldPos, 1.0);
    prevClipPos.xyz /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5 + 0.5;
    
    // Check if reprojection is valid (within screen bounds)
    bool validReproject = all(greaterThanEqual(prevUV, vec2(0.0))) && 
            all(lessThanEqual(prevUV, vec2(1.0)));
 
    // Initialize history length
    float historyLength = 0.0;
    vec3 prevRadiance = vec3(0.0);
    vec2 prevMoments = vec2(0.0);
  
    if (validReproject) {
   ivec2 prevPixel = ivec2(prevUV * u_resolution);
        
   // Sample previous frame data
        prevRadiance = texelFetch(u_prevRadiance, prevPixel, 0).rgb;
        float prevDepth = texelFetch(u_prevDepth, prevPixel, 0).r;
        prevMoments = imageLoad(u_momentsImage, prevPixel).rg;
        historyLength = imageLoad(u_historyLengthImage, prevPixel).r;
        
        // Validate temporal reprojection
        vec3 prevWorldPos = reconstructWorldPosition(prevUV, prevDepth);
    float worldDepthDiff = length(worldPos - prevWorldPos);
      
        // Simple depth validation
        if (worldDepthDiff > u_depthThreshold) {
            validReproject = false;
        }
  } else {
        validReproject = false;
    }
    
    // Temporal accumulation
    vec3 outputRadiance;
    vec2 outputMoments;
    float outputHistoryLength;
    
    if (validReproject && historyLength > 0.0) {
        // Valid history - blend with previous frame
        float alpha = max(u_temporalAlpha, 1.0 / (historyLength + 1.0));
     outputRadiance = mix(prevRadiance, currentRadiance, alpha);
        
        // Update moments for variance calculation
 float currentLuma = luminance(currentRadiance);
        float prevMean = prevMoments.x;
        float prevM2 = prevMoments.y;
     
        // Welford's online variance algorithm
      float newMean = mix(prevMean, currentLuma, alpha);
    float delta = currentLuma - prevMean;
      float delta2 = currentLuma - newMean;
        float newM2 = prevM2 + delta * delta2;
    
        outputMoments = vec2(newMean, newM2);
        outputHistoryLength = min(historyLength + 1.0, 64.0);
    } else {
        // No valid history - use current frame
        outputRadiance = currentRadiance;
        float currentLuma = luminance(currentRadiance);
        outputMoments = vec2(currentLuma, 0.0);
        outputHistoryLength = 1.0;
    }
    
    // Write outputs
    imageStore(u_outputRadiance, pixelCoord, vec4(outputRadiance, 1.0));
    imageStore(u_momentsImage, pixelCoord, vec4(outputMoments, 0.0, 0.0));
    imageStore(u_historyLengthImage, pixelCoord, vec4(outputHistoryLength, 0.0, 0.0, 0.0));
}
