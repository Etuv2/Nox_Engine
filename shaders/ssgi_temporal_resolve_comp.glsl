#version 460 core

// Temporal accumulation for SSGI stability with motion-vector reprojection
// Implements exponential moving average: result = mix(history, current, alpha)
// where alpha is SMALL (0.1-0.2) to give more weight to stable history

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
uniform float alpha; // Blend factor (0..1) - SMALL values (0.1-0.2) favor history for stability
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

    // Sample current SSGI (this frame's result after spatial denoising)
    vec4 cur = texture(curSSGI, uv);
    
    // Sample motion vector (in UV space: current - previous)
    vec2 vel = texture(velocityTex, uv).rg;
    
    // Reproject to previous frame position
    vec2 prevUV = uv - vel;

    // Check if reprojection is valid (within screen bounds with margin)
    bool prevValid = all(greaterThanEqual(prevUV, vec2(0.01))) && 
             all(lessThan(prevUV, vec2(0.99)));
    
    // Sample previous SSGI at reprojected location with bilinear filtering
    vec4 prev = prevValid ? texture(prevSSGI, prevUV) : vec4(0.0);

    // === REJECTION TESTS ===
    // These determine if history is reliable or should be discarded
    
    float totalConfidence = 1.0;
    
    if (prevValid) {
     // Depth-based rejection - detect disocclusions
    float currDepth = texture(depthTex, uv).r;
        float prevDepth = texture(depthTex, prevUV).r;
   float depthDiff = abs(currDepth - prevDepth);
        float depthConfidence = smoothstep(depthThreshold * 2.0, depthThreshold * 0.5, depthDiff);
   totalConfidence *= depthConfidence;
        
        // Normal-based rejection - detect surface orientation changes
        vec3 currNormal = octDecode(texture(normalTex, uv).rg);
vec3 prevNormal = octDecode(texture(normalTex, prevUV).rg);
  float normalDot = dot(currNormal, prevNormal);
float normalConfidence = smoothstep(1.0 - normalThreshold, 1.0, normalDot);
  totalConfidence *= normalConfidence;
        
        // Motion-based confidence - fast motion reduces history weight
        float velLen = length(vel * vec2(sz));
        float motionConfidence = exp(-velLen * 0.03);
     totalConfidence *= motionConfidence;
    } else {
     totalConfidence = 0.0;
    }

    // === NEIGHBORHOOD CLAMPING ===
    // Clamp history to current frame's neighborhood to reduce ghosting
    
    vec3 minColor = vec3(1e9);
    vec3 maxColor = vec3(-1e9);
    vec3 moment1 = vec3(0.0);
    vec3 moment2 = vec3(0.0);
    float sampleCount = 0.0;
    
    // Sample 3x3 neighborhood of current frame
for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
    vec2 nUV = uv + vec2(i, j) / vec2(sz);
            nUV = clamp(nUV, vec2(0.0), vec2(1.0));
     vec3 c = texture(curSSGI, nUV).rgb;
            
        minColor = min(minColor, c);
            maxColor = max(maxColor, c);
    moment1 += c;
      moment2 += c * c;
          sampleCount += 1.0;
        }
    }
    
    // Calculate variance-based bounds (more robust than min/max)
    vec3 mean = moment1 / sampleCount;
    vec3 variance = (moment2 / sampleCount) - (mean * mean);
    vec3 stdDev = sqrt(max(variance, vec3(0.0)));
    
    // Use variance-based clamping with adjustable gamma
    float gamma = mix(1.0, 1.5, 1.0 - totalConfidence); // Tighter bounds when confidence is low
    vec3 boundMin = mean - gamma * stdDev;
    vec3 boundMax = mean + gamma * stdDev;
    
    // Clamp history to neighborhood bounds
    vec3 prevClamped = clamp(prev.rgb, boundMin, boundMax);

    // === TEMPORAL BLENDING ===
    // Exponential moving average with adaptive blending
    
    // Adapt blend factor based on confidence
    // Lower confidence = higher alpha = more current frame (less history)
    float adaptiveAlpha = mix(alpha, 0.8, 1.0 - totalConfidence);
    adaptiveAlpha = clamp(adaptiveAlpha, 0.05, 0.95);
    
    // Blend: result = mix(history, current, alpha)
    // Alpha=0.1 means 90% history, 10% current (stable, slow convergence)
    // Alpha=0.9 means 10% history, 90% current (responsive, more noise)
    vec3 result = mix(prevClamped, cur.rgb, adaptiveAlpha);
    
    // Blend validity masks
    float outMask = mix(prev.a, cur.a, adaptiveAlpha);
    
    // Store result
    imageStore(outSSGI, id, vec4(result, outMask));
}
