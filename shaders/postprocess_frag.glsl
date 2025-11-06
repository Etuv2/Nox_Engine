#version 460 core
layout(location = 0) out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D hdrBuffer;   // scene color HDR
uniform sampler2D bloomBlur;   // final upsampled bloom
uniform float exposure;        // tone mapping exposure
uniform float gamma;           // gamma correction (used when not sRGB)

// New uniforms for tonemapping selection and parameters
uniform int uTonemap;          // 0=None, 1=ACES, 2=Uchimura (GT), 3=GT7
uniform float uP;
uniform float ua;
uniform float um;
uniform float ul;
uniform float uc;
uniform float ub;
uniform bool uOutputSRGB;      // if true, output gamma 2.2, else use provided gamma

// GT7 parameters
uniform float uTm7PeakNits;    // display peak luminance
uniform float uTm7Blend;
uniform float uTm7FadeStart;
uniform float uTm7FadeEnd;
uniform bool  uTm7UseJzazbz;   // switch UCS (currently unused)

//GT Sport tonemap function : https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf
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
// GT7 Tonemap with gentle chroma compression and SDR correction: https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf
vec3 TonemapGT7(vec3 color, float peakNits, float blend, float fadeStart, float fadeEnd)
{
    // Convert HDR linear scene (1.0 = 100 nits) to normalized luminance
    float referenceLuminance = 100.0;
    float paperWhite = 250.0;
    float sdrCorrection = referenceLuminance / paperWhite;

    float peak = peakNits / referenceLuminance;

    // GTToneMappingCurveV2 approx
    float a = 0.25;
    float m = 0.538;
    float l = 0.444;
    float t = 1.280;
    float k = (l - 1.0) / (a - 1.0);
    float kA = peak * l + peak * k;
    float kB = -peak * k * exp(l / k);
    float kC = -1.0 / (k * peak);

    vec3 result;
    for (int i = 0; i < 3; ++i)
    {
        float x = color[i];
        float weightLinear = smoothstep(0.0, m, x);
        float weightToe = 1.0 - weightLinear;
        float shoulder = kA + kB * exp(x * kC);
        if (x < l * peak)
        {
            float toeMapped = m * pow(x / m, t);
            result[i] = mix(toeMapped, x, weightLinear);
        }
        else
        {
            result[i] = shoulder;
        }
    }

    // Gentle chroma compression (approx fade)
    float luminance = dot(result, vec3(0.2627, 0.6780, 0.0593));
    float fade = 1.0 - smoothstep(fadeStart, fadeEnd, luminance / peak);
    result = mix(result, vec3(luminance), blend) * fade;

    return result * sdrCorrection;
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

    if (uTonemap == 3) {
        color = TonemapGT7(color, uTm7PeakNits, uTm7Blend, uTm7FadeStart, uTm7FadeEnd);
    } else if (uTonemap == 2) {
        color = TonemapGT(color, uP, ua, um, ul, uc, ub);
    } else if (uTonemap == 1) {
        color = TonemapACES(color);
    }

    color = clamp(color, 0.0, 1.0);

    float outGamma = uOutputSRGB ? 2.2 : gamma;
    vec3 mapped = pow(color, vec3(1.0 / outGamma));

    FragColor = vec4(mapped, 1.0);
}
