#version 450 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D hdrBuffer;
uniform float threshold;
uniform float knee;
uniform float bloomStrength;

void main()
{
    vec3 color = texture(hdrBuffer, TexCoord).rgb;
    float brightness = max(max(color.r, color.g), color.b);

    float soft = brightness - threshold;
    soft = clamp(soft / knee, 0.0, 1.0);
    soft = soft * soft * (3.0 - 2.0 * soft); // smoothstep

    color *= soft * bloomStrength;
    FragColor = vec4(color, 0.0);
}
