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

struct SoftwareBVHHit
{
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    RTMaterial material;
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

SoftwareBVHHit TraceSoftwareBLAS(vec3 origin, vec3 direction, float tMin, float tMax, int rootNodeOffset, int triangleOffset)
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

SoftwareBVHHit TraceSoftwareTLAS(vec3 origin, vec3 direction, float tMin, float tMax)
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
                int(instance.metadata.y));
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
        return TraceSoftwareTLAS(origin, direction, tMin, tMax);
    }

    return TraceSoftwareBLAS(origin, direction, tMin, tMax, 0, 0);
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

    return SampleShadowLayerWorld(startSlice + cascadeIndex, worldPos, normal, -lightDirection);
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

#endif // NOX_SURFEL_SCENE_TRACE_GLSL
