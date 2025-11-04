#version 460 core

// Edge-aware bilateral blur for SSGI denoising - ALIGNED WITH SSAO IMPLEMENTATION
// Preserves edges using depth and normal information while maximizing noise reduction

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D inTex;       // Raw SSGI to denoise (rgba: rgb=indirect, a=mask) - QUARTER RES
layout (binding = 1) uniform sampler2D depthTex;  // Depth for edge detection - FULL RES
layout (binding = 2) uniform sampler2D normalTex;   // Normals for edge detection (oct-encoded) - FULL RES

// Output texture - rgba16f
layout (binding = 3, rgba16f) writeonly uniform image2D outTex;

// Uniforms
uniform float depthSigma;    // Depth difference threshold (view-space)
uniform float normalThresh;  // Normal difference threshold
uniform vec2 invWork; // 1.0 / quarter resolution (working res for THIS pass)

// CRITICAL FIX: Use exact same octahedral decoding as SSAO
vec3 octDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) {
        vec2 s = vec2(sign(e.x), sign(e.y));
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    
    // Compute UVs for both quarter-res and full-res sampling
    vec2 uvQuarter = (vec2(id) + 0.5) * invWork;
    
    // Map to screen-space for full-res depth/normal sampling
vec2 uvScreen = uvQuarter;

    // CRITICAL FIX: Sample depth/normal from FULL RESOLUTION (matches SSAO exactly)
  float centerDepth = textureLod(depthTex, uvScreen, 0).r;
    vec3 centerNormal = octDecode(textureLod(normalTex, uvScreen, 0).rg);
    
    // Sample SSGI from QUARTER RESOLUTION
    vec4 centerColor = textureLod(inTex, uvQuarter, 0);

    // Early exit for invalid pixels
    if (centerColor.a < 0.01) {
        imageStore(outTex, id, centerColor);
        return;
    }

    // CRITICAL FIX: Use exact same bilateral weighting as SSAO (5x5 kernel)
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
            float neighborDepth = textureLod(depthTex, uvNScreen, 0).r;
      vec3 neighborNormal = octDecode(textureLod(normalTex, uvNScreen, 0).rg);

    // CRITICAL FIX: Use exact same weight calculation as SSAO
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
