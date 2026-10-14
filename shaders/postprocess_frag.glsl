#version 460 core
#include "includes/exposure_common.glsl"
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

// Image formation (docs/nfs2015/README.md M3). Order of operations:
//   exposure -> bloom -> chromatic aberration (sampling) -> vignette -> white balance ->
//   contrast / saturation / split tone (scene-linear) -> tonemap -> lift/gamma/gain (display) ->
//   output transfer (sRGB OETF) -> film grain -> dither.
uniform int   uBloomMode;            // 0 additive (legacy), 1 energy-conserving mix
uniform float uBloomStrength;
uniform bool  uHasBloom;
uniform float uBloomScatterThreshold = 0.0; // energy-conserving mode: see bloom_extract.glsl
uniform mat3  uWhiteBalance;         // linear Rec.709 chromatic adaptation (identity = off)
uniform float uContrast;
uniform float uSaturation;
uniform vec3  uShadowTint;
uniform vec3  uHighlightTint;
uniform float uSplitToneStrength;
uniform vec3  uLift;
uniform vec3  uGammaAdjust;
uniform vec3  uGain;
uniform float uVignette;
uniform float uFilmGrain;
uniform float uChromaticAberration;  // pixels at the image corner
uniform bool  uDither;
uniform uint  uFrameIndex;
uniform vec2  uResolution;
// Inspection of intermediate images (RenderContext::postDebugView); 0 = normal output.
uniform int   uDebugView = 0;
uniform sampler2D uDebugTex;
uniform sampler2D uDebugTex2;

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

// sRGB OETF (IEC 61966-2-1), piecewise; replaces the former pure 2.2 power curve.
vec3 LinearToSRGB(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}

float Hash12(vec2 p, float seed) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031 + seed * 0.1234);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main()
{
    vec2 vUV = TexCoord;
    // Physical camera: the image is pre-exposed; apply the ratio to this frame's (possibly
    // auto-adapted) exposure. Legacy scenes: ratio is 1 and `exposure` is the scene value.
    float exposureScale = exposure * NoxExposureRatio();

    vec3 hdr;
    if (uChromaticAberration > 0.0) {
        vec2 fromCenter = vUV - 0.5;
        vec2 offset = fromCenter * (2.0 * uChromaticAberration / max(uResolution.x, 1.0));
        hdr.r = texture(hdrBuffer, vUV - offset).r;
        hdr.g = texture(hdrBuffer, vUV).g;
        hdr.b = texture(hdrBuffer, vUV + offset).b;
    } else {
        hdr = texture(hdrBuffer, vUV).rgb;
    }
    bool hasBloom = uHasBloom;
    bool grain = uFilmGrain > 0.0;
    bool dither = uDither;
    if (uDebugView != 0) {
        hasBloom = false;
        if (uDebugView < 0) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        if (uDebugView == 5) {
            vec2 v = texture(uDebugTex, vUV).rg * uResolution; // pixels per frame
            // Signed, 0.5 = still, +-32 px per frame across the range (readable from 8-bit captures).
            FragColor = vec4(clamp(0.5 + v / 64.0, 0.0, 1.0), length(v) > 0.25 ? 0.25 : 0.0, 1.0);
            return;
        }
        hdr = texture(uDebugTex, vUV).rgb;
        if (uDebugView == 6) hdr = abs(hdr - texture(uDebugTex2, vUV).rgb) * 8.0;
        if (uDebugView == 8) hdr = vec3(texture(uDebugTex, vUV).r);   // ambient occlusion (red = visibility)
        grain = uDebugView != 7 && uDebugView != 8 && grain;
        dither = uDebugView != 7 && uDebugView != 8 && dither;
    }
    vec3 color = hdr * exposureScale;

    // The bloom chain is extracted from exposed values (bloom_extract.glsl), so it is in the
    // same units as `color` here.
    if (hasBloom) {
        vec3 bloom = texture(bloomBlur, vUV).rgb;
        if (uBloomMode == 1) {
            // Energy-conserving scatter: the fraction s of the scattering light leaves the pixel and
            // returns spread out (bloom). With no threshold this is mix(color, bloom, s).
            float s = clamp(uBloomStrength, 0.0, 1.0);
            float scatter = 1.0;
            if (uBloomScatterThreshold > 0.0) {
                float l = Luma(color);
                float k = 0.5 * uBloomScatterThreshold;
                float soft = clamp(l - uBloomScatterThreshold + k, 0.0, 2.0 * k);
                soft = soft * soft / (4.0 * k + 1e-5);
                scatter = max(soft, l - uBloomScatterThreshold) / max(l, 1e-5);
            }
            color = color + s * (bloom - color * scatter);
        } else {
            color = color + bloom * uBloomStrength;
        }
    }

    // Natural lens falloff (approximately cos^4), aspect corrected.
    if (uVignette > 0.0) {
        vec2 d = (vUV - 0.5) * vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
        float r2 = dot(d, d) * uVignette * 2.0;
        color *= 1.0 / ((1.0 + r2) * (1.0 + r2));
    }

    color = max(uWhiteBalance * color, vec3(0.0));

    // Scene-linear grade around middle grey.
    const float midGrey = 0.18;
    if (uContrast != 1.0) {
        color = midGrey * pow(max(color, vec3(1e-6)) / midGrey, vec3(uContrast));
    }
    float lum = Luma(color);
    color = max(vec3(lum) + (color - vec3(lum)) * uSaturation, vec3(0.0));
    if (uSplitToneStrength > 0.0) {
        float logLum = log2(max(lum, 1e-6) / midGrey);
        float shadowW = 1.0 - smoothstep(-4.0, 0.5, logLum);
        float highlightW = smoothstep(-0.5, 3.0, logLum);
        color *= vec3(1.0) + (uShadowTint * shadowW + uHighlightTint * highlightW) * uSplitToneStrength;
    }

    if (uTonemap == 3) {
        color = TonemapGT7(color, uTm7PeakNits, uTm7Blend, uTm7FadeStart, uTm7FadeEnd);
    } else if (uTonemap == 2) {
        color = TonemapGT(color, uP, ua, um, ul, uc, ub);
    } else if (uTonemap == 1) {
        color = TonemapACES(color);
    }
    color = clamp(color, 0.0, 1.0);

    // Display-referred lift / gamma / gain.
    color = clamp(uGain * (color + uLift * (vec3(1.0) - color)), 0.0, 1.0);
    color = pow(color, vec3(1.0) / max(uGammaAdjust, vec3(0.01)));

    vec3 mapped = uOutputSRGB ? LinearToSRGB(color) : pow(color, vec3(1.0 / gamma));

    // Film grain in the display encoding: added before the OETF, the curve's steep toe amplified it
    // several times in near-black areas (night sky), the opposite of the intended midtone response.
    if (grain) {
        float n = Hash12(gl_FragCoord.xy, float(uFrameIndex % 64u)) - 0.5;
        float response = 1.0 - abs(Luma(mapped) * 2.0 - 1.0); // strongest in midtones
        mapped = clamp(mapped + n * uFilmGrain * (0.35 + 0.65 * response), 0.0, 1.0);
    }

    if (dither) {
        // Triangular dither of +-1 LSB breaks 8-bit banding in dark night gradients.
        float d = Hash12(gl_FragCoord.xy, float(uFrameIndex % 64u) + 17.0) +
                  Hash12(gl_FragCoord.yx, float(uFrameIndex % 64u) + 3.0) - 1.0;
        mapped += d / 255.0;
    }

    FragColor = vec4(mapped, 1.0);
}
