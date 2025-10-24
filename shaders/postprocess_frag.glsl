#version 450 core
layout(location = 0) out vec4 FragColor;

in vec2 vTexCoord;

uniform sampler2D hdrBuffer;   // scene color HDR
uniform sampler2D bloomBlur;   // final upsampled bloom
uniform float exposure;        // tone mapping
uniform float gamma;           // gamma correction

//Aces tonemap function
vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

void main()
{
    vec3 hdrColor = texture(hdrBuffer, vTexCoord).rgb;
    vec3 bloomColor = texture(bloomBlur, vTexCoord).rgb;

    hdrColor += bloomColor;
    hdrColor *= exposure; // Apply exposure after bloom
    hdrColor = ACESFilm(hdrColor);
    hdrColor = pow(hdrColor, vec3(1.0 / gamma));

    FragColor = vec4(hdrColor, 1.0);
}
