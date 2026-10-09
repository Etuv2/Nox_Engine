#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D lowResTex;
uniform sampler2D highResTex;
uniform bool uNormalize; // energy-conserving bloom keeps the chain a weighted average

// Implement proper tent filter (9-tap) for smooth upsampling
vec3 UpsampleTent9(sampler2D tex, vec2 uv, vec2 texelSize) {
    // Tent filter weights:
    // 1  2  1
    // 2  4  2
    // 1  2  1
    // Total weight = 16
    
    vec3 result = vec3(0.0);
    
    // Center (weight 4)
    result += texture(tex, uv).rgb * 4.0;
    
    // Cardinals (weight 2 each)
    result += texture(tex, uv + vec2( texelSize.x,          0.0)).rgb * 2.0;
    result += texture(tex, uv + vec2(-texelSize.x,          0.0)).rgb * 2.0;
    result += texture(tex, uv + vec2(         0.0,  texelSize.y)).rgb * 2.0;
    result += texture(tex, uv + vec2(         0.0, -texelSize.y)).rgb * 2.0;
    
    // Diagonals (weight 1 each)
    result += texture(tex, uv + vec2( texelSize.x,  texelSize.y)).rgb;
    result += texture(tex, uv + vec2(-texelSize.x,  texelSize.y)).rgb;
    result += texture(tex, uv + vec2( texelSize.x, -texelSize.y)).rgb;
    result += texture(tex, uv + vec2(-texelSize.x, -texelSize.y)).rgb;
    
    return result / 16.0;
}

void main()
{
    // Get texel size for the low-res texture
    vec2 lowResTexelSize = 1.0 / textureSize(lowResTex, 0);
    
    // tent filter for smooth upsampling
    vec3 low = UpsampleTent9(lowResTex, TexCoord, lowResTexelSize);
    
    // High-res contribution (detail preservation)
    vec3 high = texture(highResTex, TexCoord).rgb;
    
    // Additive blend with slight bias toward upsampled low-res
    // This creates smooth, continuous bloom
    vec3 sum = low * 1.0 + high * 0.85;
    FragColor = vec4(uNormalize ? sum / 1.85 : sum, 1.0);
}
