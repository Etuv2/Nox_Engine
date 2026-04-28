#version 460 core

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform usampler2D uCoverageTex;
uniform sampler2D uDeficitTex;
uniform sampler2D uDepthTex;
uniform mat4 uInvViewProj;
uniform int uMode; // 0=raw projected support, 1=valid coverage, 2=deficit, 3=depth rejection, 4=normal rejection, 5=winner surfel ID, 6=TLAS BVH heatmap
uniform float uCoverageThreshold;
uniform int uHeatmapColorLimit;

#include "includes/rt_scene_common.glsl"

vec3 HeatRamp(float t)
{
    t = clamp(t, 0.0, 1.0);
    vec3 cool = mix(vec3(0.05, 0.12, 0.90), vec3(0.05, 0.85, 0.30), smoothstep(0.0, 0.45, t));
    vec3 warm = mix(cool, vec3(1.0, 0.85, 0.08), smoothstep(0.45, 0.75, t));
    return mix(warm, vec3(1.0, 0.15, 0.05), smoothstep(0.75, 1.0, t));
}

vec3 CoverageRamp(float ratio)
{
    ratio = clamp(ratio, 0.0, 2.0);
    if (ratio < 1.0) {
        return mix(vec3(1.0, 0.12, 0.10), vec3(1.0, 0.90, 0.18), smoothstep(0.0, 1.0, ratio));
    }
    return mix(vec3(0.10, 0.92, 0.32), vec3(0.16, 0.42, 1.0), smoothstep(1.0, 2.0, ratio));
}

vec3 RejectionRamp(float ratio, vec3 tint)
{
    float rejection = clamp(1.0 - ratio, 0.0, 1.0);
    return mix(vec3(0.03, 0.03, 0.03), tint, rejection);
}

vec3 HashToColor(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;

    vec3 color = vec3(
        float(value & 255u),
        float((value >> 8u) & 255u),
        float((value >> 16u) & 255u)) / 255.0;
    return mix(vec3(0.15), color, 0.9);
}

uint TraceTlasDebug(Ray ray, out uint nodeTests, out uint instanceTests)
{
    nodeTests = 0u;
    instanceTests = 0u;

    if (u_rtInstanceNodeCount <= 0) {
        return 0u;
    }

    int nodeStack[RT_SCENE_MAX_BVH_STACK_SIZE];
    int nodeStackPtr = 0;
    nodeStack[nodeStackPtr++] = 0;

    while (nodeStackPtr > 0 && nodeStackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
        int nodeIndex = nodeStack[--nodeStackPtr];
        if (nodeIndex < 0 || nodeIndex >= u_rtInstanceNodeCount) {
            continue;
        }

        ++nodeTests;
        RTInstanceNode node = rtInstanceNodes[nodeIndex];
        if (!rayAABBIntersect(ray, node.boundsMin.xyz, node.boundsMax.xyz)) {
            continue;
        }

        if (node.children.x >= 0 || node.children.y >= 0) {
            if (node.children.y >= 0 && nodeStackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                nodeStack[nodeStackPtr++] = node.children.y;
            }
            if (node.children.x >= 0 && nodeStackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                nodeStack[nodeStackPtr++] = node.children.x;
            }
            continue;
        }

        for (int leafSlot = 0; leafSlot < 4; ++leafSlot) {
            int instanceIndex = node.instances[leafSlot];
            if (instanceIndex < 0 || instanceIndex >= u_rtInstanceCount) {
                continue;
            }

            ++instanceTests;
            RTInstance instance = rtInstances[instanceIndex];
            if (!rayAABBIntersect(ray, instance.boundsMin.xyz, instance.boundsMax.xyz)) {
                continue;
            }
        }
    }

    return nodeTests + instanceTests * 3u;
}

vec3 TlasDebugColor(vec2 uv)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 nearClip = vec4(ndc, -1.0, 1.0);
    vec4 farClip = vec4(ndc, 1.0, 1.0);

    vec4 nearWorld = uInvViewProj * nearClip;
    vec4 farWorld = uInvViewProj * farClip;
    if (abs(nearWorld.w) < 1e-6 || abs(farWorld.w) < 1e-6) {
        return vec3(0.0);
    }

    vec3 rayOrigin = nearWorld.xyz / nearWorld.w;
    vec3 rayDirection = normalize((farWorld.xyz / farWorld.w) - rayOrigin);

    Ray ray;
    ray.origin = rayOrigin;
    ray.direction = rayDirection;
    ray.tMin = RT_SCENE_EPSILON;
    ray.tMax = RT_SCENE_MAX_FLOAT;

    uint nodeTests = 0u;
    uint instanceTests = 0u;
    uint complexity = TraceTlasDebug(ray, nodeTests, instanceTests);
    float scale = float(max(uHeatmapColorLimit, 1));
    return HeatRamp(float(complexity) / scale);
}

void main()
{
    if (uMode == 6) {
        FragColor = vec4(TlasDebugColor(TexCoord), 1.0);
        return;
    }

    float depth = texture(uDepthTex, TexCoord).r;
    if (depth >= 0.9999) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float threshold = max(uCoverageThreshold, 0.05);
    uint rawSupportBits = texture(uCoverageTex, TexCoord).r;
    float rawSupport = float(rawSupportBits) / 1024.0;
    float ratio = rawSupport / threshold;
    float deficit = texture(uDeficitTex, TexCoord).r;
    float deficitNorm = deficit / threshold;

    vec3 color = vec3(0.0);
    if (uMode == 0) {
        // Raw projected support stored in the R32UI coverage image.
        color = CoverageRamp(ratio);
    } else if (uMode == 1) {
        // Valid coverage: thresholded support visibility.
        color = mix(vec3(0.85, 0.10, 0.10), vec3(0.15, 0.95, 0.28), smoothstep(0.0, 1.0, clamp(ratio, 0.0, 1.0)));
    } else if (uMode == 2) {
        // Deficit stored in the R32F deficit image.
        color = HeatRamp(deficitNorm);
    } else if (uMode == 3) {
        // Depth rejection proxy. The current overlay path does not carry a dedicated
        // rejection buffer, so this visualizes the inverse of local support.
        color = RejectionRamp(ratio, vec3(1.0, 0.42, 0.12));
    } else if (uMode == 4) {
        // Normal rejection proxy. The pass currently does not bind a separate normal
        // rejection channel, so we keep the same support-driven contract with a different tint.
        color = RejectionRamp(ratio, vec3(0.48, 0.52, 1.0));
    } else if (uMode == 5) {
        // Winner surfel ID proxy. The current projected coverage buffer stores support counts;
        // this hashes the raw integer so the mode remains forward-compatible with an eventual ID channel.
        color = HashToColor(rawSupportBits);
    } else {
        color = HeatRamp(deficitNorm);
    }

    FragColor = vec4(color, 1.0);
}
