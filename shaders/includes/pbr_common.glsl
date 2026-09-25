/**
 * @file pbr_common.glsl
 * @brief Shared Principled-style BRDF helpers for deferred, forward, and RT rendering
 *
 * The helpers in this file keep the engine on one canonical shading contract:
 * a metallic/roughness base layer with explicit specular F0, layered clearcoat,
 * and prepared hooks for sheen, subsurface, and transmission/volume extensions.
 */

#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

// Constants
const float PI = 3.14159265359;
const float TAU = 6.283185307179586;
const float INV_PI = 0.31830988618;
const float EPSILON = 1e-4;
const vec3 DIELECTRIC_F0 = vec3(0.04);

struct PrincipledSurface {
    vec3 baseColor;
    float metallic;
    float perceptualRoughness;
    float alphaRoughness;
    vec3 diffuseColor;
    float transmission;
    vec3 dielectricF0;
    float ior;
    vec3 specularF0;
    float clearcoat;
    vec3 attenuationColor;
    float clearcoatRoughness;
    float attenuationDistance;
    float thickness;
    float specularFactor;
    float sheen;
    float sheenTint;
    float subsurface;
};

struct PrincipledLobeContext {
    vec3 N;
    vec3 V;
    vec3 L;
    vec3 H;
    float NdotV;
    float NdotL;
    float NdotH;
    float VdotH;
    float LdotH;
};

float Saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

vec3 SaturateVec3(vec3 v) {
    return clamp(v, vec3(0.0), vec3(1.0));
}

float Pow5(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}

float Average3(vec3 v) {
    return dot(v, vec3(0.3333333333));
}

float Luminance(vec3 v) {
    return dot(v, vec3(0.2126, 0.7152, 0.0722));
}

vec3 SafeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 <= 1e-8) {
        return normalize(fallback);
    }
    return v * inversesqrt(len2);
}

float ClampPerceptualRoughness(float roughness) {
    return clamp(roughness, 0.04, 1.0);
}

// Octahedral normal encoding/decoding
// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/

vec2 EncodeNormalOct(vec3 n) {
    n = normalize(n);
    n /= (abs(n.x) + abs(n.y) + abs(n.z));

    if (n.z < 0.0) {
        vec2 signN = sign(n.xy);
        signN = mix(vec2(1.0), signN, step(vec2(0.0001), abs(n.xy)));
        n.xy = (1.0 - abs(n.yx)) * signN;
    }

    return n.xy * 0.5 + 0.5;
}

vec3 DecodeNormalOct(vec2 e) {
    e = e * 2.0 - 1.0;

    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);

    if (n.z < 0.0) {
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }

    return normalize(n);
}

// Fresnel helpers

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * Pow5(Saturate(1.0 - cosTheta));
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * Pow5(Saturate(1.0 - cosTheta));
}

float F0FromIOR(float ior) {
    float f = (ior - 1.0) / (ior + 1.0);
    return f * f;
}

float FresnelDielectric(float cosTheta, float ior) {
    float f0 = F0FromIOR(ior);
    return f0 + (1.0 - f0) * Pow5(Saturate(1.0 - cosTheta));
}

vec3 ComputeDielectricF0(float ior, float specularFactor, vec3 specularColorFactor) {
    float baseF0 = F0FromIOR(max(ior, 1.0));
    vec3 dielectricF0 = vec3(baseF0);
    dielectricF0 *= max(specularFactor, 0.0);
    dielectricF0 *= max(specularColorFactor, vec3(0.0));
    return SaturateVec3(dielectricF0);
}

vec3 ComputeSurfaceF0(vec3 albedo, float metallic, float ior, float specularFactor, vec3 specularColorFactor) {
    vec3 dielectricF0 = ComputeDielectricF0(ior, specularFactor, specularColorFactor);
    return mix(dielectricF0, max(albedo, vec3(0.0)), Saturate(metallic));
}

float ComputeTransmissionWeight(float transmission, float NdotV, vec3 F0) {
    float fresnel = Average3(FresnelSchlick(Saturate(NdotV), F0));
    return Saturate(transmission) * (1.0 - fresnel);
}

vec3 ComputeRefractionDirection(vec3 incident, vec3 normal, float ior, out bool totalInternalReflection) {
    float cosThetaI = dot(incident, normal);
    float etaI = 1.0;
    float etaT = max(ior, 1.0);
    vec3 n = normal;

    if (cosThetaI < 0.0) {
        cosThetaI = -cosThetaI;
    } else {
        float swapEta = etaI;
        etaI = etaT;
        etaT = swapEta;
        n = -normal;
    }

    float eta = etaI / max(etaT, 1e-6);
    float k = 1.0 - eta * eta * (1.0 - cosThetaI * cosThetaI);
    if (k < 0.0) {
        totalInternalReflection = true;
        return reflect(incident, normal);
    }

    totalInternalReflection = false;
    vec3 refracted = eta * incident + (eta * cosThetaI - sqrt(k)) * n;
    return SafeNormalize(refracted, reflect(incident, normal));
}

PrincipledSurface BuildPrincipledSurface(
    vec3 baseColor,
    float metallic,
    float roughness,
    float ior,
    float specularFactor,
    vec3 specularColorFactor,
    float transmission,
    float clearcoat,
    float clearcoatRoughness,
    float sheen,
    float sheenTint,
    float subsurface,
    float thickness,
    vec3 attenuationColor,
    float attenuationDistance
) {
    PrincipledSurface surface;
    surface.baseColor = max(baseColor, vec3(0.0));
    surface.metallic = Saturate(metallic);
    surface.perceptualRoughness = ClampPerceptualRoughness(roughness);
    surface.alphaRoughness = max(surface.perceptualRoughness * surface.perceptualRoughness, 0.0016);
    surface.diffuseColor = surface.baseColor * (1.0 - surface.metallic);
    surface.transmission = Saturate(transmission);
    surface.ior = max(ior, 1.0);
    surface.specularFactor = max(specularFactor, 0.0);
    surface.dielectricF0 = ComputeDielectricF0(surface.ior, surface.specularFactor, specularColorFactor);
    surface.specularF0 = mix(surface.dielectricF0, surface.baseColor, surface.metallic);
    surface.clearcoat = Saturate(clearcoat);
    surface.clearcoatRoughness = ClampPerceptualRoughness(max(clearcoatRoughness, 0.03));
    surface.sheen = Saturate(sheen);
    surface.sheenTint = Saturate(sheenTint);
    surface.subsurface = Saturate(subsurface);
    surface.thickness = max(thickness, 0.0);
    surface.attenuationColor = max(attenuationColor, vec3(1e-3));
    surface.attenuationDistance = max(attenuationDistance, 1e-3);
    return surface;
}

PrincipledSurface BuildStoredPrincipledSurface(
    vec3 baseColor,
    float metallic,
    float roughness,
    vec3 storedSpecularF0,
    float transmission,
    float clearcoat,
    float clearcoatRoughness
) {
    PrincipledSurface surface = BuildPrincipledSurface(
        baseColor,
        metallic,
        roughness,
        1.5,
        1.0,
        vec3(1.0),
        transmission,
        clearcoat,
        clearcoatRoughness,
        0.0,
        0.0,
        0.0,
        0.0,
        vec3(1.0),
        1.0
    );
    surface.specularF0 = SaturateVec3(storedSpecularF0);
    surface.dielectricF0 = mix(surface.specularF0, vec3(F0FromIOR(surface.ior)), surface.metallic);
    return surface;
}

PrincipledLobeContext BuildPrincipledLobeContext(vec3 N, vec3 V, vec3 L) {
    PrincipledLobeContext ctx;
    ctx.N = SafeNormalize(N, vec3(0.0, 1.0, 0.0));
    ctx.V = SafeNormalize(V, ctx.N);
    ctx.L = SafeNormalize(L, ctx.N);
    ctx.H = SafeNormalize(ctx.V + ctx.L, ctx.N);
    ctx.NdotV = Saturate(dot(ctx.N, ctx.V));
    ctx.NdotL = Saturate(dot(ctx.N, ctx.L));
    ctx.NdotH = Saturate(dot(ctx.N, ctx.H));
    ctx.VdotH = Saturate(dot(ctx.V, ctx.H));
    ctx.LdotH = Saturate(dot(ctx.L, ctx.H));
    return ctx;
}

// GGX / Smith helpers

float DistributionGGXAlpha(float NdotH, float alpha) {
    float a2 = alpha * alpha;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float alpha = ClampPerceptualRoughness(roughness);
    alpha *= alpha;
    return DistributionGGXAlpha(Saturate(dot(N, H)), alpha);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = Saturate(dot(N, V));
    float NdotL = Saturate(dot(N, L));
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float VisibilitySmithGGXCorrelatedAlpha(float NdotV, float NdotL, float alpha) {
    float a2 = alpha * alpha;
    float lambdaV = NdotL * sqrt(max((NdotV - a2 * NdotV) * NdotV + a2, 1e-6));
    float lambdaL = NdotV * sqrt(max((NdotL - a2 * NdotL) * NdotL + a2, 1e-6));
    return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

float VisibilitySmithGGXCorrelated(float NdotV, float NdotL, float roughness) {
    float alpha = ClampPerceptualRoughness(roughness);
    alpha *= alpha;
    return VisibilitySmithGGXCorrelatedAlpha(NdotV, NdotL, alpha);
}

// Ambient occlusion

vec3 CalculateDiffuseAlbedo(vec3 albedo, float metallic) {
    return albedo * (1.0 - metallic);
}

// Lagarde & de Rousiers 2014, "Moving Frostbite to PBR", listing 26.
// Smooth lobes are occluded less than diffuse AO near normal incidence; rough lobes
// converge to the diffuse AO. Takes perceptual roughness.
float SpecularOcclusion(float NdotV, float ao, float perceptualRoughness) {
    float alpha = perceptualRoughness * perceptualRoughness;
    return Saturate(pow(max(NdotV + ao, 0.0), exp2(-16.0 * alpha - 1.0)) - 1.0 + ao);
}

// Principled lobe helpers

float DisneyDiffuseTerm(PrincipledLobeContext ctx, float perceptualRoughness) {
    float fd90 = 0.5 + 2.0 * ctx.LdotH * ctx.LdotH * ClampPerceptualRoughness(perceptualRoughness);
    float lightScatter = 1.0 + (fd90 - 1.0) * Pow5(1.0 - ctx.NdotL);
    float viewScatter = 1.0 + (fd90 - 1.0) * Pow5(1.0 - ctx.NdotV);
    return lightScatter * viewScatter;
}

float DisneySubsurfaceDiffuseTerm(PrincipledLobeContext ctx, float perceptualRoughness) {
    float fss90 = ctx.LdotH * ctx.LdotH * ClampPerceptualRoughness(perceptualRoughness);
    float lightScatter = 1.0 + (fss90 - 1.0) * Pow5(1.0 - ctx.NdotL);
    float viewScatter = 1.0 + (fss90 - 1.0) * Pow5(1.0 - ctx.NdotV);
    float subsurface = 1.25 * (lightScatter * viewScatter * (1.0 / max(ctx.NdotL + ctx.NdotV, 1e-4) - 0.5) + 0.5);
    return max(subsurface, 0.0);
}

vec3 EvaluatePrincipledSpecularLobe(PrincipledSurface surface, PrincipledLobeContext ctx, out vec3 fresnelOut) {
    if (ctx.NdotL <= 0.0 || ctx.NdotV <= 0.0) {
        fresnelOut = vec3(0.0);
        return vec3(0.0);
    }

    float D = DistributionGGXAlpha(ctx.NdotH, surface.alphaRoughness);
    float Vis = VisibilitySmithGGXCorrelatedAlpha(ctx.NdotV, ctx.NdotL, surface.alphaRoughness);
    fresnelOut = FresnelSchlick(ctx.VdotH, surface.specularF0);
    return D * Vis * fresnelOut;
}

// KHR_materials_clearcoat: the coat lobe is weighted by the clearcoat factor itself.
// (The 0.25 scale belongs to Disney's GTR1 coat, which this renderer does not use.)
float ComputeClearcoatLayerWeight(PrincipledSurface surface) {
    return surface.clearcoat;
}

vec3 EvaluatePrincipledClearcoatLobe(PrincipledSurface surface, PrincipledLobeContext ctx) {
    float clearcoatWeight = ComputeClearcoatLayerWeight(surface);
    if (clearcoatWeight <= 0.0 || ctx.NdotL <= 0.0 || ctx.NdotV <= 0.0) {
        return vec3(0.0);
    }

    float ccAlpha = surface.clearcoatRoughness * surface.clearcoatRoughness;
    float D = DistributionGGXAlpha(ctx.NdotH, ccAlpha);
    float Vis = VisibilitySmithGGXCorrelatedAlpha(ctx.NdotV, ctx.NdotL, ccAlpha);
    vec3 F = FresnelSchlick(ctx.VdotH, DIELECTRIC_F0);
    return clearcoatWeight * D * Vis * F;
}

float ComputeBaseLayerAttenuation(PrincipledSurface surface, float NdotV) {
    float clearcoatWeight = ComputeClearcoatLayerWeight(surface);
    if (clearcoatWeight <= 0.0) {
        return 1.0;
    }
    return 1.0 - clearcoatWeight * FresnelSchlick(Saturate(NdotV), DIELECTRIC_F0).r;
}

vec3 EvaluatePrincipledDiffuseLobe(
    PrincipledSurface surface,
    PrincipledLobeContext ctx,
    vec3 fresnel,
    float transmissionWeight
) {
    float burleyDiffuse = DisneyDiffuseTerm(ctx, surface.perceptualRoughness);
    float subsurfaceDiffuse = DisneySubsurfaceDiffuseTerm(ctx, surface.perceptualRoughness);
    float diffuseTerm = mix(burleyDiffuse, subsurfaceDiffuse, surface.subsurface);
    // diffuseColor already carries (1 - metallic); applying it again would square it.
    vec3 kD = (vec3(1.0) - fresnel) * (1.0 - transmissionWeight);
    return kD * surface.diffuseColor * diffuseTerm * INV_PI;
}

vec3 EvaluatePrincipledSheenLobe(
    PrincipledSurface surface,
    PrincipledLobeContext ctx,
    float transmissionWeight
) {
    if (surface.sheen <= 0.0) {
        return vec3(0.0);
    }

    float lum = max(Luminance(surface.baseColor), 1e-4);
    vec3 tint = surface.baseColor / lum;
    vec3 sheenColor = mix(vec3(1.0), tint, surface.sheenTint);
    float sheenTerm = Pow5(1.0 - ctx.LdotH);
    float sheenWeight = surface.sheen * (1.0 - surface.metallic) * (1.0 - transmissionWeight);
    return sheenColor * sheenWeight * sheenTerm * INV_PI;
}

vec3 ComputeVolumeAbsorptionCoefficient(vec3 attenuationColor, float attenuationDistance) {
    vec3 safeColor = max(attenuationColor, vec3(1e-3));
    return -log(safeColor) / max(attenuationDistance, 1e-3);
}

// Tint of light transmitted through the material. KHR_materials_transmission tints transmitted
// light by the base color even for thin (thickness 0) surfaces; volumes additionally absorb along
// the path (Beer-Lambert, KHR_materials_volume). An infinite attenuationDistance means no absorption.
vec3 ComputeVolumeTransmittance(vec3 baseTint, float thickness, float attenuationDistance, vec3 attenuationColor) {
    vec3 tint = SaturateVec3(baseTint);
    float pathLength = max(thickness, 0.0);
    if (pathLength <= 0.0) {
        return tint;
    }

    vec3 sigmaA = ComputeVolumeAbsorptionCoefficient(attenuationColor, attenuationDistance);
    return exp(-sigmaA * pathLength) * tint;
}

void EvaluatePrincipledBRDFSeparated(
    PrincipledSurface surface,
    vec3 N,
    vec3 V,
    vec3 L,
    out vec3 diffuseOut,
    out vec3 specularOut
) {
    diffuseOut = vec3(0.0);
    specularOut = vec3(0.0);

    PrincipledLobeContext ctx = BuildPrincipledLobeContext(N, V, L);
    if (ctx.NdotL <= 0.0 || ctx.NdotV <= 0.0) {
        return;
    }

    vec3 fresnel;
    vec3 specularBRDF = EvaluatePrincipledSpecularLobe(surface, ctx, fresnel);
    float transmissionWeight = ComputeTransmissionWeight(surface.transmission, ctx.NdotV, surface.specularF0);

    vec3 diffuseBRDF = EvaluatePrincipledDiffuseLobe(surface, ctx, fresnel, transmissionWeight);
    vec3 sheenBRDF = EvaluatePrincipledSheenLobe(surface, ctx, transmissionWeight);
    vec3 clearcoatBRDF = EvaluatePrincipledClearcoatLobe(surface, ctx);
    float baseAttenuation = ComputeBaseLayerAttenuation(surface, ctx.NdotV);

    diffuseOut = (diffuseBRDF + sheenBRDF) * baseAttenuation * ctx.NdotL;
    specularOut = (specularBRDF * baseAttenuation + clearcoatBRDF) * ctx.NdotL;
}

vec3 EvaluatePrincipledBRDF(
    PrincipledSurface surface,
    vec3 N,
    vec3 V,
    vec3 L
) {
    vec3 diffuse;
    vec3 specular;
    EvaluatePrincipledBRDFSeparated(surface, N, V, L, diffuse, specular);
    return diffuse + specular;
}

// Split-sum LUT lookup: x = scale, y = bias of the directional albedo (Karis 2013).
vec2 SampleBRDFLUT(sampler2D brdfLUT, float NdotV, float perceptualRoughness) {
    return max(texture(brdfLUT, vec2(Saturate(NdotV), Saturate(perceptualRoughness))).rg, vec2(0.0));
}

// Single + multiple scattering specular energy for image based lighting
// (Fdez-Aguera 2019, "A Multiple-Scattering Microfacet Model for Real-Time IBL").
//   FssEss: single-scattered energy, pairs with the prefiltered radiance
//   FmsEms: multiple-scattered energy, pairs with the (cosine-averaged) irradiance
// Diffuse must be weighted by 1 - (FssEss + FmsEms) to stay energy conserving.
// The LUT's bias term already integrates Schlick's (1 - VdotH)^5 over the lobe, so F0 is
// used as-is; feeding a view-dependent Fresnel in here would count grazing Fresnel twice.
void ComputeIBLSpecularEnergy(vec3 F0, float NdotV, vec2 brdf, out vec3 FssEss, out vec3 FmsEms) {
    FssEss = F0 * brdf.x + brdf.y;
    float Ess = brdf.x + brdf.y;
    float Ems = Saturate(1.0 - Ess);
    vec3 Favg = F0 + (vec3(1.0) - F0) * (1.0 / 21.0);
    FmsEms = Ems * FssEss * Favg / max(vec3(1.0) - Ems * Favg, vec3(1e-4));
}

vec3 EvaluatePrincipledIBL(
    PrincipledSurface surface,
    vec3 N,
    vec3 V,
    vec3 R,
    float diffuseAO,
    float specularAO,
    samplerCube irradianceMap,
    samplerCube prefilteredMap,
    sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity,
    float diffuseIBLScale,
    float specularIBLScale
) {
    float NdotV = Saturate(dot(N, V));

    // The irradiance map stores E / PI (see irradiance_convolution_frag.glsl), so a
    // Lambertian lobe is simply diffuseColor * irradiance.
    vec3 irradiance = max(texture(irradianceMap, N).rgb, vec3(0.0));
    vec3 prefiltered = max(textureLod(prefilteredMap, R, surface.perceptualRoughness * prefilteredMaxLOD).rgb, vec3(0.0));
    vec2 brdf = SampleBRDFLUT(brdfLUT, NdotV, surface.perceptualRoughness);

    vec3 FssEss;
    vec3 FmsEms;
    ComputeIBLSpecularEnergy(surface.specularF0, NdotV, brdf, FssEss, FmsEms);

    float transmissionWeight = ComputeTransmissionWeight(surface.transmission, NdotV, surface.specularF0);
    float diffuseTerm = mix(1.0, 1.0 + 0.5 * surface.perceptualRoughness, surface.subsurface);
    vec3 kD = max(vec3(1.0) - (FssEss + FmsEms), vec3(0.0)) * (1.0 - transmissionWeight);
    vec3 diffuse = kD * surface.diffuseColor * irradiance * diffuseAO * diffuseIBLScale * diffuseTerm;

    vec3 specular = (FssEss * prefiltered + FmsEms * irradiance) * specularAO * specularIBLScale;

    vec3 ibl = diffuse + specular;

    if (surface.sheen > 0.0) {
        float lum = max(Luminance(surface.baseColor), 1e-4);
        vec3 tint = surface.baseColor / lum;
        vec3 sheenColor = mix(vec3(1.0), tint, surface.sheenTint);
        ibl += irradiance * sheenColor * surface.sheen * Pow5(1.0 - NdotV) * diffuseAO * diffuseIBLScale * (1.0 - surface.metallic) * (1.0 - transmissionWeight);
    }

    float clearcoatWeight = ComputeClearcoatLayerWeight(surface);
    if (clearcoatWeight > 0.0) {
        // The coat has its own roughness, so it needs its own LUT lookup and prefiltered mip.
        vec2 ccBrdf = SampleBRDFLUT(brdfLUT, NdotV, surface.clearcoatRoughness);
        vec3 ccPrefiltered = max(textureLod(prefilteredMap, R, surface.clearcoatRoughness * prefilteredMaxLOD).rgb, vec3(0.0));
        vec3 ccSpecular = ccPrefiltered * (DIELECTRIC_F0 * ccBrdf.x + ccBrdf.y) * specularAO * specularIBLScale;
        float baseAttenuation = ComputeBaseLayerAttenuation(surface, NdotV);
        ibl = ibl * baseAttenuation + clearcoatWeight * ccSpecular;
    }

    return ibl * iblIntensity;
}

// Backwards-compatible canonical wrappers

void EvaluateCanonicalBRDFSeparated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness,
    out vec3 diffuseOut, out vec3 specularOut
) {
    PrincipledSurface surface = BuildStoredPrincipledSurface(
        albedo, metallic, roughness, F0, transmission, clearcoat, clearcoatRoughness
    );
    EvaluatePrincipledBRDFSeparated(surface, N, V, L, diffuseOut, specularOut);
}

vec3 EvaluateCanonicalBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness
) {
    PrincipledSurface surface = BuildStoredPrincipledSurface(
        albedo, metallic, roughness, F0, transmission, clearcoat, clearcoatRoughness
    );
    return EvaluatePrincipledBRDF(surface, N, V, L);
}

vec3 EvaluateCanonicalIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    PrincipledSurface surface = BuildStoredPrincipledSurface(
        albedo, metallic, roughness, F0, transmission, clearcoat, clearcoatRoughness
    );
    return EvaluatePrincipledIBL(
        surface, N, V, R,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );
}

vec3 EvaluateSpecularBRDF(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    if (NdotL <= 0.0 || NdotV <= 0.0) {
        return vec3(0.0);
    }

    float D = DistributionGGX(N, H, roughness);
    float Vis = VisibilitySmithGGXCorrelated(NdotV, NdotL, roughness);
    vec3 F = FresnelSchlick(HdotV, F0);
    return D * Vis * F;
}

vec3 EvaluateDiffuseBRDF(vec3 albedo, float metallic, vec3 F) {
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    return kD * albedo * INV_PI;
}

// Backwards-compatible wrappers for older call sites.
vec3 EvaluateBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0
) {
    return EvaluateCanonicalBRDF(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0);
}

void EvaluateBRDF_Separated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    out vec3 diffuseOut, out vec3 specularOut
) {
    EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0, diffuseOut, specularOut);
}

vec3 EvaluateMetallicRoughnessBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0
) {
    return EvaluateCanonicalBRDF(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0);
}

void EvaluateMetallicRoughnessBRDFSeparated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    out vec3 diffuseOut, out vec3 specularOut
) {
    EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0, diffuseOut, specularOut);
}

vec3 EvaluateIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    return EvaluateCanonicalIBL(
        N, V, R, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );
}

vec3 EvaluateMetallicRoughnessIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    return EvaluateCanonicalIBL(
        N, V, R, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );
}

// Sampling utilities (for path tracing)

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, vec3 V, float roughness) {
    float alpha = roughness * roughness;

    float phi = TAU * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    H = normalize(tangent * H.x + bitangent * H.y + N * H.z);
    return reflect(-V, H);
}

float GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness) {
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    float D = DistributionGGX(N, H, roughness);
    return D * NdotH / (4.0 * HdotV + 0.0001);
}

float CosinePDF(float NdotL) {
    return NdotL * INV_PI;
}

#endif // PBR_COMMON_GLSL
