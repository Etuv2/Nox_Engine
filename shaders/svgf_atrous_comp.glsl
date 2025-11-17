#version 460 core

/**
 * @file svgf_atrous_comp.glsl
 * @brief SVGF À-Trous Wavelet Filter Pass
 * 
 * Performs edge-aware spatial filtering using the à-trous algorithm.
 * Uses G-buffer normals and depth for edge preservation.
 * Filter kernel size adapts based on luminance variance.
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
uniform float u_phiColor;          // Color weight parameter
uniform float u_phiNormal; // Normal weight parameter (in radians)
uniform float u_phiDepth;          // Depth weight parameter

// Constants
#define PI 3.1415926535897932384626433832795
#define EPSILON 1e-4

// À-trous 5x5 kernel weights
const float kernel[3][3] = {
    { 1.0/16.0, 2.0/16.0, 1.0/16.0 },
    { 2.0/16.0, 4.0/16.0, 2.0/16.0 },
    { 1.0/16.0, 2.0/16.0, 1.0/16.0 }
};

// Octahedron normal decoding
vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
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
    
    // Sample center pixel
    vec3 centerColor = texelFetch(u_inputRadiance, pixelCoord, 0).rgb;
    float centerDepth = texelFetch(u_gbufferDepth, pixelCoord, 0).r;
    vec4 centerNormalRM = texelFetch(u_gbufferPackedNormalRM, pixelCoord, 0);
    vec3 centerNormal = octDecode(centerNormalRM.rg);
    float centerVariance = texelFetch(u_varianceTexture, pixelCoord, 0).r;
    
    // Sky pixels don't need filtering
    if (centerDepth >= 1.0 - EPSILON) {
        imageStore(u_outputRadiance, pixelCoord, vec4(centerColor, 1.0));
      return;
    }
    
    // Calculate step size for à-trous (exponentially increases each iteration)
    int stepSize = 1 << u_iteration;  // 1, 2, 4, 8, 16...
    
    // Adaptive kernel size based on variance
    // High variance = larger kernel for more smoothing
    float varianceScale = sqrt(max(centerVariance, 0.0));
    float adaptivePhiColor = u_phiColor * (1.0 + varianceScale);
    
    // Accumulate weighted samples
    vec3 colorSum = vec3(0.0);
    float weightSum = 0.0;
    
 // 3x3 à-trous kernel
    for (int dy = -1; dy <= 1; dy++) {
for (int dx = -1; dx <= 1; dx++) {
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
 vec3 sampleNormal = octDecode(sampleNormalRM.rg);
        
    // Skip sky pixels
            if (sampleDepth >= 1.0 - EPSILON) {
      continue;
            }
       
            // Kernel weight
   float kernelWeight = kernel[dy + 1][dx + 1];
    
            // Color/luminance weight
     float centerLuma = luminance(centerColor);
  float sampleLuma = luminance(sampleColor);
     float lumaDiff = abs(centerLuma - sampleLuma);
            float colorWeight = exp(-lumaDiff / adaptivePhiColor);
            
    // Normal weight (cosine similarity)
   float normalSimilarity = max(0.0, dot(centerNormal, sampleNormal));
     float normalWeight = pow(normalSimilarity, u_phiNormal);
          
            // Depth weight (relative depth difference)
     float depthDiff = abs(centerDepth - sampleDepth);
   float depthWeight = exp(-depthDiff / u_phiDepth);
            
        // Combined weight
     float weight = kernelWeight * colorWeight * normalWeight * depthWeight;
        
            colorSum += sampleColor * weight;
            weightSum += weight;
     }
    }
    
    // Normalize
    vec3 filteredColor = (weightSum > EPSILON) ? (colorSum / weightSum) : centerColor;
    
    // Store result
    imageStore(u_outputRadiance, pixelCoord, vec4(filteredColor, 1.0));
}
