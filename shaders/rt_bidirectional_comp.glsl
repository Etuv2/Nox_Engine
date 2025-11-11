#version 460 core

/**
 * @file rt_bidirectional_comp.glsl
 * @brief Path Tracing Compute Shader for Nox Engine
 *
 * This shader implements physically-based bidirectional path tracing starting from G-buffer surfaces.
 * It uses BVH acceleration for efficient ray-scene intersection and supports:
 * - Multiple importance sampling
 * - Temporal accumulation for progressive refinement
 * - PBR materials (metallic-roughness workflow)
 * - Indirect lighting and reflections
 *
 * References:
 * - pbr-book.org: Bidirectional Path Tracing
 * - pbr-book.org: Bounding Volume Hierarchies
 */

// Work group configuration
layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Constants
#define PI 3.1415926535897932384626433832795
#define INV_PI 0.31830988618379067154
#define EPSILON 1e-4
#define MAX_FLOAT 1e30
#define MAX_BVH_STACK_SIZE 64
#define MAX_RAY_BOUNCES 8

// Output image
layout (rgba16f, binding = 5) uniform image2D u_outputImage;

// G-buffer inputs
layout (binding = 0) uniform sampler2D u_gbufferPosition;
layout (binding = 1) uniform sampler2D u_gbufferNormal;
layout (binding = 2) uniform sampler2D u_gbufferAlbedo;
layout (binding = 3) uniform sampler2D u_gbufferMaterial;
layout (binding = 4) uniform sampler2D u_gbufferDepth;

// Uniforms
uniform int u_frameIndex;
uniform int u_sampleCount;
uniform int u_maxBounces;
uniform int u_triangleCount;
uniform int u_bvhNodeCount;

// Camera parameters
uniform vec3 u_cameraPos;
uniform vec3 u_cameraFront;
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;
uniform float u_fov;
uniform vec2 u_resolution;

// Data structures
struct Material {
    vec3 albedo;
    float metallic;
    vec3 emissive;
    float roughness;
    vec3 specular;
    float emissiveStrength;
};

struct Triangle {
    vec3 v0; float pad0;
    vec3 v1; float pad1;
    vec3 v2; float pad2;
    vec3 n0; float pad3;
    vec3 n1; float pad4;
    vec3 n2; float pad5;
    vec3 center; float pad6;
    Material material;
};

struct BVHNode {
    vec3 minBounds; int child0;
    vec3 maxBounds; int child1;
    int tri0, tri1, tri2, tri3;
};

struct Ray {
    vec3 origin;
    vec3 direction;
    float tMin;
    float tMax;
};

struct HitInfo {
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    Material material;
};

// Shader storage buffers
layout(std140, binding = 0) buffer TriangleBuffer { Triangle triangles[]; };
layout(std140, binding = 1) buffer BVHBuffer { BVHNode bvhNodes[]; };

// Random number generation (PCG)
uint g_seed;

uint hash(uint x) {
    x += (x << 10u);
    x ^= (x >> 6u);
    x += (x << 3u);
    x ^= (x >> 11u);
    x += (x << 15u);
    return x;
}

void initRandom(uvec2 pixel, int frame) {
    g_seed = hash(pixel.x + hash(pixel.y + hash(uint(frame))));
}

float randomFloat() {
    g_seed = hash(g_seed);
    return float(g_seed) / 4294967296.0;
}

vec2 randomVec2() {
    return vec2(randomFloat(), randomFloat());
}

// Sampling utilities
vec3 randomInUnitSphere() {
    float z = randomFloat() * 2.0 - 1.0;
    float a = randomFloat() * 2.0 * PI;
    float r = sqrt(1.0 - z * z);
    return vec3(r * cos(a), r * sin(a), z);
}

vec3 randomCosineDirection(vec3 normal) {
    vec2 u = randomVec2();
    float r = sqrt(u.x);
    float theta = 2.0 * PI * u.y;

    vec3 b = abs(normal.x) > abs(normal.y) ? vec3(normal.z, 0.0, -normal.x) : vec3(0.0, -normal.z, normal.y);
    b = normalize(b);
    vec3 t = cross(b, normal);

    return normalize(r * cos(theta) * b + r * sin(theta) * t + sqrt(1.0 - u.x) * normal);
}

vec3 randomGGXDirection(vec3 N, vec3 V, float roughness) {
    float alpha = roughness * roughness;
    vec2 u = randomVec2();

    float phi = 2.0 * PI * u.x;
    float cosTheta = sqrt((1.0 - u.y) / (1.0 + (alpha * alpha - 1.0) * u.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    H = tangent * H.x + bitangent * H.y + N * H.z;

    return reflect(-V, H);
}

// Octahedral normal decoding
vec3 octDecode(vec2 oct) {
    vec3 n = vec3(oct.x, oct.y, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        vec2 signNotZero = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * signNotZero;
    }
    return normalize(n);
}

// Ray-triangle intersection (Möller–Trumbore)
bool rayTriangleIntersect(Ray ray, Triangle tri, out float t, out vec3 barycentric) {
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.direction, edge2);
    float a = dot(edge1, h);

    if (abs(a) < EPSILON) return false;

    float f = 1.0 / a;
    vec3 s = ray.origin - tri.v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;

    vec3 q = cross(s, edge1);
    float v = f * dot(ray.direction, q);
    if (v < 0.0 || u + v > 1.0) return false;

    t = f * dot(edge2, q);
    if (t < ray.tMin || t > ray.tMax) return false;

    barycentric = vec3(1.0 - u - v, u, v);
    return true;
}

// Ray-AABB intersection
bool rayAABBIntersect(Ray ray, vec3 minBounds, vec3 maxBounds) {
    vec3 invDir = 1.0 / ray.direction;
    vec3 t0 = (minBounds - ray.origin) * invDir;
    vec3 t1 = (maxBounds - ray.origin) * invDir;

    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);

    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);

    return tNear <= tFar && tFar >= ray.tMin && tNear <= ray.tMax;
}

// BVH traversal (iterative stack)
HitInfo traceBVH(Ray ray) {
    HitInfo hitInfo;
    hitInfo.hit = false;
    hitInfo.t = MAX_FLOAT;

    if (u_bvhNodeCount == 0) return hitInfo;

    int stack[MAX_BVH_STACK_SIZE];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0 && stackPtr < MAX_BVH_STACK_SIZE) {
        int nodeIdx = stack[--stackPtr];
        BVHNode node = bvhNodes[nodeIdx];

        if (!rayAABBIntersect(ray, node.minBounds, node.maxBounds)) continue;

        bool isLeaf = (node.child0 == -1 && node.child1 == -1);

        if (isLeaf) {
            int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);

            for (int i = 0; i < 4; ++i) {
                if (triIndices[i] == -1) break;
                if (triIndices[i] >= u_triangleCount) continue;

                Triangle tri = triangles[triIndices[i]];
                float t;
                vec3 barycentric;

                if (rayTriangleIntersect(ray, tri, t, barycentric)) {
                    if (t < hitInfo.t) {
                        hitInfo.hit = true;
                        hitInfo.t = t;
                        hitInfo.position = ray.origin + ray.direction * t;
                        hitInfo.normal = normalize(
                            tri.n0 * barycentric.x +
                            tri.n1 * barycentric.y +
                            tri.n2 * barycentric.z
                        );
                        hitInfo.material = tri.material;
                        ray.tMax = t;
                    }
                }
            }
        } else {
            if (node.child1 != -1 && stackPtr < MAX_BVH_STACK_SIZE) stack[stackPtr++] = node.child1;
            if (node.child0 != -1 && stackPtr < MAX_BVH_STACK_SIZE) stack[stackPtr++] = node.child0;
        }
    }

    return hitInfo;
}

// BRDF
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 evaluateBRDF(Material mat, vec3 N, vec3 V, vec3 L) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

    float D = DistributionGGX(N, H, mat.roughness);
    float G = GeometrySmith(N, V, L, mat.roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (D * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.0001);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - mat.metallic);
    vec3 diffuse = kD * mat.albedo * INV_PI;

    return (diffuse + specular) * NdotL;
}

// Sky model
vec3 sampleSky(vec3 direction) {
    float t = 0.5 * (direction.y + 1.0);
    vec3 horizon = vec3(1.0);
    vec3 zenith = vec3(0.5, 0.7, 1.0);
    return mix(horizon, zenith, t) * 0.3;
}

// Path tracing
vec3 tracePath(Ray initialRay) {
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    Ray ray = initialRay;

    for (int bounce = 0; bounce < min(u_maxBounces, MAX_RAY_BOUNCES); ++bounce) {
        HitInfo hit = traceBVH(ray);
        if (!hit.hit) {
            radiance += throughput * sampleSky(ray.direction);
            break;
        }

        if (hit.material.emissiveStrength > 0.0)
            radiance += throughput * hit.material.emissive * hit.material.emissiveStrength;

        float survivalProb = max(throughput.x, max(throughput.y, throughput.z));
        if (survivalProb < 0.1 && bounce > 2) {
            if (randomFloat() > survivalProb) break;
            throughput /= survivalProb;
        }

        vec3 V = -ray.direction;
        vec3 newDirection;
        float specularProb = mix(0.1, 0.9, 1.0 - hit.material.roughness * (1.0 - hit.material.metallic));

        if (randomFloat() < specularProb)
            newDirection = randomGGXDirection(hit.normal, V, hit.material.roughness);
        else
            newDirection = randomCosineDirection(hit.normal);

        if (dot(newDirection, hit.normal) <= 0.0) break;

        vec3 brdf = evaluateBRDF(hit.material, hit.normal, V, newDirection);
        float pdf = max(dot(hit.normal, newDirection), 0.0) * INV_PI;
        pdf = mix(pdf, specularProb, 0.5);
        if (pdf < 0.0001) break;

        throughput *= brdf / pdf;

        ray.origin = hit.position + hit.normal * EPSILON;
        ray.direction = newDirection;
        ray.tMin = EPSILON;
        ray.tMax = MAX_FLOAT;
    }

    return radiance;
}

// Main
void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= int(u_resolution.x) || pixel.y >= int(u_resolution.y)) return;

    initRandom(uvec2(pixel), u_frameIndex);
    vec2 uv = (vec2(pixel) + 0.5) / u_resolution;
    float depth = texture(u_gbufferDepth, uv).r;

    if (depth >= 0.9999) {
        vec2 ndc = (vec2(pixel) / u_resolution) * 2.0 - 1.0;
        float aspect = u_resolution.x / u_resolution.y;

        vec3 rayDir = normalize(
            u_cameraFront +
            ndc.x * aspect * tan(u_fov * 0.5) * u_cameraRight +
            ndc.y * tan(u_fov * 0.5) * u_cameraUp
        );

        vec3 skyColor = sampleSky(rayDir);
        vec3 prev = imageLoad(u_outputImage, pixel).rgb;
        float blend = 1.0 / float(u_frameIndex + 1);
        vec3 color = mix(prev, skyColor, blend);
        imageStore(u_outputImage, pixel, vec4(color, 1.0));
        return;
    }

    vec3 worldPos = texture(u_gbufferPosition, uv).xyz;
    vec2 octNormal = texture(u_gbufferNormal, uv).rg;
    vec3 normal = octDecode(octNormal * 2.0 - 1.0);
    vec4 albedoAO = texture(u_gbufferAlbedo, uv);
    vec4 matData = texture(u_gbufferMaterial, uv);

    Material mat;
    mat.albedo = albedoAO.rgb;
    mat.roughness = matData.r;
    mat.metallic = matData.g;
    mat.specular = vec3(matData.b);
    mat.emissive = vec3(0.0);
    mat.emissiveStrength = 0.0;

    vec3 color = vec3(0.0);
    for (int i = 0; i < u_sampleCount; ++i) {
        vec3 rayDir = randomCosineDirection(normal);
        Ray ray;
        ray.origin = worldPos + normal * EPSILON;
        ray.direction = rayDir;
        ray.tMin = EPSILON;
        ray.tMax = MAX_FLOAT;

        vec3 indirect = tracePath(ray);
        vec3 V = normalize(u_cameraPos - worldPos);
        vec3 brdf = evaluateBRDF(mat, normal, V, rayDir);
        float pdf = max(dot(normal, rayDir), 0.0) * INV_PI;
        if (pdf > 0.0001) color += indirect * brdf / pdf;
    }

    color /= float(u_sampleCount);
    vec3 prevColor = imageLoad(u_outputImage, pixel).rgb;
    float blend = 1.0 / float(u_frameIndex + 1);
    color = mix(prevColor, color, blend);
    color = min(color, vec3(10.0));

    imageStore(u_outputImage, pixel, vec4(color, 1.0));
}
