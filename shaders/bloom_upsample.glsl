#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D lowResTex;
uniform sampler2D highResTex;

void main()
{
    vec3 low = texture(lowResTex, TexCoord).rgb;
    vec3 high = texture(highResTex, TexCoord).rgb;
    FragColor = vec4(low + high, 1.0);
}
