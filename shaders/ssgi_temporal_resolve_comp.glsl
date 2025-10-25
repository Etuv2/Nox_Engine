#version 460 core

// Temporal accumulation for SSGI stability with motion-vector reprojection
// Blends current frame with reprojected history using exponential moving average

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D curSSGI;   // Current frame (denoised, rgba with a=mask)
layout (binding = 1) uniform sampler2D prevSSGI;  // Previous frame SSGI
layout (binding = 2) uniform sampler2D velocityTex; // Motion vectors in UV space (currentUV - prevUV)
layout (binding = 3) uniform sampler2D depthTex; // Current depth (for rejection)
layout (binding = 4) uniform sampler2D normalTex; // Current normals (oct encoded RG)

// Output
layout (binding = 5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform float alpha; // Base blend factor (0..1, smaller = more history)
uniform float depthThreshold; // Depth rejection threshold
uniform float normalThreshold; // Normal rejection threshold

// Oct normal decode
vec3 octDecode(vec2 e) {
    vec2 f = e * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0 ? -t : t);
    n.y += (n.y >= 0.0 ? -t : t);
    return normalize(n);
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(outSSGI);
    if (any(greaterThanEqual(id, sz))) return;
    vec2 uv = (vec2(id) + 0.5) / vec2(sz);

    // Sample current SSGI
  vec4 cur = texture(curSSGI, uv);
    
    // Sample motion vector (in UV space: current - previous)
    vec2 vel = texture(velocityTex, uv).rg;
    
    // Reproject to previous frame position
    vec2 prevUV = uv - vel;

    // Check if reprojection is valid (within screen bounds)
    bool prevValid = all(greaterThanEqual(prevUV, vec2(0.001))) && 
   all(lessThan(prevUV, vec2(0.999)));
    
    // Sample previous SSGI at reprojected location
    vec4 prev = prevValid ? texture(prevSSGI, prevUV) : vec4(0.0);

    // Depth-based rejection
    float depthConfidence = 1.0;
    if (prevValid) {
    float currDepth = texture(depthTex, uv).r;
     float prevDepth = texture(depthTex, prevUV).r;
        float depthDiff = abs(currDepth - prevDepth);
   depthConfidence = depthDiff < depthThreshold ? 1.0 : 0.0;
    }

    // Normal-based rejection
    float normalConfidence = 1.0;
    if (prevValid && depthConfidence > 0.0) {
     vec3 currNormal = octDecode(texture(normalTex, uv).rg);
        vec3 prevNormal = octDecode(texture(normalTex, prevUV).rg);
        float normDiff = 1.0 - dot(currNormal, prevNormal);
        normalConfidence = normDiff < normalThreshold ? 1.0 : 0.0;
    }

    // Motion-based confidence (reduce history weight for fast-moving pixels)
    float velLen = length(vel * vec2(sz));
    float motionConfidence = exp(-velLen * 0.05); // Adjust sensitivity

    // Combined confidence
    float totalConfidence = prevValid ? 
        (depthConfidence * normalConfidence * motionConfidence) : 0.0;

    // Neighborhood clamping to reduce ghosting
    // Sample 3x3 neighborhood of current frame
    float minL = 1e9;
    float maxL = -1e9;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 nUV = uv + vec2(i, j) / vec2(sz);
   nUV = clamp(nUV, vec2(0.0), vec2(1.0));
   vec3 c = texture(curSSGI, nUV).rgb;
            float L = luma(c);
        minL = min(minL, L);
 maxL = max(maxL, L);
        }
    }
  
    // Clamp history luminance to current neighborhood
    float hL = luma(prev.rgb);
    float clampedL = clamp(hL, minL, maxL);
    float lumScale = (hL > 1e-6) ? (clampedL / hL) : 1.0;
    vec3 prevClamped = prev.rgb * lumScale;

    // Adaptive blend factor based on confidence
    float dynamicAlpha = mix(alpha, 1.0, 1.0 - totalConfidence);
    dynamicAlpha = clamp(dynamicAlpha, 0.05, 0.95);

    // Blend current with clamped history
    vec3 result = mix(prevClamped, cur.rgb, dynamicAlpha);
    
    // Alpha channel: blend validity masks
    float outA = max(cur.a, 0.95 * prev.a * totalConfidence);
    
    imageStore(outSSGI, id, vec4(result, outA));
}
