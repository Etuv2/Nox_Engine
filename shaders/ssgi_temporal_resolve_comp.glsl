#version 460 core

// Temporal accumulation for SSGI stability - FIXED with proper reprojection
// Implements exponential moving average with robust rejection heuristics

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures - ALL AT WORKING RESOLUTION (half-res by default)
layout (binding = 0) uniform sampler2D curSSGI; // Current frame (denoised, rgba with a=mask) - WORKING RES
layout (binding = 1) uniform sampler2D prevSSGI;    // Previous frame SSGI - WORKING RES
layout (binding = 2) uniform sampler2D velocityTex; // Motion vectors in UV space - FULL RES
layout (binding = 3) uniform sampler2D depthTex;    // Current depth (for rejection) - FULL RES
layout (binding = 4) uniform sampler2D normalTex;   // Current normals (oct encoded RG) - FULL RES

// Output - WORKING RESOLUTION
layout (binding = 5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform float alpha; // Base blend factor (0..1) - SMALL values (0.1-0.2) favor history
uniform float depthThreshold; // Depth rejection threshold
uniform float normalThreshold; // Normal rejection threshold

// CRITICAL FIX: Use exact same octahedral decode as SSAO
vec3 octDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 s = vec2(sign(e.x), sign(e.y));
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

// Luminance calculation
float luma(vec3 c) { 
    return dot(c, vec3(0.2126, 0.7152, 0.0722)); 
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(outSSGI);
    if (any(greaterThanEqual(id, sz))) return;
    
    // CRITICAL FIX: Compute UVs for working and screen resolution
    vec2 uvWork = (vec2(id) + 0.5) / vec2(sz);
    vec2 uvScreen = uvWork; // Normalized UVs map directly

    // Sample current SSGI from WORKING RESOLUTION
    vec4 current = texture(curSSGI, uvWork);
    
    // CRITICAL FIX: Sample motion vector from FULL RESOLUTION
    vec2 velocity = texture(velocityTex, uvScreen).rg;
    
    // Reproject to previous frame position
    vec2 prevUV = uvWork - velocity;

    // Screen boundary check with margin
    bool historyValid = all(greaterThanEqual(prevUV, vec2(0.005))) && 
     all(lessThan(prevUV, vec2(0.995)));
    
    // Sample previous SSGI at reprojected location - WORKING RESOLUTION
    vec4 history = historyValid ? texture(prevSSGI, prevUV) : vec4(0.0);

    // ========================================================================
    // CRITICAL FIX: Proper rejection tests using depth and normal
    // ========================================================================
    
    float confidence = 1.0;
    
    if (historyValid) {
        // Sample current pixel depth/normal from FULL RESOLUTION
        float currDepth = texture(depthTex, uvScreen).r;
        vec3 currNormal = octDecode(texture(normalTex, uvScreen).rg);
 
  // Sample reprojected pixel depth/normal from FULL RESOLUTION
     vec2 prevScreenUV = prevUV; // Maps to screen space
        float prevDepth = texture(depthTex, prevScreenUV).r;
      vec3 prevNormal = octDecode(texture(normalTex, prevScreenUV).rg);
        
        // CRITICAL: Depth-based disocclusion detection (exponential falloff)
    float depthDiff = abs(currDepth - prevDepth);
        float depthConfidence = exp(-depthDiff / depthThreshold);
   confidence *= depthConfidence;
    
        // CRITICAL: Normal-based surface orientation change detection
        float normalDot = dot(currNormal, prevNormal);
      float normalConfidence = exp(-max(0.0, 1.0 - normalDot) / normalThreshold);

  // Hard reject for sharp edge transitions
        if (normalDot < (1.0 - normalThreshold * 3.0)) {
            normalConfidence = 0.0;
        }
     confidence *= normalConfidence;
   
        // Motion-based confidence (fast motion reduces history weight)
        float velLen = length(velocity * vec2(sz));
      float motionConfidence = exp(-velLen * 0.04);
   confidence *= motionConfidence;
    } else {
        confidence = 0.0; // No valid history
    }

    // ========================================================================
    // CRITICAL FIX: Neighborhood clamping to prevent ghosting
    // ========================================================================
    
    vec3 minColor = vec3(1e9);
    vec3 maxColor = vec3(-1e9);
    
    // 3x3 neighborhood analysis
for (int j = -1; j <= 1; ++j) {
    for (int i = -1; i <= 1; ++i) {
    vec2 nUV = uvWork + vec2(i, j) / vec2(sz);
            nUV = clamp(nUV, vec2(0.0), vec2(1.0));
  vec3 c = texture(curSSGI, nUV).rgb;
          
  minColor = min(minColor, c);
 maxColor = max(maxColor, c);
    }
    }
    
    // Clamp history to neighborhood bounds (prevents ghosting)
    vec3 historyClamped = clamp(history.rgb, minColor, maxColor);
    
    // ========================================================================
    // CRITICAL FIX: Adaptive alpha based on confidence
    // ========================================================================
    
    float adaptiveAlpha = alpha;
    
    // Low confidence = high alpha (favor current frame)
    adaptiveAlpha = mix(0.9, adaptiveAlpha, confidence);
    
 // Luminance change detection
    float lumaCur = luma(current.rgb);
    float lumaHist = luma(historyClamped);
    float lumaChange = abs(lumaCur - lumaHist) / max(lumaCur, 0.001);
    
    // Large luminance change = favor current frame
    adaptiveAlpha = mix(adaptiveAlpha, 0.8, clamp(lumaChange * 5.0, 0.0, 1.0));
    
 // Clamp final alpha to safe range
    adaptiveAlpha = clamp(adaptiveAlpha, 0.05, 0.95);
    
    // ========================================================================
    // CRITICAL FIX: Exponential moving average (like TAA)
    // ========================================================================
    
    // result = mix(history, current, alpha)
    // Small alpha = more history (stable, slower convergence)
    // Large alpha = more current (responsive, faster convergence)
    vec3 result = mix(historyClamped, current.rgb, adaptiveAlpha);
    
    // Blend validity masks
    float outMask = mix(history.a, current.a, adaptiveAlpha);
    
    // Final validation: ensure output is non-negative and finite
    result = max(result, vec3(0.0));
    if (any(isnan(result)) || any(isinf(result))) {
     result = current.rgb; // Fallback to current frame if history corrupted
    }
    
    outMask = clamp(outMask, 0.0, 1.0);
  
    // Store result
    imageStore(outSSGI, id, vec4(result, outMask));
}
