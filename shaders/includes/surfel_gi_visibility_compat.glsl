#ifndef SURFEL_GI_VISIBILITY_COMPAT_GLSL
#define SURFEL_GI_VISIBILITY_COMPAT_GLSL

const uint SURFEL_GI_VISIBILITY_REJECT_DEPTH = 1u;
const uint SURFEL_GI_VISIBILITY_REJECT_NORMAL = 2u;

struct SurfelGIVisibilityCompatResult {
    uint reasonBits;
    float validCoverage;
    float depthWeight;
    float normalWeight;
    float viewDepthDelta;
    float planeDepthDelta;
    float normalAlignment;
};

float SurfelGIResolveAcceptedCoverageWeight(float depthWeight, float normalWeight)
{
    float rawWeight = clamp(depthWeight * normalWeight, 0.0, 1.0);
    return mix(0.65, 1.0, rawWeight);
}

vec3 SurfelGIReconstructWorldPosition(ivec2 pixel, float depth, vec2 resolution, mat4 invViewProj)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(resolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / max(abs(world.w), 0.00001);
}

vec3 SurfelGIDecodeNormalOct(vec2 encodedNormal)
{
    vec2 e = encodedNormal * 2.0 - 1.0;
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 signNotZero = mix(vec2(1.0), sign(e), step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(n.yx)) * signNotZero;
    }
    return normalize(n);
}

vec3 SurfelGIWorldToViewPosition(vec3 worldPosition, mat4 viewMatrix)
{
    return (viewMatrix * vec4(worldPosition, 1.0)).xyz;
}

float SurfelGIComputeViewDepthDelta(vec3 surfelWorldPosition, vec3 gbufferWorldPosition, mat4 viewMatrix)
{
    vec3 surfelViewPosition = SurfelGIWorldToViewPosition(surfelWorldPosition, viewMatrix);
    vec3 gbufferViewPosition = SurfelGIWorldToViewPosition(gbufferWorldPosition, viewMatrix);
    return abs(gbufferViewPosition.z - surfelViewPosition.z);
}

float SurfelGIComputePlaneDepthDelta(vec3 surfelWorldPosition, vec3 surfelWorldNormal, vec3 gbufferWorldPosition)
{
    vec3 normal = normalize(surfelWorldNormal);
    return abs(dot(gbufferWorldPosition - surfelWorldPosition, normal));
}

float SurfelGIComputeNormalAlignment(vec3 surfelWorldNormal, vec3 gbufferWorldNormal)
{
    vec3 surfelNormal = normalize(surfelWorldNormal);
    vec3 receiverNormal = normalize(gbufferWorldNormal);
    return clamp(dot(surfelNormal, receiverNormal), -1.0, 1.0);
}

uint SurfelGIComputeVisibilityRejectBits(
    vec3 surfelWorldPosition,
    vec3 surfelWorldNormal,
    vec3 gbufferWorldPosition,
    vec3 gbufferWorldNormal,
    mat4 viewMatrix,
    float viewSpaceThickness,
    float normalRejectThreshold)
{
    float thickness = max(viewSpaceThickness, 0.0);
    float normalAlignment = SurfelGIComputeNormalAlignment(surfelWorldNormal, gbufferWorldNormal);
    float viewDepthDelta = SurfelGIComputeViewDepthDelta(surfelWorldPosition, gbufferWorldPosition, viewMatrix);
    float planeDepthDelta = SurfelGIComputePlaneDepthDelta(surfelWorldPosition, surfelWorldNormal, gbufferWorldPosition);

    uint reasonBits = 0u;
    if (max(viewDepthDelta, planeDepthDelta) > thickness) {
        reasonBits |= SURFEL_GI_VISIBILITY_REJECT_DEPTH;
    }
    if (normalAlignment < 1.0 - clamp(normalRejectThreshold, 0.0, 1.0)) {
        reasonBits |= SURFEL_GI_VISIBILITY_REJECT_NORMAL;
    }
    return reasonBits;
}

float SurfelGIComputeWeightedValidCoverage(
    vec3 surfelWorldPosition,
    vec3 surfelWorldNormal,
    vec3 gbufferWorldPosition,
    vec3 gbufferWorldNormal,
    mat4 viewMatrix,
    float viewSpaceThickness,
    float normalRejectThreshold)
{
    float thickness = max(viewSpaceThickness, 0.00001);
    float normalAlignment = SurfelGIComputeNormalAlignment(surfelWorldNormal, gbufferWorldNormal);
    float viewDepthDelta = SurfelGIComputeViewDepthDelta(surfelWorldPosition, gbufferWorldPosition, viewMatrix);
    float planeDepthDelta = SurfelGIComputePlaneDepthDelta(surfelWorldPosition, surfelWorldNormal, gbufferWorldPosition);
    float depthDelta = max(viewDepthDelta, planeDepthDelta);

    float depthWeight = 1.0 - smoothstep(thickness * 0.5, thickness, depthDelta);
    float normalWeight = smoothstep(1.0 - clamp(normalRejectThreshold, 0.0, 1.0), 1.0, normalAlignment);
    uint reasonBits = SurfelGIComputeVisibilityRejectBits(
        surfelWorldPosition,
        surfelWorldNormal,
        gbufferWorldPosition,
        gbufferWorldNormal,
        viewMatrix,
        viewSpaceThickness,
        normalRejectThreshold);
    return (reasonBits == 0u)
        ? SurfelGIResolveAcceptedCoverageWeight(depthWeight, normalWeight)
        : 0.0;
}

SurfelGIVisibilityCompatResult SurfelGIEvaluateVisibilityCompatibility(
    vec3 surfelWorldPosition,
    vec3 surfelWorldNormal,
    vec3 gbufferWorldPosition,
    vec3 gbufferWorldNormal,
    mat4 viewMatrix,
    float viewSpaceThickness,
    float normalRejectThreshold)
{
    SurfelGIVisibilityCompatResult result;
    result.viewDepthDelta = SurfelGIComputeViewDepthDelta(surfelWorldPosition, gbufferWorldPosition, viewMatrix);
    result.planeDepthDelta = SurfelGIComputePlaneDepthDelta(surfelWorldPosition, surfelWorldNormal, gbufferWorldPosition);
    result.normalAlignment = SurfelGIComputeNormalAlignment(surfelWorldNormal, gbufferWorldNormal);
    result.reasonBits = SurfelGIComputeVisibilityRejectBits(
        surfelWorldPosition,
        surfelWorldNormal,
        gbufferWorldPosition,
        gbufferWorldNormal,
        viewMatrix,
        viewSpaceThickness,
        normalRejectThreshold);
    result.depthWeight = 1.0 - smoothstep(
        max(viewSpaceThickness, 0.00001) * 0.5,
        max(viewSpaceThickness, 0.00001),
        max(result.viewDepthDelta, result.planeDepthDelta));
    result.normalWeight = smoothstep(
        1.0 - clamp(normalRejectThreshold, 0.0, 1.0),
        1.0,
        result.normalAlignment);
    result.validCoverage = (result.reasonBits == 0u)
        ? SurfelGIResolveAcceptedCoverageWeight(result.depthWeight, result.normalWeight)
        : 0.0;
    return result;
}

#endif
