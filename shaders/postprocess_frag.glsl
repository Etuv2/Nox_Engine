#version 450 core
layout(location = 0) out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D hdrBuffer;   // scene color HDR
uniform sampler2D bloomBlur;   // final upsampled bloom
uniform float exposure;        // tone mapping exposure
uniform float gamma;           // gamma correction (used when not sRGB)

// New uniforms for tonemapping selection and parameters
uniform int uTonemap;          // 0=None, 1=ACES, 2=Uchimura (GT)
uniform float uP;
uniform float ua;
uniform float um;
uniform float ul;
uniform float uc;
uniform float ub;
uniform bool uOutputSRGB;      // if true, output gamma 2.2, else use provided gamma

// Uchimura (Gran Turismo Sport) tonemap function : https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf
vec3 TonemapGT(vec3 x, float P, float a, float m, float l, float c, float b) {
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;
    vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
    vec3 w2 = vec3(step(m + l0, x));
    vec3 w1 = vec3(1.0) - w0 - w2;
    vec3 T = m * pow(x / m, vec3(c)) + b;
    vec3 L = m + a * (x - m);
    vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
    return T * w0 + L * w1 + S * w2;
}
// ACES tonemap function : https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 TonemapACES(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main()
{
    vec2 vUV = TexCoord;

    vec3 hdr = texture(hdrBuffer, vUV).rgb * exposure;
    vec3 bloom = texture(bloomBlur, vUV).rgb;
    vec3 color = hdr + bloom;

    if (uTonemap == 2) {
        color = TonemapGT(color, uP, ua, um, ul, uc, ub);
    } else if (uTonemap == 1) {
        color = TonemapACES(color);
    }

    color = clamp(color, 0.0, 1.0);

    float outGamma = uOutputSRGB ? 2.2 : gamma;
    vec3 mapped = pow(color, vec3(1.0 / outGamma));

    FragColor = vec4(mapped, 1.0);
}
