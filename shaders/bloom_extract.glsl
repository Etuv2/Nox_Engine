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

void main()
{
    // Threshold in exposed units so bloom follows exposure (pre-exposed physical scenes and legacy
    // scenes alike). Strength is applied in post-processing.
    vec3 color = texture(hdrBuffer, TexCoord).rgb * uExposure * NoxExposureRatio();
    if (uBloomMode == 1) {
        FragColor = vec4(clamp(color, 0.0, uMaxValue), 1.0);
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
