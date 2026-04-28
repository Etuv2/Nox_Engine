#ifndef RT_SCENE_COMMON_GLSL
#define RT_SCENE_COMMON_GLSL

const float RT_SCENE_MAX_FLOAT = 1e30;
const float RT_SCENE_EPSILON = 1e-5;
const int RT_SCENE_MAX_BVH_STACK_SIZE = 64;

struct Material {
    vec3  albedo;
    float metallic;

    vec3  emissive;
    float roughness;

    vec3  specular;
    float emissiveStrength;

    vec3  specularColorFactor;
    float transmissionFactor;

    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float ior;
    float attenuationDistance;

    vec3  attenuationColor;
    float thicknessFactor;

    uint  materialID;
    float normalScale;
    float occlusionStrength;
    float specularFactor;

    float alpha;
    float alphaCutoff;
    uint  alphaMode;
    float padding0;
};

struct Triangle {
    vec3 v0; float pad0;
    vec3 v1; float pad1;
    vec3 v2; float pad2;
    vec3 n0; float pad3;
    vec3 n1; float pad4;
    vec3 n2; float pad5;
    vec3 center; float pad6;
    vec3 aabbMin; float pad7;
    vec3 aabbMax; float pad8;
    Material material;
};

struct BVHNode {
    vec3 minBounds; int child0;
    vec3 maxBounds; int child1;
    int  tri0, tri1, tri2, tri3;
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

struct Ray {
    vec3  origin;
    vec3  direction;
    float tMin;
    float tMax;
};

struct HitInfo {
    bool     hit;
    float    t;
    vec3     position;
    vec3     normal;
    Material material;
};

layout(std140, binding = 0) buffer TriangleBuffer { Triangle triangles[]; };
layout(std140, binding = 1) buffer BVHBuffer { BVHNode bvhNodes[]; };
layout(std430, binding = 31) readonly buffer RTInstanceBuffer { RTInstance rtInstances[]; };
layout(std430, binding = 32) readonly buffer RTInstanceNodeBuffer { RTInstanceNode rtInstanceNodes[]; };

#ifndef RT_SCENE_EXTERNAL_COUNTS
uniform int u_triangleCount;
uniform int u_bvhNodeCount;
uniform int u_rtInstanceCount;
uniform int u_rtInstanceNodeCount;
#endif

uint rtSceneHash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float rtSceneHash01(uint x)
{
    return float(rtSceneHash(x) & 0x00ffffffu) / 16777215.0;
}

float rtSceneAlphaDither(Ray ray, int triangleIndex)
{
    uvec3 o = floatBitsToUint(ray.origin);
    uvec3 d = floatBitsToUint(ray.direction);
    uint h = uint(triangleIndex) * 747796405u;
    h ^= o.x ^ rtSceneHash(o.y) ^ rtSceneHash(o.z);
    h ^= rtSceneHash(d.x) ^ rtSceneHash(d.y) ^ rtSceneHash(d.z);
    return rtSceneHash01(h);
}

bool rtSceneAcceptAlpha(Material mat, Ray ray, int triangleIndex)
{
    if (mat.alphaMode == 1u) {
        return mat.alpha >= mat.alphaCutoff;
    }
    if (mat.alphaMode == 2u) {
        return rtSceneAlphaDither(ray, triangleIndex) <= clamp(mat.alpha, 0.0, 1.0);
    }
    return true;
}

bool rayTriangleIntersect(Ray ray, Triangle tri, out float t, out vec3 barycentric)
{
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.direction, edge2);
    float a = dot(edge1, h);

    if (abs(a) < RT_SCENE_EPSILON) {
        return false;
    }

    float f = 1.0 / a;
    vec3 s = ray.origin - tri.v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) {
        return false;
    }

    vec3 q = cross(s, edge1);
    float v = f * dot(ray.direction, q);
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }

    t = f * dot(edge2, q);
    if (t < ray.tMin || t > ray.tMax) {
        return false;
    }

    barycentric = vec3(1.0 - u - v, u, v);
    return true;
}

bool rayAABBIntersect(Ray ray, vec3 minBounds, vec3 maxBounds)
{
    vec3 invDir = 1.0 / ray.direction;
    vec3 t0 = (minBounds - ray.origin) * invDir;
    vec3 t1 = (maxBounds - ray.origin) * invDir;

    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);

    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);

    return tNear <= tFar && tFar >= ray.tMin && tNear <= ray.tMax;
}

Ray rtSceneTransformRay(Ray ray, mat4 transform)
{
    Ray transformed;
    transformed.origin = (transform * vec4(ray.origin, 1.0)).xyz;
    transformed.direction = normalize((transform * vec4(ray.direction, 0.0)).xyz);
    transformed.tMin = ray.tMin;
    transformed.tMax = RT_SCENE_MAX_FLOAT;
    return transformed;
}

HitInfo traceBLAS(Ray ray, int nodeOffset, int triangleOffset)
{
    HitInfo hitInfo;
    hitInfo.hit = false;
    hitInfo.t = RT_SCENE_MAX_FLOAT;

    if (u_bvhNodeCount == 0 || nodeOffset < 0 || nodeOffset >= u_bvhNodeCount) {
        return hitInfo;
    }

    int stack[RT_SCENE_MAX_BVH_STACK_SIZE];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
        int localNodeIdx = stack[--stackPtr];
        int nodeIdx = nodeOffset + localNodeIdx;
        if (localNodeIdx < 0 || nodeIdx < 0 || nodeIdx >= u_bvhNodeCount) {
            continue;
        }

        BVHNode node = bvhNodes[nodeIdx];
        if (!rayAABBIntersect(ray, node.minBounds, node.maxBounds)) {
            continue;
        }

        bool isLeaf = (node.child0 == -1 && node.child1 == -1);
        if (isLeaf) {
            int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);
            for (int i = 0; i < 4; ++i) {
                if (triIndices[i] == -1) {
                    break;
                }

                int packedTriIndex = triangleOffset + triIndices[i];
                if (packedTriIndex < 0 || packedTriIndex >= u_triangleCount) {
                    continue;
                }

                Triangle tri = triangles[packedTriIndex];
                float t;
                vec3 barycentric;
                if (rayTriangleIntersect(ray, tri, t, barycentric)) {
                    Material mat = tri.material;
                    if (!rtSceneAcceptAlpha(mat, ray, packedTriIndex)) {
                        continue;
                    }

                    if (t < hitInfo.t) {
                        hitInfo.hit = true;
                        hitInfo.t = t;
                        hitInfo.position = ray.origin + ray.direction * t;
                        hitInfo.normal = normalize(tri.n0 * barycentric.x +
                                                   tri.n1 * barycentric.y +
                                                   tri.n2 * barycentric.z);
                        hitInfo.material = mat;
                        ray.tMax = t;
                    }
                }
            }
        } else {
            if (node.child1 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child1;
            }
            if (node.child0 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child0;
            }
        }
    }

    return hitInfo;
}

HitInfo traceInstanceScene(Ray ray)
{
    HitInfo bestHit;
    bestHit.hit = false;
    bestHit.t = RT_SCENE_MAX_FLOAT;

    if (u_rtInstanceNodeCount <= 0) {
        return bestHit;
    }

    int nodeStack[RT_SCENE_MAX_BVH_STACK_SIZE];
    int nodeStackPtr = 0;
    nodeStack[nodeStackPtr++] = 0;

    while (nodeStackPtr > 0 && nodeStackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
        int nodeIndex = nodeStack[--nodeStackPtr];
        if (nodeIndex < 0 || nodeIndex >= u_rtInstanceNodeCount) {
            continue;
        }

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

        RTInstance instance = rtInstances[instanceIndex];
        if (!rayAABBIntersect(ray, instance.boundsMin.xyz, instance.boundsMax.xyz)) {
            continue;
        }

        int nodeOffset = int(instance.metadata.x);
        int triangleOffset = int(instance.metadata.y);
        Ray localRay = rtSceneTransformRay(ray, instance.localFromWorld);
        HitInfo localHit = traceBLAS(localRay, nodeOffset, triangleOffset);
        if (!localHit.hit) {
            continue;
        }

        vec3 worldPosition = (instance.worldFromLocal * vec4(localHit.position, 1.0)).xyz;
        float worldT = length(worldPosition - ray.origin);
        if (worldT < ray.tMin || worldT > ray.tMax || worldT >= bestHit.t) {
            continue;
        }

        mat3 normalMatrix = transpose(mat3(instance.localFromWorld));
        bestHit.hit = true;
        bestHit.t = worldT;
        bestHit.position = worldPosition;
        bestHit.normal = normalize(normalMatrix * localHit.normal);
        bestHit.material = localHit.material;
        }
    }

    return bestHit;
}

HitInfo traceBVH(Ray ray)
{
    HitInfo hitInfo;
    hitInfo.hit = false;
    hitInfo.t = RT_SCENE_MAX_FLOAT;

    if (u_rtInstanceCount > 0) {
        return traceInstanceScene(ray);
    }

    if (u_bvhNodeCount == 0) {
        return hitInfo;
    }

    int stack[RT_SCENE_MAX_BVH_STACK_SIZE];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
        int nodeIdx = stack[--stackPtr];
        if (nodeIdx < 0 || nodeIdx >= u_bvhNodeCount) {
            continue;
        }

        BVHNode node = bvhNodes[nodeIdx];

        if (!rayAABBIntersect(ray, node.minBounds, node.maxBounds)) {
            continue;
        }

        bool isLeaf = (node.child0 == -1 && node.child1 == -1);

        if (isLeaf) {
            int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);

            for (int i = 0; i < 4; ++i) {
                if (triIndices[i] == -1) {
                    break;
                }
                if (triIndices[i] < 0 || triIndices[i] >= u_triangleCount) {
                    continue;
                }

                Triangle tri = triangles[triIndices[i]];
                float t;
                vec3 barycentric;

                if (rayTriangleIntersect(ray, tri, t, barycentric)) {
                    Material mat = tri.material;
                    if (!rtSceneAcceptAlpha(mat, ray, triIndices[i])) {
                        continue;
                    }

                    if (t < hitInfo.t) {
                        hitInfo.hit = true;
                        hitInfo.t = t;
                        hitInfo.position = ray.origin + ray.direction * t;
                        hitInfo.normal = normalize(tri.n0 * barycentric.x +
                                                   tri.n1 * barycentric.y +
                                                   tri.n2 * barycentric.z);
                        hitInfo.material = mat;
                        ray.tMax = t;
                    }
                }
            }
        } else {
            if (node.child1 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child1;
            }
            if (node.child0 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child0;
            }
        }
    }

    return hitInfo;
}

HitInfo traceBVHDebug(Ray ray, inout uint aabbIntersectCount, inout uint triIntersectCount)
{
    HitInfo hitInfo;
    hitInfo.hit = false;
    hitInfo.t = RT_SCENE_MAX_FLOAT;

    if (u_bvhNodeCount == 0) {
        return hitInfo;
    }

    int stack[RT_SCENE_MAX_BVH_STACK_SIZE];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
        int nodeIdx = stack[--stackPtr];
        if (nodeIdx < 0 || nodeIdx >= u_bvhNodeCount) {
            continue;
        }

        BVHNode node = bvhNodes[nodeIdx];

        if (!rayAABBIntersect(ray, node.minBounds, node.maxBounds)) {
            continue;
        }

        aabbIntersectCount++;

        bool isLeaf = (node.child0 == -1 && node.child1 == -1);

        if (isLeaf) {
            int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);

            for (int i = 0; i < 4; ++i) {
                if (triIndices[i] == -1) {
                    break;
                }
                if (triIndices[i] < 0 || triIndices[i] >= u_triangleCount) {
                    continue;
                }

                Triangle tri = triangles[triIndices[i]];
                float t;
                vec3 barycentric;
                triIntersectCount++;

                if (rayTriangleIntersect(ray, tri, t, barycentric)) {
                    Material mat = tri.material;
                    if (!rtSceneAcceptAlpha(mat, ray, triIndices[i])) {
                        continue;
                    }

                    if (t < hitInfo.t) {
                        hitInfo.hit = true;
                        hitInfo.t = t;
                        hitInfo.position = ray.origin + ray.direction * t;
                        hitInfo.normal = normalize(tri.n0 * barycentric.x +
                                                   tri.n1 * barycentric.y +
                                                   tri.n2 * barycentric.z);
                        hitInfo.material = mat;
                        ray.tMax = t;
                    }
                }
            }
        } else {
            if (node.child1 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child1;
            }
            if (node.child0 != -1 && stackPtr < RT_SCENE_MAX_BVH_STACK_SIZE) {
                stack[stackPtr++] = node.child0;
            }
        }
    }

    return hitInfo;
}

#endif
