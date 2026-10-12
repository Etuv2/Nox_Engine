#version 460 core
#include "includes/exposure_common.glsl"

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D hdrBuffer;
uniform float threshold;
uniform float knee;
uniform float uExposure;  // scene/user exposure multiplier (post applies the same value)
uniform int uBloomMode;   // 0 thresholded (additive in post), 1 full image (energy-conserving mix)
uniform float uMaxValue;  // firefly clamp in exposed units
// Half-resolution extract: source texel size of the full-resolution input (0 = 1:1, single tap).
uniform vec2 uSourceTexel = vec2(0.0);
// Energy-conserving mode: exposed luminance below which light is not scattered (0 = whole image).
uniform float uScatterThreshold = 0.0;

// 13-tap 2:1 downsample (Jimenez 2014): stable under sub-pixel motion of small bright sources.
vec3 Downsample13(vec2 uv, vec2 t) {
    vec3 a = texture(hdrBuffer, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 b = texture(hdrBuffer, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 c = texture(hdrBuffer, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 d = texture(hdrBuffer, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 e = texture(hdrBuffer, uv).rgb;
    vec3 f = texture(hdrBuffer, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 g = texture(hdrBuffer, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 h = texture(hdrBuffer, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 i = texture(hdrBuffer, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 j = texture(hdrBuffer, uv + t * vec2( 1.0,  1.0)).rgb;
    vec3 k = texture(hdrBuffer, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 l = texture(hdrBuffer, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 m = texture(hdrBuffer, uv + t * vec2( 2.0,  2.0)).rgb;
    return (d + f + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + g + h + l) * 0.0625 + e * 0.125;
}

// Fraction of a pixel's light that scatters (soft knee of half the threshold).
float ScatterWeight(vec3 c) {
    if (uScatterThreshold <= 0.0) return 1.0;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = 0.5 * uScatterThreshold;
    float soft = clamp(l - uScatterThreshold + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 1e-5);
    return max(soft, l - uScatterThreshold) / max(l, 1e-5);
}

void main()
{
    // Threshold in exposed units so bloom follows exposure (pre-exposed physical scenes and legacy
    // scenes alike). Strength is applied in post-processing.
    vec3 src = uSourceTexel.x > 0.0 ? Downsample13(TexCoord, uSourceTexel) : texture(hdrBuffer, TexCoord).rgb;
    vec3 color = src * uExposure * NoxExposureRatio();
    if (uBloomMode == 1) {
        FragColor = vec4(clamp(color * ScatterWeight(color), 0.0, uMaxValue), 1.0);
        return;
    }
    
    // luminance calculation 
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Improved soft threshold with quadratic knee
    float softThreshold = threshold - knee;
    float hardThreshold = threshold + knee;
    
    // Compute weight based on luminance
    float weight = 0.0;
    
    if (luma < softThreshold) {
        weight = 0.0;
    } else if (luma > hardThreshold) {
        weight = 1.0;
    } else {
        // Smooth quadratic curve in the knee region
        float range = hardThreshold - softThreshold;
        float t = (luma - softThreshold) / max(range, 1e-5);
        weight = t * t * (3.0 - 2.0 * t); // smoothstep
    }

    vec3 bloom = clamp(color * weight, 0.0, uMaxValue);
    
    FragColor = vec4(bloom, 1.0);
}
