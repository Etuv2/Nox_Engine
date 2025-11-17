#version 460 core

/**
 * @file svgf_variance_comp.glsl
 * @brief SVGF Variance Estimation Pass
 * 
 * Computes luminance variance from moments for adaptive filtering.
 * Uses a small spatial filter to stabilize variance estimates.
 */

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Input: Moments (mean and M2 from temporal pass)
layout (binding = 0) uniform sampler2D u_momentsTexture;

// Input: History length
layout (binding = 1) uniform sampler2D u_historyLengthTexture;

// Output: Variance texture (luminance variance in R channel)
layout (r16f, binding = 2) uniform image2D u_varianceImage;

// Uniforms
uniform vec2 u_resolution;

// Constants
#define VARIANCE_KERNEL_RADIUS 1

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    
    if (pixelCoord.x >= int(u_resolution.x) || pixelCoord.y >= int(u_resolution.y)) {
        return;
    }
    
    // Sample center pixel moments
    vec2 centerMoments = texelFetch(u_momentsTexture, pixelCoord, 0).rg;
    float centerHistoryLength = texelFetch(u_historyLengthTexture, pixelCoord, 0).r;
    
    // If no history, variance is high
 if (centerHistoryLength < 1.0) {
        imageStore(u_varianceImage, pixelCoord, vec4(1.0));
        return;
    }
    
    // Compute variance from moments
    // Variance = M2 / (n - 1)
    float variance = centerMoments.y / max(centerHistoryLength - 1.0, 1.0);
    
    // Apply small spatial filter to stabilize variance estimates
    float varianceSum = 0.0;
    float weightSum = 0.0;
    
    for (int dy = -VARIANCE_KERNEL_RADIUS; dy <= VARIANCE_KERNEL_RADIUS; dy++) {
     for (int dx = -VARIANCE_KERNEL_RADIUS; dx <= VARIANCE_KERNEL_RADIUS; dx++) {
ivec2 sampleCoord = pixelCoord + ivec2(dx, dy);
            
   // Check bounds
          if (sampleCoord.x < 0 || sampleCoord.x >= int(u_resolution.x) ||
           sampleCoord.y < 0 || sampleCoord.y >= int(u_resolution.y)) {
        continue;
   }
            
   vec2 sampleMoments = texelFetch(u_momentsTexture, sampleCoord, 0).rg;
      float sampleHistoryLength = texelFetch(u_historyLengthTexture, sampleCoord, 0).r;
          
          if (sampleHistoryLength > 0.0) {
       float sampleVariance = sampleMoments.y / max(sampleHistoryLength - 1.0, 1.0);
         
              // Simple box filter
       float weight = 1.0;
            varianceSum += sampleVariance * weight;
     weightSum += weight;
            }
        }
    }
    
  // Compute filtered variance
    float filteredVariance = (weightSum > 0.0) ? (varianceSum / weightSum) : variance;
    
    // Store variance (clamped to reasonable range)
    imageStore(u_varianceImage, pixelCoord, vec4(clamp(filteredVariance, 0.0, 10.0)));
}
