#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D hdrBuffer;
uniform float threshold;
uniform float knee;
uniform float bloomStrength;

void main()
{
    vec3 color = texture(hdrBuffer, TexCoord).rgb;
    
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

    // Apply weight and strength
    vec3 bloom = color * weight * bloomStrength;
    
    // Clamp to prevent extreme values
    bloom = clamp(bloom, 0.0, 64.0);
    
    FragColor = vec4(bloom, 1.0);
}
