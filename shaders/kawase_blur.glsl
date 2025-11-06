#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D image;
uniform vec2 texelSize;
uniform int pass;

void main()
{
    vec2 offset = texelSize * (1.0 + pass * 0.5);

    vec3 result = vec3(0.0);
    result += texture(image, TexCoord + vec2( offset.x,  offset.y)).rgb;
    result += texture(image, TexCoord + vec2(-offset.x,  offset.y)).rgb;
    result += texture(image, TexCoord + vec2( offset.x, -offset.y)).rgb;
    result += texture(image, TexCoord + vec2(-offset.x, -offset.y)).rgb;

    FragColor = vec4(result * 0.25, 1.0);
}
