#version 460 core
#include "includes/pbr_common.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// OPTIMIZED G-BUFFER LAYOUT
uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM;  // RT0: oct normal (RG) + roughness (B) + metallic (A)

// Input textures
layout (binding = 0) uniform sampler2D inTex;       // Raw SSGI to denoise (rgba: rgb=indirect, a=mask) - QUARTER RES

// Output texture - rgba16f
layout (binding = 3, rgba16f) writeonly uniform image2D outTex;

// Uniforms
uniform float depthSigma;    // Depth difference threshold (view-space)
uniform float normalThresh;  // Normal difference threshold
uniform vec2 invWork; // 1.0 / quarter resolution (working res for THIS pass)

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    
    // Compute UVs for both quarter-res and full-res sampling
    vec2 uvQuarter = (vec2(id) + 0.5) * invWork;
    
    // Map to screen-space for full-res depth/normal sampling
vec2 uvScreen = uvQuarter;

    // Sample depth/normal from FULL RESOLUTION (matches SSAO exactly)
  float centerDepth = textureLod(gDepth, uvScreen, 0).r;
    vec2 encNormal = textureLod(gPackedNormalRM, uvScreen, 0.0).rg;
    vec3 centerNormal = DecodeNormalOct(encNormal);
    
    // Sample SSGI from QUARTER RESOLUTION
    vec4 centerColor = textureLod(inTex, uvQuarter, 0);

    // Early exit for invalid pixels
    if (centerColor.a < 0.01) {
        imageStore(outTex, id, centerColor);
        return;
    }

    // Use exact same bilateral weighting as SSAO (5x5 kernel)
    vec3 result = vec3(0.0);
    float weightSum = 0.0;

    // 5x5 kernel matching SSAO blur
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
     // Neighbor UVs in both spaces
       vec2 uvNQuarter = uvQuarter + vec2(x, y) * invWork;
            vec2 uvNScreen = uvNQuarter;
            
    // Sample neighbor SSGI from QUARTER RESOLUTION
            vec4 neighborColor = textureLod(inTex, uvNQuarter, 0);
          
          // Sample neighbor depth/normal from FULL RESOLUTION
            float neighborDepth = textureLod(gDepth, uvNScreen, 0).r;
      vec2 encNeighborNormal = textureLod(gPackedNormalRM, uvNScreen, 0.0).rg;
      vec3 neighborNormal = DecodeNormalOct(encNeighborNormal);

    // Use exact same weight calculation as SSAO
          // Depth weight: exponential falloff based on raw depth difference
   float depthDiff = abs(centerDepth - neighborDepth);
       float depthWeight = exp(-depthDiff / depthSigma);

            // Normal weight: exponential falloff based on normal difference
            float normalDiff = max(0.0, 1.0 - dot(centerNormal, neighborNormal));
        float normalWeight = exp(-normalDiff / normalThresh);

  // Combined weight (multiplicative)
        float weight = depthWeight * normalWeight;
            
   // Accumulate weighted samples
          result += neighborColor.rgb * weight;
      weightSum += weight;
        }
    }

    // Normalize result (matches SSAO exactly)
    vec3 outRGB = result / max(weightSum, 1e-5);

    // Preserve alpha channel
    float outA = centerColor.a;
  
    imageStore(outTex, id, vec4(outRGB, outA));
}
