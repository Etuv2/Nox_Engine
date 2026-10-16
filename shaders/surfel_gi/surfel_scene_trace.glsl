#ifndef NOX_SURFEL_SCENE_TRACE_GLSL
#define NOX_SURFEL_SCENE_TRACE_GLSL

#include "../includes/shadow_bias_common.glsl"

// Software BVH traversal, direct light at a hit and the sky miss radiance. Shared by trace_rays.comp (surfel
// ray tracing) and reference_gi.comp (independent path-traced reference). Include after common.glsl.

const uint SURFEL_TRACE_MAX_LIGHTS = 32u;
const float SURFEL_TRACE_BVH_FAR_DISTANCE = 256.0;
// Miss radiance is read at the base level of the environment cube. The prefiltered cube has five levels (0-4),
// so the old mip 5 read a level that does not exist and returned black: the surfel and reference sky was zero
// on every scene with a skybox (plan section 13, bug 1). Level 0 is the unfiltered map, so a sky miss is exact.
const float SURFEL_ENVIRONMENT_MISS_MIP = 0.0;
const float SURFEL_INV_PI = 0.3183098861837907;
const int SURFEL_TRACE_BVH_STACK_SIZE = 64;

struct LightData {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 attenuation;
    vec4 shadowData;
    vec4 spotData;
    vec4 areaData;
    vec4 sampling;
};

layout(binding = B_LIGHTS, std430) readonly buffer LightDataBuffer
{
    LightData lights[];
};

layout(binding = B_SHADOW_MATRICES, std430) readonly buffer ShadowMatricesBuffer
{
    mat4 shadowMatrices[];
};

struct RTMaterial {
    vec3 albedo;
    float metallic;
    vec3 emissive;
    float roughness;
    vec3 specular;
    float emissiveStrength;
    vec3 specularColorFactor;
    float transmissionFactor;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float ior;
    float attenuationDistance;
    vec3 attenuationColor;
    float thicknessFactor;
    uint materialID;
    float normalScale;
    float occlusionStrength;
    float specularFactor;
    float alpha;
    float alphaCutoff;
    uint alphaMode;
    float padding0;
};

struct RTTriangle {
    vec3 v0; float pad0;
    vec3 v1; float pad1;
    vec3 v2; float pad2;
    vec3 n0; float pad3;
    vec3 n1; float pad4;
    vec3 n2; float pad5;
    vec3 center; float pad6;
    vec3 aabbMin; float pad7;
    vec3 aabbMax; float pad8;
    RTMaterial material;
};

struct RTBVHNode {
    vec3 minBounds; int child0;
    vec3 maxBounds; int child1;
    int tri0;
    int tri1;
    int tri2;
    int tri3;
};

struct RTInstance {
    vec4 boundsMin;
    vec4 boundsMax;
    mat4 worldFromLocal;
    mat4 localFromWorld;
    uvec4 metadata;
};

struct RTInstanceNode {
    vec4 boundsMin;
    vec4 boundsMax;
    ivec4 children;
    ivec4 instances;
};

layout(std140, binding = B_BVH_TRIANGLES) readonly buffer SurfelTraceTriangleBuffer
{
    RTTriangle bvhTriangles[];
};

layout(std140, binding = B_BVH_NODES) readonly buffer SurfelTraceBVHNodeBuffer
{
    RTBVHNode bvhNodes[];
};

layout(std430, binding = B_RT_INSTANCES) readonly buffer SurfelTraceInstanceBuffer
{
    RTInstance rtInstances[];
};

layout(std430, binding = B_RT_INSTANCE_NODES) readonly buffer SurfelTraceInstanceNodeBuffer
{
    RTInstanceNode rtInstanceNodes[];
};

uniform sampler2DArrayShadow uMultiLightShadowArray;
uniform samplerCube uSkyEnvironmentMap;
uniform uint uTriangleCount = 0u;
uniform uint uBVHNodeCount = 0u;
uniform uint uRTInstanceCount = 0u;
uniform uint uRTInstanceNodeCount = 0u;
uniform vec3 uDirectionalLightDirection;
uniform vec3 uDirectionalLightRadiance;
uniform vec3 uSkyRadiance;
uniform uint uLightCount;
uniform int uUseSkyEnvironmentMap = 0;
uniform float uSkyDiffuseScale = 0.0;
uniform int uUseSoftwareBVHTrace = 1;
uniform mat4 uView;
uniform int uEnableShadows = 1;
uniform vec4 uCascadeSplits = vec4(10.0, 30.0, 100.0, 500.0);
// Bias strength shared with the deferred pass (NOX_SHADOW_REFERENCE_BIAS == 1x).
uniform float uShadowBias = 0.001;

// Emissive triangles of the traced instances, world space, for next-event estimation (plan section 14). Built and
// uploaded by RTSceneResources together with the instance list, so the emitters and the BVH always agree.
struct SurfelEmitter
{
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 emission;   // rgb emitted radiance, w = luminance
    vec4 normal;     // xyz geometric unit normal, w = area
    vec4 cdf;        // x = cumulative luminance * area
};

layout(std430, binding = B_SURFEL_EMITTERS) readonly buffer SurfelEmitterBuffer
{
    SurfelEmitter emitters[];
};

uniform uint uEmitterCount = 0u;
// Total power sum(luminance * area): an emitter is picked with weight luminance * area / total, and a point on it has
// area density luminance / total.
uniform float uEmitterPowerTotal = 0.0;

struct SoftwareBVHHit
{
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    RTMaterial material;
    // Geometric normal of the hit triangle in world space (emitter MIS uses its exact Jacobian), and the triangle's
    // local vertices so the instance transform can take them to world space.
    vec3 geometricNormal;
    vec3 localV0;
    vec3 localV1;
    vec3 localV2;
    // Transform record of the hit instance (GPU transform buffer index, or 0xffffffff), and whether it is skinned.
    // A world surfel can only be re-attached to a rigid transform, so skinned hits are not queued.
    uint transformID;
    bool skinned;
};


bool SurfelTraceTriangle(vec3 origin,
                         vec3 direction,
                         float tMin,
                         float tMax,
                         RTTriangle tri,
                         out float t,
                         out vec3 barycentric)
{
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(direction, edge2);
    float a = dot(edge1, h);
    if (abs(a) < 1e-6) {
        return false;
    }

    float invA = 1.0 / a;
    vec3 s = origin - tri.v0;
    float u = invA * dot(s, h);
    if (u < 0.0 || u > 1.0) {
        return false;
    }

    vec3 q = cross(s, edge1);
    float v = invA * dot(direction, q);
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }

    t = invA * dot(edge2, q);
    if (t < tMin || t > tMax) {
        return false;
    }

    barycentric = vec3(1.0 - u - v, u, v);
    return true;
}

bool SurfelTraceAABB(vec3 origin, vec3 direction, vec3 minBounds, vec3 maxBounds, float tMin, float tMax)
{
    vec3 safeDirection = mix(vec3(1e-8), direction, greaterThan(abs(direction), vec3(1e-8)));
    vec3 invDir = 1.0 / safeDirection;
    vec3 t0 = (minBounds - origin) * invDir;
    vec3 t1 = (maxBounds - origin) * invDir;
    vec3 tNear3 = min(t0, t1);
    vec3 tFar3 = max(t0, t1);
    float tNear = max(max(tNear3.x, tNear3.y), max(tNear3.z, tMin));
    float tFar = min(min(tFar3.x, tFar3.y), min(tFar3.z, tMax));
    return tNear <= tFar;
}

// anyHit stops at the first triangle in range (an occlusion query); the hit is then not the closest one.
SoftwareBVHHit TraceSoftwareBLAS(vec3 origin, vec3 direction, float tMin, float tMax, int rootNodeOffset, int triangleOffset, bool anyHit)
{
    SoftwareBVHHit hit;
    hit.hit = false;
    hit.t = tMax;
    hit.position = origin + direction * tMax;
    hit.normal = vec3(0.0, 1.0, 0.0);

    if (uUseSoftwareBVHTrace == 0 ||
        uBVHNodeCount == 0u ||
        uTriangleCount == 0u ||
        rootNodeOffset < 0 ||
        uint(rootNodeOffset) >= uBVHNodeCount ||
        triangleOffset < 0 ||
        uint(triangleOffset) >= uTriangleCount) {
        return hit;
    }

    int stack[SURFEL_TRACE_BVH_STACK_SIZE];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0) {
        int localNodeIndex = stack[--stackPtr];
        int nodeIndex = rootNodeOffset + localNodeIndex;
        if (localNodeIndex < 0 || nodeIndex < 0 || uint(nodeIndex) >= uBVHNodeCount) {
            continue;
        }

        RTBVHNode node = bvhNodes[nodeIndex];
        if (!SurfelTraceAABB(origin, direction, node.minBounds, node.maxBounds, tMin, hit.t)) {
            continue;
        }

        bool leaf = node.child0 < 0 && node.child1 < 0;
        if (leaf) {
            int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);
            for (int i = 0; i < 4; ++i) {
                int triIndex = triIndices[i];
                if (triIndex < 0 || uint(triIndex) >= uTriangleCount) {
                    continue;
                }
                int packedTriIndex = triangleOffset + triIndex;
                if (packedTriIndex < 0 || uint(packedTriIndex) >= uTriangleCount) {
                    continue;
                }

                RTTriangle tri = bvhTriangles[packedTriIndex];
                if (tri.material.alphaMode != 0u && tri.material.alpha < tri.material.alphaCutoff) {
                    continue;
                }

                float t;
                vec3 bary;
                if (!SurfelTraceTriangle(origin, direction, tMin, hit.t, tri, t, bary)) {
                    continue;
                }

                vec3 normal = SurfelSafeNormalize(tri.n0 * bary.x + tri.n1 * bary.y + tri.n2 * bary.z);
                if (dot(normal, -direction) < 0.0) {
                    normal = -normal;
                }

                hit.hit = true;
                hit.t = t;
                hit.position = origin + direction * t;
                hit.normal = normal;
                hit.material = tri.material;
                hit.localV0 = tri.v0;
                hit.localV1 = tri.v1;
                hit.localV2 = tri.v2;
                hit.geometricNormal = SurfelSafeNormalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
                if (anyHit) {
                    return hit;
                }
            }
        } else {
            if (node.child1 >= 0 && stackPtr < SURFEL_TRACE_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child1;
            }
            if (node.child0 >= 0 && stackPtr < SURFEL_TRACE_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child0;
            }
        }
    }

    return hit;
}

SoftwareBVHHit TraceSoftwareTLAS(vec3 origin, vec3 direction, float tMin, float tMax, bool anyHit)
{
    SoftwareBVHHit bestHit;
    bestHit.hit = false;
    bestHit.t = tMax;
    bestHit.position = origin + direction * tMax;
    bestHit.normal = vec3(0.0, 1.0, 0.0);

    if (uRTInstanceCount == 0u || uRTInstanceNodeCount == 0u) {
        return bestHit;
    }

    int nodeStack[SURFEL_TRACE_BVH_STACK_SIZE];
    int nodeStackPtr = 0;
    nodeStack[nodeStackPtr++] = 0;

    while (nodeStackPtr > 0) {
        int nodeIndex = nodeStack[--nodeStackPtr];
        if (nodeIndex < 0 || uint(nodeIndex) >= uRTInstanceNodeCount) {
            continue;
        }

        RTInstanceNode node = rtInstanceNodes[nodeIndex];
        if (!SurfelTraceAABB(origin, direction, node.boundsMin.xyz, node.boundsMax.xyz, tMin, bestHit.t)) {
            continue;
        }

        if (node.children.x >= 0 || node.children.y >= 0) {
            if (node.children.y >= 0 && nodeStackPtr < SURFEL_TRACE_BVH_STACK_SIZE) {
                nodeStack[nodeStackPtr++] = node.children.y;
            }
            if (node.children.x >= 0 && nodeStackPtr < SURFEL_TRACE_BVH_STACK_SIZE) {
                nodeStack[nodeStackPtr++] = node.children.x;
            }
            continue;
        }

        for (int leafSlot = 0; leafSlot < 4; ++leafSlot) {
            int instanceIndex = node.instances[leafSlot];
            if (instanceIndex < 0 || uint(instanceIndex) >= uRTInstanceCount) {
                continue;
            }

            RTInstance instance = rtInstances[instanceIndex];
            if (!SurfelTraceAABB(origin, direction, instance.boundsMin.xyz, instance.boundsMax.xyz, tMin, bestHit.t)) {
                continue;
            }

            vec3 localOrigin = (instance.localFromWorld * vec4(origin, 1.0)).xyz;
            vec3 localDirection = SurfelSafeNormalize((instance.localFromWorld * vec4(direction, 0.0)).xyz);
            SoftwareBVHHit localHit = TraceSoftwareBLAS(
                localOrigin,
                localDirection,
                tMin,
                tMax,
                int(instance.metadata.x),
                int(instance.metadata.y),
                anyHit);
            if (!localHit.hit) {
                continue;
            }

            vec3 worldPosition = (instance.worldFromLocal * vec4(localHit.position, 1.0)).xyz;
            float worldT = length(worldPosition - origin);
            if (worldT < tMin || worldT > bestHit.t || worldT > tMax) {
                continue;
            }

            mat3 normalMatrix = transpose(mat3(instance.localFromWorld));
            bestHit.hit = true;
            bestHit.t = worldT;
            bestHit.position = worldPosition;
            bestHit.normal = SurfelSafeNormalize(normalMatrix * localHit.normal);
            bestHit.material = localHit.material;
            bestHit.transformID = instance.metadata.w;
            bestHit.skinned = instance.metadata.z != 0u;
            // Same cross product as the emitter list (RTSceneResources::AppendInstanceEmitters): world vertices.
            vec3 worldV0 = (instance.worldFromLocal * vec4(localHit.localV0, 1.0)).xyz;
            vec3 worldV1 = (instance.worldFromLocal * vec4(localHit.localV1, 1.0)).xyz;
            vec3 worldV2 = (instance.worldFromLocal * vec4(localHit.localV2, 1.0)).xyz;
            bestHit.geometricNormal = SurfelSafeNormalize(cross(worldV1 - worldV0, worldV2 - worldV0));
            if (anyHit) {
                return bestHit;
            }
        }
    }

    return bestHit;
}

SoftwareBVHHit TraceSoftwareBVH(vec3 origin, vec3 direction, float tMin, float tMax)
{
    if (uUseSoftwareBVHTrace == 0 || uBVHNodeCount == 0u || uTriangleCount == 0u) {
        SoftwareBVHHit miss;
        miss.hit = false;
        miss.t = tMax;
        miss.position = origin + direction * tMax;
        miss.normal = vec3(0.0, 1.0, 0.0);
        return miss;
    }

    if (uRTInstanceCount > 0u && uRTInstanceNodeCount > 0u) {
        return TraceSoftwareTLAS(origin, direction, tMin, tMax, false);
    }

    return TraceSoftwareBLAS(origin, direction, tMin, tMax, 0, 0, false);
}

// Occlusion query: any triangle in range blocks. Cheaper than the closest hit, which the emitter shadow rays do not need.
SoftwareBVHHit TraceSoftwareBVHAnyHit(vec3 origin, vec3 direction, float tMin, float tMax)
{
    if (uUseSoftwareBVHTrace == 0 || uBVHNodeCount == 0u || uTriangleCount == 0u) {
        SoftwareBVHHit miss;
        miss.hit = false;
        miss.t = tMax;
        miss.position = origin + direction * tMax;
        miss.normal = vec3(0.0, 1.0, 0.0);
        return miss;
    }

    if (uRTInstanceCount > 0u && uRTInstanceNodeCount > 0u) {
        return TraceSoftwareTLAS(origin, direction, tMin, tMax, true);
    }

    return TraceSoftwareBLAS(origin, direction, tMin, tMax, 0, 0, true);
}


bool ShadowInBounds(vec3 p)
{
    // No upper depth bound (see InShadowBounds in includes/shadow_common.glsl): ray hits beyond a
    // cascade's far plane compare correctly at depth 1.0.
    return p.x >= 0.0 && p.x <= 1.0 &&
        p.y >= 0.0 && p.y <= 1.0 &&
        p.z >= -0.01;
}

float SampleShadowLayer(int layer, vec3 projCoords, float bias)
{
    if (!ShadowInBounds(projCoords) || layer < 0) {
        return 1.0;
    }

    vec2 texel = 1.0 / vec2(max(textureSize(uMultiLightShadowArray, 0).xy, ivec2(1)));
    vec2 uv = clamp(projCoords.xy, texel * 0.5, vec2(1.0) - texel * 0.5);
    float compareDepth = clamp(projCoords.z, 0.0, 1.0) - bias;

    float center = texture(uMultiLightShadowArray, vec4(uv, float(layer), compareDepth));
    float sum = center * 2.0;
    float weight = 2.0;
    vec2 offsets[4] = vec2[](
        vec2(1.0, 0.0),
        vec2(-1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(0.0, -1.0)
    );
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUV = uv + offsets[i] * texel;
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
            continue;
        }
        sum += texture(uMultiLightShadowArray, vec4(sampleUV, float(layer), compareDepth));
        weight += 1.0;
    }

    return sum / max(weight, 1e-5);
}

// One shadow layer with the same world-space receiver biasing as the deferred lighting pass.
float SampleShadowLayerWorld(int layer, vec3 worldPos, vec3 normal, vec3 surfaceToLight)
{
    if (layer < 0) {
        return 1.0;
    }

    mat4 lightSpace = shadowMatrices[layer];
    vec3 surfaceCoords;
    if (!NoxProjectToShadowMap(lightSpace, worldPos, surfaceCoords) || !ShadowInBounds(surfaceCoords)) {
        return 1.0;
    }

    float resolution = float(max(textureSize(uMultiLightShadowArray, 0).x, 1));
    float texelWorld = NoxShadowTexelWorldSize(lightSpace, worldPos, resolution);
    // SampleShadowLayer filters a one-texel cross around the lookup.
    vec3 receiver = NoxShadowReceiverPosition(worldPos, normal, surfaceToLight, texelWorld, 1.0, NoxShadowBiasScale(uShadowBias));
    vec3 receiverCoords;
    if (!NoxProjectToShadowMap(lightSpace, receiver, receiverCoords)) {
        return 1.0;
    }
    receiverCoords.xy = clamp(receiverCoords.xy, vec2(0.0), vec2(1.0));
    return SampleShadowLayer(layer, receiverCoords, 0.0);
}

// Sun visibility outside the cascades. SampleShadowLayerWorld reads a point outside the cascade as lit, so the sun leaked
// into enclosed geometry and into every off-screen hit (plan section 15, lever 6). With this set (the surfel trace only),
// such a point gets a BVH shadow ray toward the sun instead. The reference keeps the cascade-only answer.
uniform int uSunShadowBVHFallback = 0;
// Debug trace-component skip mask (NOX_SURFEL_GI_TRACE_SKIP; plan section 19). 0 = the full trace. Each bit removes one
// component for a timing ablation (the output changes):
//   1  sun cascade samples read as lit (the outside-cascade BVH fallback is unchanged)
//   2  guide density off (trace_rays.comp, SurfelRaySamplingDensity)
//   4  emitter next-event samples (vertex and origin) off
//   8  visible G-buffer albedo off
//   16 cached albedo lookups at hits off
//   64 cache irradiance lookups and world requests off
//   128 closest-hit shading off (BVH trace only)
//   256 path continuation off past the first hit's own terms
uniform uint uTraceSkip = 0u;
// Whether the BVH sun fallback applies to the direct light being evaluated now. SurfelPathRadiance sets it per vertex
// (SurfelPathControl.sunBVHDepth), so the any-hit ray is paid only where the first hit's direct light needs it (plan 17).
bool gSunShadowBVHHere = true;
const float SURFEL_SHADOW_ORIGIN_OFFSET = 0.004;

bool SurfelShadowPointInLayer(int layer, vec3 worldPos)
{
    vec3 coords;
    return NoxProjectToShadowMap(shadowMatrices[layer], worldPos, coords) && ShadowInBounds(coords);
}

float DirectionalShadowVisibility(int startSlice, int sliceCount, vec3 worldPos, vec3 normal, vec3 lightDirection)
{
    int cascades = clamp(sliceCount, 1, 4);
    vec3 viewSpacePos = (uView * vec4(worldPos, 1.0)).xyz;
    float viewDepth = max(-viewSpacePos.z, 0.0);
    int cascadeIndex = cascades - 1;
    for (int i = 0; i < 4; ++i) {
        if (i >= cascades) {
            break;
        }
        if (viewDepth <= uCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }

    vec3 surfaceToLight = -lightDirection;
    if (uSunShadowBVHFallback != 0 && gSunShadowBVHHere && !SurfelShadowPointInLayer(startSlice + cascadeIndex, worldPos)) {
        SoftwareBVHHit blocker = TraceSoftwareBVHAnyHit(worldPos + normal * SURFEL_SHADOW_ORIGIN_OFFSET, surfaceToLight,
            1e-4, SURFEL_TRACE_BVH_FAR_DISTANCE);
        return blocker.hit ? 0.0 : 1.0;
    }
    if ((uTraceSkip & 1u) != 0u) {
        return 1.0;
    }
    return SampleShadowLayerWorld(startSlice + cascadeIndex, worldPos, normal, surfaceToLight);
}

float PointShadowVisibility(int startSlice, vec3 worldPos, vec3 normal, vec3 lightPos)
{
    vec3 toSurface = worldPos - lightPos;
    float distanceToLight = length(toSurface);
    if (distanceToLight <= 1e-5) {
        return 1.0;
    }

    vec3 absDir = abs(toSurface);
    int face = 0;
    if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
        face = toSurface.x >= 0.0 ? 0 : 1;
    } else if (absDir.y >= absDir.x && absDir.y >= absDir.z) {
        face = toSurface.y >= 0.0 ? 2 : 3;
    } else {
        face = toSurface.z >= 0.0 ? 4 : 5;
    }

    return SampleShadowLayerWorld(startSlice + face, worldPos, normal, -toSurface / distanceToLight);
}

float SpotShadowVisibility(int startSlice, vec3 worldPos, vec3 normal, vec3 lightPos)
{
    return SampleShadowLayerWorld(startSlice, worldPos, normal, SurfelSafeNormalize(lightPos - worldPos));
}

float EvaluateLightShadow(int type, LightData light, vec3 worldPos, vec3 normal, vec3 surfaceToLight)
{
    if (uEnableShadows == 0 || light.shadowData.z <= 0.5) {
        return 1.0;
    }

    int startSlice = int(light.shadowData.x + 0.5);
    int sliceCount = int(light.shadowData.y + 0.5);
    if (startSlice < 0 || sliceCount <= 0) {
        return 1.0;
    }

    if (type == 0) {
        return DirectionalShadowVisibility(startSlice, sliceCount, worldPos, normal, SurfelSafeNormalize(light.direction.xyz));
    }
    if (type == 1) {
        return PointShadowVisibility(startSlice, worldPos, normal, light.position.xyz);
    }
    if (type == 2) {
        return SpotShadowVisibility(startSlice, worldPos, normal, light.position.xyz);
    }

    return 1.0;
}

vec3 EvaluateHitDirectIrradiance(vec3 worldPos, vec3 normal)
{
    vec3 direct = vec3(0.0);
    uint lightCount = min(uLightCount, SURFEL_TRACE_MAX_LIGHTS);
    for (uint i = 0u; i < lightCount; ++i) {
        LightData light = lights[i];
        int type = int(light.position.w + 0.5);
        vec3 lightDir = SurfelSafeNormalize(light.direction.xyz);
        vec3 L = vec3(0.0);
        float attenuation = 1.0;

        if (type == 0) {
            L = -lightDir;
            attenuation = max(light.color.w, 0.0);
        } else {
            vec3 toLight = light.position.xyz - worldPos;
            float dist = length(toLight);
            float range = max(light.attenuation.w, 0.001);
            if (dist <= 0.001 || dist > range) {
                continue;
            }

            L = toLight / dist;
            vec3 att = light.attenuation.xyz;
            float invAtt = 1.0 / max(att.x + att.y * dist + att.z * dist * dist, 1e-4);
            float rangeFade = max(1.0 - pow(dist / range, 4.0), 0.0);
            attenuation = invAtt * rangeFade * rangeFade * max(light.color.w, 0.0);

            if (type == 2) {
                float theta = dot(L, -lightDir);
                float innerCos = light.spotData.x > 0.0 ? light.spotData.x : cos(radians(20.0));
                float outerCos = light.spotData.y > 0.0 ? light.spotData.y : cos(radians(30.0));
                outerCos = min(outerCos, innerCos - 0.001);
                if (theta < outerCos) {
                    continue;
                }
                attenuation *= smoothstep(outerCos, innerCos, theta);
            }
        }

        vec3 surfaceToLight = SurfelSafeNormalize(L);
        float nDotL = max(dot(normal, surfaceToLight), 0.0);
        if (nDotL <= 0.0) {
            continue;
        }
        float shadow = EvaluateLightShadow(type, light, worldPos, normal, surfaceToLight);
        direct += max(light.color.rgb, vec3(0.0)) * attenuation * nDotL * shadow;
    }

    if (lightCount == 0u) {
        vec3 lightToSurface = SurfelSafeNormalize(uDirectionalLightDirection);
        vec3 surfaceToLight = -lightToSurface;
        float nDotL = max(dot(normal, surfaceToLight), 0.0);
        direct = uDirectionalLightRadiance * nDotL;
    }

    return direct;
}


// Radiance of the sky along a ray that leaves the scene, scaled like the diffuse IBL
// (iblIntensity * diffuseIBLScale) so surfel irradiance and the deferred IBL agree.
vec3 EvaluateMissEnvironmentRadiance(vec3 rayDir)
{
    if (uSkyDiffuseScale <= 1e-5) {
        return vec3(0.0);
    }

    rayDir = SurfelSafeNormalize(rayDir);
    vec3 skyRadiance = uUseSkyEnvironmentMap != 0
        ? textureLod(uSkyEnvironmentMap, rayDir, SURFEL_ENVIRONMENT_MISS_MIP).rgb
        : uSkyRadiance;
    // No upper clamp: a sky miss is a physical sample. A clamp here biased the reference low for bright skies.
    skyRadiance = max(skyRadiance, vec3(0.0));
    if (any(isnan(skyRadiance)) || any(isinf(skyRadiance))) {
        return vec3(0.0);
    }
    return skyRadiance * uSkyDiffuseScale;
}


// Random numbers and cosine sampling for the path estimators (trace_rays.comp continuation, reference_gi.comp).
// PCG (RXS-M-XS) on a 32-bit state, uniform in [0, 1).
uint SurfelPathSeed(uint seed, uint frameIndex)
{
    return SurfelHash(seed ^ SurfelHash(frameIndex * 0x9e3779b9u + 0x7f4a7c15u));
}

float SurfelPathRandom(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) / 4294967296.0;
}

vec3 SurfelCosineDirection(vec3 normal, vec2 u)
{
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = SurfelSafeNormalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    float r = sqrt(u.x);
    float phi = 2.0 * SURFEL_PI * u.y;
    vec3 local = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
    return SurfelSafeNormalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

vec3 SurfelDecodeNormalOct(vec2 e)
{
    e = e * 2.0 - 1.0;
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        vec2 folded = (1.0 - abs(v.yx)) * sign(v.xy);
        v.xy = folded;
    }
    return SurfelSafeNormalize(v);
}

// ---------------------------------------------------------------- next-event estimation and MIS (plan section 14)
//
// Emitters are sampled by a shadow ray: pick an emitter with probability luminance * area / total, a point uniformly on
// its triangle. The solid-angle density of that sample at distance d with emitter cosine c is
// pdf = (luminance / total) * d^2 / c. A BSDF-sampled ray that hits an emitter gets the same density for the same
// direction, and the power heuristic weights each strategy: w = p^2 / (p^2 + q^2). Every emitter contribution is then
// counted once, through one of the two strategies, whatever the sampling density of the ray.

// Shadow test toward a point on an emitter (the emitter's own surface is excluded by the 0.999 margin).
bool SurfelEmitterOccluded(vec3 origin, vec3 target)
{
    vec3 delta = target - origin;
    float dist = length(delta);
    if (dist <= 1e-4) {
        return false;
    }
    SoftwareBVHHit hit = TraceSoftwareBVHAnyHit(origin, delta / dist, 1e-4, dist * 0.999);
    return hit.hit;
}

// Picks an emitter and a point on it. u.x selects, u.y and u.z place the point (uniform on the triangle).
bool SurfelSampleEmitterPoint(vec3 u, out vec3 position, out vec3 normal, out vec3 emission, out float luminance)
{
    if (uEmitterCount == 0u || !(uEmitterPowerTotal > 0.0)) {
        return false;
    }
    float target = clamp(u.x, 0.0, 0.999999) * uEmitterPowerTotal;
    uint lo = 0u;
    uint hi = uEmitterCount - 1u;
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        if (emitters[mid].cdf.x > target) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    SurfelEmitter e = emitters[lo];
    float s = sqrt(clamp(u.y, 0.0, 1.0));
    float b0 = 1.0 - s;
    float b1 = s * (1.0 - u.z);
    float b2 = s * u.z;
    position = b0 * e.v0.xyz + b1 * e.v1.xyz + b2 * e.v2.xyz;
    normal = e.normal.xyz;
    emission = e.emission.rgb;
    luminance = e.emission.w;
    return true;
}

// One emitter sample for a receiver at origin (already offset from its surface) with normal. Returns the irradiance
// estimate before MIS (emission * cos / pdf, zero when occluded), the sample direction and its solid-angle density.
bool SurfelEmitterIrradianceSample(vec3 origin, vec3 normal, vec3 u, out vec3 irradiance, out vec3 direction, out float lightPdf)
{
    irradiance = vec3(0.0);
    direction = normal;
    lightPdf = 0.0;
    if ((uTraceSkip & 4u) != 0u) {
        return false;
    }
    vec3 position;
    vec3 emitterNormal;
    vec3 emission;
    float luminance;
    if (!SurfelSampleEmitterPoint(u, position, emitterNormal, emission, luminance) || !(luminance > 0.0)) {
        return false;
    }
    vec3 delta = position - origin;
    float dist2 = dot(delta, delta);
    if (dist2 < 1e-8) {
        return false;
    }
    float dist = sqrt(dist2);
    direction = delta / dist;
    float cosReceiver = dot(normal, direction);
    // Emitters radiate from both faces, as the BSDF-hit emission does, so the emitter cosine is the absolute value.
    float cosEmitter = abs(dot(emitterNormal, direction));
    if (cosReceiver <= 0.0 || cosEmitter < 1e-4) {
        return false;
    }
    lightPdf = (luminance / uEmitterPowerTotal) * dist2 / cosEmitter;
    if (SurfelEmitterOccluded(origin, position)) {
        return true;
    }
    irradiance = emission * (cosReceiver / lightPdf);
    return true;
}

// Solid-angle density with which emitter sampling would have produced a BSDF-sampled hit on an emitter.
float SurfelEmitterHitPdf(float luminance, float dist, vec3 geometricNormal, vec3 direction)
{
    float cosEmitter = abs(dot(geometricNormal, direction));
    if (uEmitterCount == 0u || !(uEmitterPowerTotal > 0.0) || cosEmitter < 1e-4) {
        return 0.0;
    }
    return (luminance / uEmitterPowerTotal) * dist * dist / cosEmitter;
}

// Power heuristic: the weight of a sample drawn with density pa, when the other strategy has density pb.
float SurfelPowerWeight(float pa, float pb)
{
    float a = pa * pa;
    return a / max(a + pb * pb, 1e-30);
}

// Cached indirect irradiance at a path vertex, or false where the vertex has no coplanar surfel. trace_rays.comp
// implements it from the surfel cache; reference_gi.comp returns false (it keeps no surfels).
bool SurfelPathCachedIrradiance(vec3 position, vec3 normal, uint sourceSurfelID, uint seed, out vec3 irradiance);

// Where a path vertex came from, for queuing a world surfel when the cache misses there (plan section 17).
// eligible is false for skinned instances, for hits without a transform, and for the reference.
struct SurfelWorldSource
{
    uint transformID;
    uint materialID;
    bool eligible;
};

SurfelWorldSource SurfelWorldSourceFromHit(SoftwareBVHHit hit)
{
    SurfelWorldSource source;
    source.transformID = hit.transformID;
    source.materialID = 0u;
    // Transform ID 0 is INVALID_TRANSFORM_ID on the host side.
    source.eligible = !hit.skinned && hit.transformID != 0u && hit.transformID != SURFEL_INVALID_INDEX;
    return source;
}

// Queues a world surfel request at a path vertex the cache could not answer. trace_rays.comp implements it; the reference
// keeps no surfels and returns at once (reference_gi.comp).
void SurfelPathWorldRequest(vec3 position, vec3 normal, vec3 albedo, SurfelWorldSource source, uint depth);

const uint SURFEL_PATH_MAX_HITS = 32u;
const float SURFEL_PATH_RR_MAX_CONTINUE = 0.95;
const float SURFEL_PATH_ORIGIN_OFFSET = 0.004;

// How one path is run. The caller sets it; the reference keeps its own values (reference_gi.comp).
struct SurfelPathControl
{
    uint maxHits;             // hard depth cap; the residual albedo^cap is dropped
    uint rouletteStart;       // first vertex (after the hit) with Russian roulette, albedo based
    uint neeDepth;            // vertices below this get a next-event estimate of the emitters
    bool useCache;            // the surfel cache answers the first vertices
    // Weight of the path past its first hit. 0 stops the path at the first hit (its own direct, NEE and cache terms
    // still count). w > 0 continues it and multiplies the continuation by w; the caller takes the continuation with
    // probability 1/w, so the estimate stays unbiased.
    float continuationWeight;
    // Queue a world surfel request at each queued vertex the cache misses (trace only; the reference leaves it off).
    bool recordWorld;
    // Vertices below this depth use the BVH sun fallback for their direct light; deeper ones use the cascades only.
    uint sunBVHDepth;
    // Vertices below this depth read the cache (and queue a world surfel on a miss). Deeper vertices continue without it.
    uint cacheDepth;
};

// Radiance leaving a hit toward the path origin, by a path that continues from the hit. emissionWeight is the hit's own
// emission, already MIS weighted by the caller against the strategy that reached it. Each vertex adds its direct light
// and a next-event estimate of the emitters (MIS against cosine sampling), then the path continues by one cosine sample.
// It ends at a vertex with cached irradiance (added as E), at a sky miss (the miss radiance), or by Russian roulette from
// vertex rouletteStart on. Dividing by the continuation probability keeps it unbiased; the depth cap only drops a
// residual of albedo^cap. The sky enters only through these miss rays, so there is no separate sky-visibility term.
// The cache is optional (useCache): the independent reference never uses it.
// control.neeDepth limits the next-event samples to the vertices below it. Past that depth the emission of the next vertex
// counts at full weight (no MIS), which is still unbiased, and each deeper vertex costs one trace fewer.
vec3 SurfelPathRadiance(vec3 position, vec3 normal, vec3 albedo, vec3 emissionWeight, uint sourceSurfelID, uint seed,
                        SurfelPathControl control, SurfelWorldSource source)
{
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    vec3 emitted = emissionWeight;
    uint rng = seed;
    SurfelWorldSource vertexSource = source;
    for (uint depth = 0u; depth < control.maxHits; ++depth) {
        radiance += throughput * emitted;

        gSunShadowBVHHere = depth < control.sunBVHDepth;
        vec3 direct = EvaluateHitDirectIrradiance(position, normal);
        vec3 cached;
        // The cache is read at the first vertices only. Deeper vertices continue the path, which is unbiased and cheaper
        // than a cache walk at every bounce.
        if (control.useCache && depth < control.cacheDepth && SurfelPathCachedIrradiance(position, normal, depth == 0u ? sourceSurfelID : SURFEL_INVALID_INDEX,
                rng ^ (depth * 0x9e3779b9u), cached)) {
            // The cache already holds the emitters' light at this point, so no next-event sample here.
            return radiance + throughput * albedo * SURFEL_INV_PI * (direct + cached);
        }
        if (control.useCache && control.recordWorld && depth < control.cacheDepth) {
            // The cache could not answer this vertex: queue a world surfel here, so later rays stop at it.
            SurfelPathWorldRequest(position, normal, albedo, vertexSource, depth);
        }

        vec3 neeU = vec3(SurfelPathRandom(rng), SurfelPathRandom(rng), SurfelPathRandom(rng));
        vec3 neeIrradiance;
        vec3 neeDirection;
        float neePdf;
        vec3 nee = vec3(0.0);
        if (depth < control.neeDepth && SurfelEmitterIrradianceSample(position + normal * SURFEL_PATH_ORIGIN_OFFSET, normal, neeU,
                neeIrradiance, neeDirection, neePdf)) {
            float cosineDensity = max(dot(normal, neeDirection), 0.0) * SURFEL_INV_PI;
            nee = neeIrradiance * SurfelPowerWeight(neePdf, cosineDensity);
        }
        radiance += throughput * albedo * SURFEL_INV_PI * (direct + nee);
        if ((uTraceSkip & 256u) != 0u) {
            return radiance;
        }
        if (depth == 0u) {
            // The first hit's own terms are counted above. The path past it runs only when it has a weight (see
            // SurfelPathControl), and that weight multiplies everything below.
            if (control.continuationWeight <= 0.0) {
                return radiance;
            }
            throughput = albedo * control.continuationWeight;
        } else {
            throughput *= albedo;
        }

        if (depth >= control.rouletteStart) {
            float q = min(SURFEL_PATH_RR_MAX_CONTINUE, max(max(albedo.r, albedo.g), albedo.b));
            if (q <= 0.0 || SurfelPathRandom(rng) >= q) {
                return radiance;
            }
            throughput /= q;
        }

        vec3 direction = SurfelCosineDirection(normal, vec2(SurfelPathRandom(rng), SurfelPathRandom(rng)));
        SoftwareBVHHit next = TraceSoftwareBVH(position + normal * SURFEL_PATH_ORIGIN_OFFSET, direction, 0.0,
            SURFEL_TRACE_BVH_FAR_DISTANCE);
        if (!next.hit) {
            return radiance + throughput * EvaluateMissEnvironmentRadiance(direction);
        }

        vec3 nextNormal = next.normal;
        if (dot(nextNormal, direction) > 0.0) {
            nextNormal = -nextNormal;
        }
        vec3 nextEmission = max(next.material.emissive * next.material.emissiveStrength, vec3(0.0));
        float cosineDensity = max(dot(normal, direction), 0.0) * SURFEL_INV_PI;
        float hitPdf = SurfelEmitterHitPdf(SurfelLuminance(nextEmission), next.t, next.geometricNormal, direction);
        // The next-event sample at this vertex covered the emitters only when this vertex had one.
        float emissionMis = depth < control.neeDepth ? SurfelPowerWeight(cosineDensity, hitPdf) : 1.0;
        emitted = nextEmission * emissionMis;

        position = next.position;
        normal = SurfelSafeNormalize(nextNormal);
        albedo = clamp(next.material.albedo, vec3(0.0), vec3(1.0));
        vertexSource = SurfelWorldSourceFromHit(next);
    }
    return radiance;
}

#endif // NOX_SURFEL_SCENE_TRACE_GLSL
