#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D image;
uniform vec2 texelSize;
uniform int pass;

void main()
{
    // Improved Kawase blur with proper offset and weights
    // Each pass increases the blur radius progressively
    float offset = 0.5 + float(pass) * 0.5;
    vec2 pixelOffset = texelSize * offset;

    vec3 result = vec3(0.0);
    
    // 4-tap box filter with proper bilinear sampling
    // Sample at half-pixel offsets for better quality
    result += texture(image, TexCoord + vec2( pixelOffset.x,  pixelOffset.y)).rgb;
    result += texture(image, TexCoord + vec2(-pixelOffset.x,  pixelOffset.y)).rgb;
    result += texture(image, TexCoord + vec2( pixelOffset.x, -pixelOffset.y)).rgb;
    result += texture(image, TexCoord + vec2(-pixelOffset.x, -pixelOffset.y)).rgb;

    // Also sample center for better blur quality
    result += texture(image, TexCoord).rgb * 2.0;

    // Proper normalization (4 corner samples + 2x center)
    FragColor = vec4(result / 6.0, 1.0);
}
