#ifndef SURFEL_GI_PROJECTION_SUPPORT_GLSL
#define SURFEL_GI_PROJECTION_SUPPORT_GLSL

#include "surfel_gi_common.glsl"

const float SURFEL_GI_PROJECTION_EPSILON = 1e-5;

void SurfelBuildOrthonormalBasis(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
    normal = normalize(normal);

    float signN = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (signN + normal.z);
    float b = normal.x * normal.y * a;

    tangent = vec3(1.0 + signN * normal.x * normal.x * a, signN * b, -signN * normal.x);
    bitangent = vec3(b, signN + normal.y * normal.y * a, -normal.y);
}

vec4 SurfelWorldToClip(vec3 worldPos, mat4 view, mat4 projection)
{
    return projection * (view * vec4(worldPos, 1.0));
}

vec2 SurfelClipToPixel(vec4 clip, vec2 resolution)
{
    vec2 ndc = clip.xy / max(clip.w, SURFEL_GI_PROJECTION_EPSILON);
    return (ndc * 0.5 + 0.5) * max(resolution, vec2(1.0));
}

bool SurfelProjectWorldToPixel(vec3 worldPos, mat4 view, mat4 projection, vec2 resolution, out vec2 pixel)
{
    vec4 clip = SurfelWorldToClip(worldPos, view, projection);
    if (clip.w <= SURFEL_GI_PROJECTION_EPSILON) {
        pixel = vec2(0.0);
        return false;
    }

    pixel = SurfelClipToPixel(clip, resolution);
    return true;
}

bool SurfelProjectPatchCorners(
    vec3 worldPos,
    vec3 tangent,
    vec3 bitangent,
    float radius,
    mat4 view,
    mat4 projection,
    vec2 resolution,
    out vec2 centerPx,
    out vec2 corner00,
    out vec2 corner10,
    out vec2 corner01,
    out vec2 corner11
)
{
    vec4 centerClip = SurfelWorldToClip(worldPos, view, projection);
    if (centerClip.w <= SURFEL_GI_PROJECTION_EPSILON) {
        centerPx = vec2(0.0);
        corner00 = vec2(0.0);
        corner10 = vec2(0.0);
        corner01 = vec2(0.0);
        corner11 = vec2(0.0);
        return false;
    }

    centerPx = SurfelClipToPixel(centerClip, resolution);

    vec3 cornerWorld00 = worldPos + (-tangent - bitangent) * radius;
    vec3 cornerWorld10 = worldPos + ( tangent - bitangent) * radius;
    vec3 cornerWorld01 = worldPos + (-tangent + bitangent) * radius;
    vec3 cornerWorld11 = worldPos + ( tangent + bitangent) * radius;

    bool valid00 = SurfelProjectWorldToPixel(cornerWorld00, view, projection, resolution, corner00);
    bool valid10 = SurfelProjectWorldToPixel(cornerWorld10, view, projection, resolution, corner10);
    bool valid01 = SurfelProjectWorldToPixel(cornerWorld01, view, projection, resolution, corner01);
    bool valid11 = SurfelProjectWorldToPixel(cornerWorld11, view, projection, resolution, corner11);

    if (!valid00) corner00 = centerPx;
    if (!valid10) corner10 = centerPx;
    if (!valid01) corner01 = centerPx;
    if (!valid11) corner11 = centerPx;

    return valid00 || valid10 || valid01 || valid11;
}

bool SurfelProjectPatchAxes(
    vec3 worldPos,
    vec3 tangent,
    vec3 bitangent,
    float radius,
    mat4 view,
    mat4 projection,
    vec2 resolution,
    out vec2 centerPx,
    out vec2 axisTangentPx,
    out vec2 axisBitangentPx
)
{
    vec4 centerClip = SurfelWorldToClip(worldPos, view, projection);
    if (centerClip.w <= SURFEL_GI_PROJECTION_EPSILON) {
        centerPx = vec2(0.0);
        axisTangentPx = vec2(0.0);
        axisBitangentPx = vec2(0.0);
        return false;
    }

    centerPx = SurfelClipToPixel(centerClip, resolution);

    vec4 tangentClip = SurfelWorldToClip(worldPos + tangent * radius, view, projection);
    vec4 bitangentClip = SurfelWorldToClip(worldPos + bitangent * radius, view, projection);

    vec2 fallback = vec2(max(radius, 1.0));
    axisTangentPx = tangentClip.w > SURFEL_GI_PROJECTION_EPSILON ? SurfelClipToPixel(tangentClip, resolution) - centerPx : vec2(fallback.x, 0.0);
    axisBitangentPx = bitangentClip.w > SURFEL_GI_PROJECTION_EPSILON ? SurfelClipToPixel(bitangentClip, resolution) - centerPx : vec2(0.0, fallback.y);

    if (length(axisTangentPx) < SURFEL_GI_PROJECTION_EPSILON || length(axisBitangentPx) < SURFEL_GI_PROJECTION_EPSILON) {
        axisTangentPx = vec2(fallback.x, 0.0);
        axisBitangentPx = vec2(0.0, fallback.y);
    }

    return true;
}

void SurfelSupportBoundsFromCornerPixels(
    vec2 centerPx,
    vec2 corner00,
    vec2 corner10,
    vec2 corner01,
    vec2 corner11,
    vec2 resolution,
    out ivec2 minPixel,
    out ivec2 maxPixel
)
{
    vec2 minPx = min(min(corner00, corner10), min(corner01, corner11));
    vec2 maxPx = max(max(corner00, corner10), max(corner01, corner11));

    minPx = min(minPx, centerPx);
    maxPx = max(maxPx, centerPx);

    minPx -= vec2(1.0);
    maxPx += vec2(1.0);

    ivec2 screenMax = max(ivec2(floor(resolution)) - ivec2(1), ivec2(0));
    minPixel = clamp(ivec2(floor(minPx)), ivec2(0), screenMax);
    maxPixel = clamp(ivec2(ceil(maxPx)), ivec2(0), screenMax);
}

void SurfelSupportBoundsFromAxes(
    vec2 centerPx,
    vec2 axisTangentPx,
    vec2 axisBitangentPx,
    vec2 resolution,
    out ivec2 minPixel,
    out ivec2 maxPixel
)
{
    vec2 support = abs(axisTangentPx) + abs(axisBitangentPx);
    support = max(support + vec2(1.0), vec2(1.0));

    ivec2 screenMax = max(ivec2(floor(resolution)) - ivec2(1), ivec2(0));
    minPixel = clamp(ivec2(floor(centerPx - support)), ivec2(0), screenMax);
    maxPixel = clamp(ivec2(ceil(centerPx + support)), ivec2(0), screenMax);
}

float SurfelProjectedEllipseMetric(vec2 deltaPx, vec2 axisTangentPx, vec2 axisBitangentPx)
{
    float det = axisTangentPx.x * axisBitangentPx.y - axisTangentPx.y * axisBitangentPx.x;
    if (abs(det) < SURFEL_GI_PROJECTION_EPSILON) {
        return 1e9;
    }

    float u = (deltaPx.x * axisBitangentPx.y - deltaPx.y * axisBitangentPx.x) / det;
    float v = (-deltaPx.x * axisTangentPx.y + deltaPx.y * axisTangentPx.x) / det;
    return u * u + v * v;
}

bool SurfelInvertProjectedCoordinates(
    vec2 deltaPx,
    vec2 axisTangentPx,
    vec2 axisBitangentPx,
    out vec2 localUV
)
{
    float det = axisTangentPx.x * axisBitangentPx.y - axisTangentPx.y * axisBitangentPx.x;
    if (abs(det) < SURFEL_GI_PROJECTION_EPSILON) {
        localUV = vec2(0.0);
        return false;
    }

    localUV.x = (deltaPx.x * axisBitangentPx.y - deltaPx.y * axisBitangentPx.x) / det;
    localUV.y = (-deltaPx.x * axisTangentPx.y + deltaPx.y * axisTangentPx.x) / det;
    return true;
}

float SurfelEdgeFunction(vec2 a, vec2 b, vec2 p)
{
    vec2 ab = b - a;
    vec2 ap = p - a;
    return ab.x * ap.y - ab.y * ap.x;
}

bool SurfelPointInConvexQuad(
    vec2 p,
    vec2 corner00,
    vec2 corner10,
    vec2 corner11,
    vec2 corner01)
{
    float e0 = SurfelEdgeFunction(corner00, corner10, p);
    float e1 = SurfelEdgeFunction(corner10, corner11, p);
    float e2 = SurfelEdgeFunction(corner11, corner01, p);
    float e3 = SurfelEdgeFunction(corner01, corner00, p);

    bool nonNegative = e0 >= -SURFEL_GI_PROJECTION_EPSILON &&
        e1 >= -SURFEL_GI_PROJECTION_EPSILON &&
        e2 >= -SURFEL_GI_PROJECTION_EPSILON &&
        e3 >= -SURFEL_GI_PROJECTION_EPSILON;
    bool nonPositive = e0 <= SURFEL_GI_PROJECTION_EPSILON &&
        e1 <= SURFEL_GI_PROJECTION_EPSILON &&
        e2 <= SURFEL_GI_PROJECTION_EPSILON &&
        e3 <= SURFEL_GI_PROJECTION_EPSILON;
    return nonNegative || nonPositive;
}

float SurfelSignedPlaneDistance(vec3 point, vec3 planePoint, vec3 planeNormal)
{
    return dot(point - planePoint, normalize(planeNormal));
}

float SurfelViewSpacePlaneZAtXY(vec3 planePointView, vec3 planeNormalView, vec2 viewXY)
{
    vec3 n = normalize(planeNormalView);
    if (abs(n.z) < SURFEL_GI_PROJECTION_EPSILON) {
        return planePointView.z;
    }

    return planePointView.z - ((viewXY.x - planePointView.x) * n.x + (viewXY.y - planePointView.y) * n.y) / n.z;
}

float SurfelViewSpacePlaneDepthDelta(vec3 planePointView, vec3 planeNormalView, vec3 sampleViewPos)
{
    return sampleViewPos.z - SurfelViewSpacePlaneZAtXY(planePointView, planeNormalView, sampleViewPos.xy);
}

vec3 SurfelReconstructWorldPosition(ivec2 pixel, float depth, vec2 resolution, mat4 invViewProj)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(resolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / max(world.w, SURFEL_GI_PROJECTION_EPSILON);
}

#endif
