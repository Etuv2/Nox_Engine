#version 460 core

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uToneMap = 0;

vec3 DebugTonemap(vec3 value)
{
    vec3 safeValue = max(value, vec3(0.0));
    vec3 mapped = safeValue / (safeValue + vec3(1.0));
    return pow(clamp(mapped, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
}

void main() {
    vec4 sampleValue = texture(uTexture, TexCoord);
    vec3 color = uToneMap != 0 ? DebugTonemap(sampleValue.rgb) : sampleValue.rgb;
    FragColor = vec4(color, sampleValue.a);
}
