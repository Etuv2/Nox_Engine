#version 460 core

in vec2 TexCoord;

uniform sampler2D currentFrame;     // Current frame after post-processing
uniform sampler2D historyFrame;     // Previous frame TAA result
uniform sampler2D velocityBuffer;   // Motion vectors
uniform sampler2D depthBuffer;      // Depth buffer for depth-based rejection
uniform sampler2D gNormal;          // Normal buffer for normal-based rejection

uniform float blendFactor;          // TAA blend factor
uniform float varianceThreshold;    // Variance threshold for motion detection
uniform float lumaWeight;          // Luminance weight for blending
uniform bool useYCoCg;             // Use YCoCg color space
uniform bool historyValid;         // Whether history buffer contains valid data
uniform vec2 screenSize;           // Screen dimensions
uniform vec2 jitter;               // Current frame jitter offset

// TAA quality parameters
uniform float depthThreshold = 0.001;      // Depth rejection threshold
uniform float normalThreshold = 0.1;       // Normal rejection threshold
uniform float edgeThreshold = 0.05;        // Edge detection threshold
uniform float reactiveMaskStrength = 0.8;  // Reactive mask strength

layout (location = 0) out vec3 taaResult;

// Constants for depth linearization
const float NEAR_PLANE = 0.1;
const float FAR_PLANE = 1000.0;

// RGB to HSV conversion
vec3 rgbToHsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB conversion
vec3 hsvToRgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// Convert RGB to YCoCg color space (more perceptually uniform)
vec3 RGBToYCoCg(vec3 rgb) {
    float Y = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    float Co = 0.5 * rgb.r - 0.5 * rgb.b;
    float Cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return vec3(Y, Co, Cg);
}

// Convert YCoCg to RGB color space
vec3 YCoCgToRGB(vec3 ycocg) {
    float Y = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    
    float r = Y + Co - Cg;
    float g = Y + Cg;
    float b = Y - Co - Cg;
    
    return vec3(r, g, b);
}

// Decode normal from G-buffer (octahedral encoding)
vec3 DecodeNormal(vec2 encoded) {
    encoded = encoded * 2.0 - 1.0;
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (n.z < 0.0) {
        vec2 signNotZero = vec2(sign(encoded.x), sign(encoded.y));
        n.xy = (1.0 - abs(n.yx)) * signNotZero;
    }
    return normalize(n);
}

// Linearize depth value
float LinearizeDepth(float depth) {
    return (2.0 * NEAR_PLANE) / (FAR_PLANE + NEAR_PLANE - depth * (FAR_PLANE - NEAR_PLANE));
}

// High-quality bicubic sampling for history buffer
vec3 SampleBicubic(sampler2D tex, vec2 uv) {
    vec2 texSize = textureSize(tex, 0);
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    
    // Catmull-Rom weights
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / (w1 + w2);
    
    vec2 texPos0 = texPos1 - vec2(1.0);
    vec2 texPos3 = texPos1 + vec2(2.0);
    vec2 texPos12 = texPos1 + offset12;
    
    texPos0 /= texSize;
    texPos3 /= texSize;
    texPos12 /= texSize;
    
    vec3 result = vec3(0.0);
    result += texture(tex, vec2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += texture(tex, vec2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += texture(tex, vec2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    
    result += texture(tex, vec2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += texture(tex, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += texture(tex, vec2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    
    result += texture(tex, vec2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += texture(tex, vec2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += texture(tex, vec2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    
    return max(vec3(0.0), result);
}

// Enhanced neighborhood analysis with proper statistics
void AnalyzeNeighborhood(vec2 uv, out vec3 minColor, out vec3 maxColor, out vec3 avgColor, 
                     out vec3 variance, out float edgeMask) {
    vec3 samples[9];
    int index = 0;
    
    // Sample 3x3 neighborhood
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 sampleUV = uv + vec2(float(x), float(y)) / screenSize;
       sampleUV = clamp(sampleUV, vec2(0.0), vec2(1.0)); // Ensure valid coordinates
   samples[index] = texture(currentFrame, sampleUV).rgb;
         if (useYCoCg) {
      samples[index] = RGBToYCoCg(samples[index]);
            }
            index++;
      }
    }
    
    // Calculate basic statistics
    minColor = samples[0];
    maxColor = samples[0];
    avgColor = samples[0];
    
    for (int i = 1; i < 9; i++) {
        minColor = min(minColor, samples[i]);
    maxColor = max(maxColor, samples[i]);
    avgColor += samples[i];
  }
    avgColor /= 9.0;
    
    // Calculate variance for proper statistical bounds
    variance = vec3(0.0);
    for (int i = 0; i < 9; i++) {
   vec3 diff = samples[i] - avgColor;
        variance += diff * diff;
    }
    variance /= 9.0;
    
    // Edge detection based on luminance variance
    float luminanceVariance = useYCoCg ? variance.x : dot(variance, vec3(0.299, 0.587, 0.114));
    edgeMask = clamp(sqrt(luminanceVariance) / edgeThreshold, 0.0, 1.0);
}

// Improved neighborhood clamping with high-luminance handling
vec3 ClampToNeighborhood(vec3 historyColor, vec3 minColor, vec3 maxColor, vec3 avgColor, 
          vec3 variance) {
    // Use statistical bounds instead of hard min/max
    vec3 sigma = sqrt(max(variance, vec3(0.0001))); // Avoid zero variance
    
    // Detect high-luminance regions (bright colors that may flicker)
    float avgLuma = useYCoCg ? avgColor.x : dot(avgColor, vec3(0.299, 0.587, 0.114));
    float histLuma = useYCoCg ? historyColor.x : dot(historyColor, vec3(0.299, 0.587, 0.114));
    
    // For bright regions, use tighter clamping to prevent flickering
    // For normal regions, use standard gamma
    float gamma = 1.25; // Standard gamma for temporal stability
    
    // If average luminance is high (>0.8) or history is bright, reduce gamma
    if (avgLuma > 0.8 || histLuma > 0.8) {
        gamma = 1.0; // Tighter bounds for bright regions
    }
  
    vec3 boundMin = avgColor - gamma * sigma;
    vec3 boundMax = avgColor + gamma * sigma;
    
    // Standard statistical clamping
    return clamp(historyColor, boundMin, boundMax);
}

// Calculate depth-based rejection with proper linearization
float CalculateDepthRejection(vec2 uv, vec2 historyUV) {
    float currentDepth = texture(depthBuffer, uv).r;
    float historyDepth = texture(depthBuffer, historyUV).r;
    
    float currentLinear = LinearizeDepth(currentDepth);
    float historyLinear = LinearizeDepth(historyDepth);
    
    float depthDiff = abs(currentLinear - historyLinear);
    return clamp(1.0 - depthDiff / depthThreshold, 0.0, 1.0);
}

// Calculate normal-based rejection
float CalculateNormalRejection(vec2 uv, vec2 historyUV) {
    vec3 currentNormal = DecodeNormal(texture(gNormal, uv).rg);
    vec3 historyNormal = DecodeNormal(texture(gNormal, historyUV).rg);
    
    float normalSimilarity = dot(currentNormal, historyNormal);
    return clamp(normalSimilarity * normalSimilarity, 0.0, 1.0); // Square for sharper falloff
}

// Enhanced reactive mask with proper motion and edge detection
float CalculateReactiveMask(vec2 uv, vec2 velocity, float edgeMask) {
    // Motion-based reactive mask
    float motionLength = length(velocity * screenSize);
    float motionMask = clamp(motionLength * 0.03, 0.0, 1.0); // Reduced sensitivity
    
    // Combine motion and edge information
    float combinedMask = max(motionMask, edgeMask * 0.5);
    
    return combinedMask * reactiveMaskStrength;
}

// Enhanced color-aware temporal blending with high-luminance handling
vec3 ColorAwareBlend(vec3 currentColor, vec3 historyColor, float blendWeight) {
    if (useYCoCg) {
        // YCoCg space naturally separates luminance from chrominance
        vec3 currentYCoCg = currentColor;
        vec3 historyYCoCg = historyColor;
     
   // Separate treatment for luminance and chrominance
        float currentLuma = currentYCoCg.x;
    float historyLuma = historyYCoCg.x;
     vec2 currentChroma = currentYCoCg.yz;
     vec2 historyChroma = historyYCoCg.yz;
      
        // Detect high-luminance regions (bright colors like SSGI/bloom)
      bool isBright = currentLuma > 0.8 || historyLuma > 0.8;
        
   // For bright regions, use more aggressive blending to prevent flickering
     float lumaBlendWeight = isBright ? min(blendWeight * 1.5, 0.85) : blendWeight;
     float blendedLuma = mix(historyLuma, currentLuma, lumaBlendWeight);
        
        // Blend chrominance conservatively to preserve color accuracy
        // For bright regions, blend chrominance more aggressively to prevent color shifts
        float chromaBlendWeight = isBright ? min(blendWeight * 2.0, 0.9) : min(blendWeight * 1.5, 0.8);
        vec2 blendedChroma = mix(historyChroma, currentChroma, chromaBlendWeight);
        
      return vec3(blendedLuma, blendedChroma);
    } else {
        // RGB space - convert to HSV for hue preservation
        vec3 currentHSV = rgbToHsv(currentColor);
  vec3 historyHSV = rgbToHsv(historyColor);
   
        // Detect high-luminance (value in HSV)
        bool isBright = currentHSV.z > 0.8 || historyHSV.z > 0.8;
        
        // Blend HSV components separately for better color preservation
        // For bright regions, use more aggressive blending
        float hueBlendWeight = isBright ? min(blendWeight * 1.2, 0.8) : blendWeight * 0.7;
        float satBlendWeight = isBright ? min(blendWeight * 1.3, 0.85) : blendWeight;
        float valBlendWeight = isBright ? min(blendWeight * 1.5, 0.9) : blendWeight;
        
        vec3 blendedHSV = vec3(
   mix(historyHSV.x, currentHSV.x, hueBlendWeight),
    mix(historyHSV.y, currentHSV.y, satBlendWeight),
  mix(historyHSV.z, currentHSV.z, valBlendWeight)
        );
 
        return hsvToRgb(blendedHSV);
    }
}

void main()
{
    vec2 uv = TexCoord;
    
    // Sample current frame
    vec3 currentColor = texture(currentFrame, uv).rgb;
    
    if (!historyValid) {
        taaResult = currentColor;
        return;
    }
    
    // Sample motion vector with proper bounds checking
    vec2 velocity = texture(velocityBuffer, uv).rg;
    vec2 historyUV = uv - velocity;
    
    // Strict bounds checking for history sampling
    if (any(lessThan(historyUV, vec2(0.001))) || any(greaterThan(historyUV, vec2(0.999)))) {
        taaResult = currentColor;
        return;
    }
    
    // High-quality history sampling
    vec3 historyColor = SampleBicubic(historyFrame, historyUV);
    
    // Convert to working color space
    if (useYCoCg) {
        currentColor = RGBToYCoCg(currentColor);
        historyColor = RGBToYCoCg(historyColor);
    }
    
    // Analyze neighborhood with proper statistics
    vec3 minColor, maxColor, avgColor, variance;
    float edgeMask;
    AnalyzeNeighborhood(uv, minColor, maxColor, avgColor, variance, edgeMask);
    
    // Apply standard neighborhood clamping (no bloom considerations)
    vec3 clampedHistory = ClampToNeighborhood(historyColor, minColor, maxColor, avgColor, variance);
    
    // Calculate rejection factors
    float depthConfidence = CalculateDepthRejection(uv, historyUV);
    float normalConfidence = CalculateNormalRejection(uv, historyUV);
    
    // Motion-based confidence with better stability
    float velocityLength = length(velocity * screenSize);
    float motionConfidence = exp(-velocityLength * 0.02); // Reduced sensitivity for stability
    
    // Strong rejection for invalid reprojections
    if (velocityLength > 2.0 || depthConfidence < 0.3 || normalConfidence < 0.7) {
        // Reject history completely for major discontinuities
        taaResult = currentColor;
        return;
    }
    
    // Calculate reactive mask
    float reactiveMask = CalculateReactiveMask(uv, velocity, edgeMask);
    
    // Combine confidence factors
    float totalConfidence = depthConfidence * normalConfidence * motionConfidence * (1.0 - reactiveMask);
    
    // Adaptive blend factor with proper bounds
    float baseBlendFactor = mix(0.95, blendFactor, totalConfidence);
    baseBlendFactor = mix(baseBlendFactor, 0.9, edgeMask); // Higher blend for edges
    
    // Final blend factor clamping
    baseBlendFactor = clamp(baseBlendFactor, 0.05, 0.95);
    
    // Perform enhanced color-aware temporal blending
    vec3 result = ColorAwareBlend(currentColor, clampedHistory, baseBlendFactor);
    
 // Enhanced anti-flickering for high-frequency details
    float currentLuma = useYCoCg ? currentColor.x : dot(currentColor, vec3(0.299, 0.587, 0.114));
    bool isBrightRegion = currentLuma > 0.8;
    
    if (velocityLength < 0.5 && (edgeMask > 0.3 || isBrightRegion)) {
        // For static high-frequency OR bright content, use more conservative blending
    float stabilityFactor = isBrightRegion ? (1.0 - edgeMask * 0.5) : (1.0 - edgeMask * 0.3);
        
      // For bright regions, favor current frame more to prevent lag
        float brightBias = isBrightRegion ? 0.3 : 0.2;
    result = mix(result, mix(clampedHistory, currentColor, brightBias), stabilityFactor);
    }
    
    // Convert back to RGB if needed
    if (useYCoCg) {
        result = YCoCgToRGB(result);
    }
    
    // Ensure valid output
    taaResult = max(vec3(0.0), result);
}