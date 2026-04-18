#version 460 core

/**
 * @file rt_bidirectional_comp.glsl
 * @brief Path Tracing Compute Shader for Nox Engine
 *
 * Implements physically-based bidirectional path tracing starting from G-buffer surfaces.
 * Uses BVH acceleration for efficient ray-scene intersection and supports:
 * - Multiple importance sampling
 * - Temporal accumulation for progressive refinement
 * - PBR materials (canonical metallic-roughness workflow)
 * - Indirect lighting and reflections
 *
 * References:
 * - pbr-book.org: Bidirectional Path Tracing
 * - pbr-book.org: Bounding Volume Hierarchies
 */

// Include shared PBR functions
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"

// Workgroup
layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// RT-specific constants (not in pbr_common)
const float MAX_FLOAT = 1e30;
const int MAX_BVH_STACK_SIZE = 64;
const int MAX_RAY_BOUNCES = 8;

// Note: PI, TAU, INV_PI, EPSILON, DIELECTRIC_F0 are in pbr_common.glsl
// Note: CalculateDiffuseAlbedo, SpecularOcclusion, DecodeNormalOct, 
//       DistributionGGX, GeometrySmith, FresnelSchlick, ImportanceSampleGGX
//       are all defined in pbr_common.glsl


/* Output image */

layout (rgba16f, binding = 5) uniform image2D u_outputImage;


// G-buffer inputs (packed layout)

// RT0: Oct normal (RG) + Roughness (B) + Metallic (A)
layout (binding = 0) uniform sampler2D u_gbufferPackedNormalRM;
// RT1: Albedo (RGB) + Occlusion (A)
layout (binding = 1) uniform sampler2D u_gbufferAlbedoAO;
// RT2: Specular F0 (RGB) + Emissive strength (A)
layout (binding = 2) uniform sampler2D u_gbufferSpecularF0;
// Depth buffer
layout (binding = 3) uniform sampler2D u_gbufferDepth;
// RT3: Material ID (uint8, opaque PBR or transmission)
layout (binding = 8) uniform usampler2D u_gbufferMaterialID;
// RT4: Emissive color (RGB)
layout (binding = 9) uniform sampler2D u_gbufferEmissive;
// RT6: Clearcoat factor + roughness
layout (binding = 10) uniform sampler2D u_gbufferClearCoat;
// RT7: Principled extras: transmission (R), IOR (G), reserved (BA)
layout (binding = 11) uniform sampler2D u_gbufferPrincipledParams;


// IBL Environment maps
layout (binding = 4) uniform samplerCube u_environmentMap;  // HDR environment
layout (binding = 6) uniform samplerCube u_irradianceMap;  // Diffuse IBL
layout (binding = 7) uniform samplerCube u_prefilteredMap; // Specular IBL


// Uniforms

uniform int   u_frameIndex;
uniform int   u_sampleCount;
uniform int   u_maxBounces;
uniform int   u_triangleCount;
uniform int   u_bvhNodeCount;

// Light system
uniform int   u_lightCount;
uniform bool  u_enableNEE;          // Next Event Estimation
uniform bool  u_enableMIS;          // Multiple Importance Sampling
uniform float u_misWeight;        // MIS power-heuristic weight (reserved)

// BVH Debug Visualization
uniform bool  u_displayBVH;          // Enable BVH visualization mode
uniform bool  u_displayMultipleBVHLayers;   // Show multiple layers
uniform int   u_BVHLayerToDisplay;  // Which layer to show
uniform int   u_heatmapColorLimit;  // Max value for heatmap color scale

// IBL Environment
uniform bool  u_enableIBL;         // Enable IBL skybox sampling
uniform float u_iblIntensity;    // IBL intensity multiplier

// Camera
uniform vec3  u_cameraPos;
uniform vec3  u_cameraFront;
uniform vec3  u_cameraRight;
uniform vec3  u_cameraUp;
uniform float u_fov;
uniform vec2  u_resolution;

// Matrices for position reconstruction
uniform mat4  u_invView;
uniform mat4  u_invProj;
uniform float u_cameraNear;
uniform float u_cameraFar;


// Data structures

/**
 * @struct Material
 * @brief Extended PBR material matching RTStructures.h layout (128 bytes)
 * 
 * Supports:
 * - Metallic-roughness workflow (materialID = 0)
 * - Transmissive/glass materials (materialID = 2)
 * - Alpha transparency (mask and blend modes)
 */
struct Material {
	// Row 0: Albedo + Metallic
	vec3  albedo;
	float metallic;
	
	// Row 1: Emissive + Roughness
	vec3  emissive;
	float roughness;
	
	// Row 2: Specular F0 + Emissive Strength
	vec3  specular;
	float emissiveStrength;
	
	// Row 3: Specular Color Factor + Transmission
	vec3  specularColorFactor;
	float transmissionFactor;
	
	// Row 4: Clearcoat + IOR
	float clearcoatFactor;
	float clearcoatRoughnessFactor;
	float ior;
	float attenuationDistance;
	
	// Row 5: Volume attenuation + thickness
	vec3  attenuationColor;
	float thicknessFactor;
	
	// Row 6: Material Flags
	uint  materialID;       // 0=Standard PBR, 2=Transmission
	float normalScale;
	float occlusionStrength;
	float specularFactor;
	
	// Row 7: Alpha/Transparency
	float alpha;            // Base alpha value [0,1]
	float alphaCutoff;      // Cutoff for MASK mode
	uint  alphaMode;        // 0=OPAQUE, 1=MASK, 2=BLEND
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
	vec3 aabbMin; float pad7;  // Precomputed AABB min bounds (offset 112)
	vec3 aabbMax; float pad8;  // Precomputed AABB max bounds (offset 128)
	Material material;         // Material starts at offset 144, size 128 bytes, total 272 bytes
};

struct BVHNode {
	vec3 minBounds; int child0;
	vec3 maxBounds; int child1;
	int  tri0, tri1, tri2, tri3;
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

// Matches RTStructures.h
struct RTLightData {
	vec4 position;     // xyz = pos, w = type (0=dir, 1=point, 2=spot, 3=area)
	vec4 direction;    // xyz = dir (normalized), w = unused
	vec4 color;        // xyz = color, w = intensity
	vec4 attenuation;  // xyz = const/linear/quadratic, w = range
	vec4 shadowData;   // x = startSlice, y = sliceCount, z = castsShadows, w = pcss
	vec4 spotData;     // x = inner cos, y = outer cos
	vec4 areaData;     // xyz = size (area lights), w = reserved
	vec4 sampling;     // x = PDF weight, y = solid angle, z/w = reserved
};

// Buffers

layout(std140, binding = 0) buffer TriangleBuffer  { Triangle  triangles[]; };
layout(std140, binding = 1) buffer BVHBuffer { BVHNode bvhNodes[];   };
layout(std430, binding = 2) buffer LightBuffer     { RTLightData lights[];   };


// RNG (PCG-based with better quality)

uint g_seed;

vec3 tracePath(Ray initialRay);
vec3 sampleSky(vec3 direction);
vec3 reconstructWorldPosition(vec2 uv, float depth);
vec3 DecodeNormalOct8(vec2 oct);
vec3 evaluateBRDF(Material mat, vec3 N, vec3 V, vec3 L);
vec3 randomCosineDirection(vec3 normal);
vec3 ResolveMaterialF0(Material mat);

// PCG hash: https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint pcg_hash(uint input_) {
	uint state = input_ * 747796405u + 2891336453u;
	uint word = ((state >> 27u) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// Initialize RNG seed with better distribution
void initRandom(uvec2 pixel, int frame) {
	// Combine pixel and frame with different primes for decorrelation
	uint seed = pixel.x * 1973u + pixel.y * 9277u + uint(frame) * 26699u;
	g_seed = pcg_hash(seed);
}

// Generate a random float in [0, 1) with better uniformity
float randomFloat() {
	g_seed = pcg_hash(g_seed);
	return float(g_seed) * (1.0 / 4294967296.0);
}

// Generate stratified random in [0,1) within a cell
float randomFloatStratified(int sampleIndex, int totalSamples) {
	float cellSize = 1.0 / float(totalSamples);
	float base = float(sampleIndex) * cellSize;
	return base + randomFloat() * cellSize;
}

// Generate a random vec2 with components in [0, 1)
vec2 randomVec2() {
	return vec2(randomFloat(), randomFloat());
}

// Radical inverse for low-discrepancy sequence (Halton) : https://www.pbr-book.org/3ed-2018/Sampling_and_Reconstruction/The_Halton_Sampler
float radicalInverse(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

// Hammersley sequence for better sample distribution
vec2 hammersleyVec2(uint i, uint N) {
	return vec2(float(i) / float(N), radicalInverse(i));
}
// Sampling utilities
vec3 randomInUnitSphere() {
	float z = randomFloat() * 2.0 - 1.0;
	float a = randomFloat() * TAU;
	float r = sqrt(1.0 - z * z);
	return vec3(r * cos(a), r * sin(a), z);
}

// Cosine-weighted hemisphere sampling around normal
vec3 randomCosineDirection(vec3 normal) {
	vec2 u = randomVec2();
	float r = sqrt(u.x);
	float theta = TAU * u.y;

	vec3 b = (abs(normal.x) > abs(normal.y))
		   ? vec3(normal.z, 0.0, -normal.x)
		   : vec3(0.0, -normal.z, normal.y);
	b = normalize(b);
	vec3 t = cross(b, normal);

	return normalize(r * cos(theta) * b +
					 r * sin(theta) * t +
					 sqrt(1.0 - u.x) * normal);
}
// GGX importance sampling - improved version for low roughness metals
// Uses proper half-vector distribution and handles near-mirror surfaces correctly
vec3 randomGGXDirection(vec3 N, vec3 V, float roughness) {
	vec3 L = ImportanceSampleGGX(randomVec2(), N, V, ClampPerceptualRoughness(roughness));
	if (dot(L, N) <= 0.0) {
		L = reflect(-V, N);
	}
	return L;
}


// Octahedral normal decoding (from G-buffer)
// Now properly remaps from [0,1] to [-1,1] before decoding
vec3 DecodeNormalOct8(vec2 e) {
	// Use the shared function which handles the remap correctly
	return DecodeNormalOct(e);
}

// Position reconstruction from depth

vec3 reconstructWorldPosition(vec2 uv, float depth) {
	vec2 ndc = uv * 2.0 - 1.0;
	vec4 clipPos = vec4(ndc, depth * 2.0 - 1.0, 1.0);
	vec4 viewPos = u_invProj * clipPos;
	viewPos /= viewPos.w;
	vec4 worldPos = u_invView * viewPos;
	return worldPos.xyz;
}


// Ray / Triangle (Möller–Trumbore)

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


// Ray / AABB

bool rayAABBIntersect(Ray ray, vec3 minBounds, vec3 maxBounds) {
	vec3 invDir = 1.0 / ray.direction;
	vec3 t0 = (minBounds - ray.origin) * invDir;
	vec3 t1 = (maxBounds - ray.origin) * invDir;

	vec3 tMin = min(t0, t1);
	vec3 tMax = max(t0, t1);

	float tNear = max(max(tMin.x, tMin.y), tMin.z);
	float tFar  = min(min(tMax.x, tMax.y), tMax.z);

	return tNear <= tFar && tFar >= ray.tMin && tNear <= ray.tMax;
}


// BVH traversal (iterative stack) with alpha handling

HitInfo traceBVH(Ray ray) {
	HitInfo hitInfo;
	hitInfo.hit = false;
	hitInfo.t   = MAX_FLOAT;

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
					// Alpha handling for masked/blended materials
					Material mat = tri.material;
					
					// Alpha MASK mode: discard if below cutoff
					if (mat.alphaMode == 1u) {
						if (mat.alpha < mat.alphaCutoff) {
							continue; // Skip this triangle, ray continues
						}
					}
					// Alpha BLEND mode: stochastic alpha test
					else if (mat.alphaMode == 2u) {
						if (randomFloat() > mat.alpha) {
							continue; // Probabilistic pass-through
						}
					}
					// OPAQUE mode (alphaMode == 0): always accept hit
					
					if (t < hitInfo.t) {
						hitInfo.hit = true;
						hitInfo.t   = t;
						hitInfo.position = ray.origin + ray.direction * t;
						hitInfo.normal   = normalize(tri.n0 * barycentric.x +
													 tri.n1 * barycentric.y +
													 tri.n2 * barycentric.z);
						hitInfo.material = mat;
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

// BVH traversal with debug counters and alpha handling
HitInfo traceBVHDebug(Ray ray, inout uint aabbIntersectCount, inout uint triIntersectCount) {
	HitInfo hitInfo;
	hitInfo.hit = false;
	hitInfo.t   = MAX_FLOAT;

	if (u_bvhNodeCount == 0) return hitInfo;

	int stack[MAX_BVH_STACK_SIZE];
	int stackPtr = 0;
	stack[stackPtr++] = 0;

	while (stackPtr > 0 && stackPtr < MAX_BVH_STACK_SIZE) {
		int nodeIdx = stack[--stackPtr];
		BVHNode node = bvhNodes[nodeIdx];

		if (!rayAABBIntersect(ray, node.minBounds, node.maxBounds)) continue;
		
		aabbIntersectCount++;

		bool isLeaf = (node.child0 == -1 && node.child1 == -1);

		if (isLeaf) {
			int triIndices[4] = int[4](node.tri0, node.tri1, node.tri2, node.tri3);

			for (int i = 0; i < 4; ++i) {
				if (triIndices[i] == -1) break;
				if (triIndices[i] >= u_triangleCount) continue;

				Triangle tri = triangles[triIndices[i]];
				float t;
				vec3 barycentric;

				triIntersectCount++;
				
				if (rayTriangleIntersect(ray, tri, t, barycentric)) {
					// Alpha handling
					Material mat = tri.material;
					if (mat.alphaMode == 1u && mat.alpha < mat.alphaCutoff) continue;
					if (mat.alphaMode == 2u && randomFloat() > mat.alpha) continue;
					
					if (t < hitInfo.t) {
						hitInfo.hit = true;
						hitInfo.t   = t;
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
			if (node.child1 != -1 && stackPtr < MAX_BVH_STACK_SIZE) stack[stackPtr++] = node.child1;
			if (node.child0 != -1 && stackPtr < MAX_BVH_STACK_SIZE) stack[stackPtr++] = node.child0;
		}
	}

	return hitInfo;
}

// Calculate refraction direction using Snell's law
// Returns reflection direction if total internal reflection occurs
vec3 calculateRefraction(vec3 I, vec3 N, float materialIOR) {
	bool totalInternalReflection = false;
	return ComputeRefractionDirection(I, N, materialIOR, totalInternalReflection);
}

// Evaluate BRDF for canonical metallic-roughness workflow (standard PBR + transmission)
vec3 evaluateBRDF_Canonical(Material mat, vec3 N, vec3 V, vec3 L) {
	vec3 F0 = ResolveMaterialF0(mat);
	return EvaluateCanonicalBRDF(
		N, V, L,
		mat.albedo, mat.metallic, mat.roughness, F0, mat.transmissionFactor,
		mat.clearcoatFactor, mat.clearcoatRoughnessFactor
	);
}

// Evaluate BRDF for canonical workflow with separate diffuse/specular outputs
// This matches deferred lighting's approach where AO is applied separately to diffuse and specular
void evaluateBRDF_Canonical_Separated(Material mat, vec3 N, vec3 V, vec3 L,
	out vec3 diffuseOut, out vec3 specularOut) {
	vec3 F0 = ResolveMaterialF0(mat);
	EvaluateCanonicalBRDFSeparated(
		N, V, L,
		mat.albedo, mat.metallic, mat.roughness, F0, mat.transmissionFactor,
		mat.clearcoatFactor, mat.clearcoatRoughnessFactor,
		diffuseOut, specularOut
	);
}

// Backwards-compatible metallic-roughness wrappers now map to the canonical material contract.
vec3 evaluateBRDF_MetallicRoughness(Material mat, vec3 N, vec3 V, vec3 L) {
	return evaluateBRDF_Canonical(mat, N, V, L);
}

void evaluateBRDF_MetallicRoughness_Separated(Material mat, vec3 N, vec3 V, vec3 L,
	out vec3 diffuseOut, out vec3 specularOut) {
	evaluateBRDF_Canonical_Separated(mat, N, V, L, diffuseOut, specularOut);
}

// Evaluate BRDF for transmissive materials (glass, etc.)
vec3 evaluateBRDF_Transmissive(Material mat, vec3 N, vec3 V, vec3 L, out float transmission) {
	vec3 F0 = ResolveMaterialF0(mat);
	float NdotV = max(dot(N, V), 0.0);
	transmission = ComputeTransmissionWeight(mat.transmissionFactor, NdotV, F0);
	return EvaluateCanonicalBRDF(
		N, V, L,
		mat.albedo, mat.metallic, mat.roughness, F0, mat.transmissionFactor,
		mat.clearcoatFactor, mat.clearcoatRoughnessFactor
	);
}

// Main BRDF evaluation with material routing based on materialID
vec3 evaluateBRDF(Material mat, vec3 N, vec3 V, vec3 L) {
	if (mat.materialID == 2u) {
		float transmission;
		return evaluateBRDF_Transmissive(mat, N, V, L, transmission);
	}
	return evaluateBRDF_Canonical(mat, N, V, L);
}


// Light sampling

vec3 sampleDirectionalLight(RTLightData light, vec3 hitPos, out vec3 lightDir, out float pdf) {
	lightDir = normalize(-light.direction.xyz);
	pdf = 1.0; // delta
	return light.color.xyz * light.color.w;
}

vec3 samplePointLight(RTLightData light, vec3 hitPos, out vec3 lightDir, out float pdf) {
	vec3 lightPos = light.position.xyz;
	vec3 toLight  = lightPos - hitPos;
	float distance = length(toLight);
	lightDir = toLight / max(distance, EPSILON);

	// Match deferred renderer's attenuation calculation
	vec3 att_coeffs = light.attenuation.xyz;
	float range = light.attenuation.w;
	float intensity = light.color.w;
	
	// Inverse attenuation (standard polynomial falloff)
	float attDenom = att_coeffs.x + att_coeffs.y * distance + att_coeffs.z * distance * distance;
	float inv = 1.0 / max(attDenom, 1.0);
	
	// Range falloff factor (smooth cutoff at max range)
	float rf = 1.0 - pow(distance / range, 4.0);
	rf = max(rf, 0.0);
	rf = rf * rf;  // Square for smoother falloff
	
	float attenuation = inv * rf * intensity;

	// PDF for light sampling (uniform over solid angle approximation)
	pdf = 1.0;  // Delta distribution for point lights (deterministic direction)
	
	return light.color.xyz * attenuation;
}

vec3 sampleSpotLight(RTLightData light, vec3 hitPos, out vec3 lightDir, out float pdf) {
	vec3 lightPos = light.position.xyz;
	vec3 toLight  = lightPos - hitPos;
	float distance = length(toLight);
	lightDir = toLight / max(distance, EPSILON);

	vec3  spotDir   = normalize(light.direction.xyz);
	float cosTheta  = dot(-lightDir, spotDir);
	float innerCos  = light.spotData.x;
	float outerCos  = light.spotData.y;
	
	// Match deferred renderer's spot cone calculation
	outerCos = min(outerCos, innerCos - 0.001);
	float eps = max(innerCos - outerCos, 0.001);
	float cone = clamp((cosTheta - outerCos) / eps, 0.0, 1.0);
	cone = cone * cone * (3.0 - 2.0 * cone);  // Smoothstep

	// Match deferred renderer's attenuation calculation
	vec3 att_coeffs = light.attenuation.xyz;
	float range = light.attenuation.w;
	float intensity = light.color.w;
	
	// Inverse attenuation (standard polynomial falloff)
	float attDenom = att_coeffs.x + att_coeffs.y * distance + att_coeffs.z * distance * distance;
	float inv = 1.0 / max(attDenom, 1.0);
	
	// Range falloff factor (smooth cutoff at max range)
	float rf = 1.0 - pow(distance / range, 4.0);
	rf = max(rf, 0.0);
	rf = rf * rf;  // Square for smoother falloff
	
	float attenuation = inv * rf * intensity * cone;

	// PDF for light sampling (uniform over solid angle approximation)
	pdf = 1.0;  // Delta distribution for spot lights (deterministic direction)
	
	return light.color.xyz * attenuation;
}

int sampleLightIndex() {
	if (u_lightCount == 0) return -1;
	if (u_lightCount == 1) return 0;
	return int(randomFloat() * float(u_lightCount)) % u_lightCount;
}

vec3 sampleLight(int lightIndex, vec3 hitPos, out vec3 lightDir, out float pdf) {
	if (lightIndex < 0 || lightIndex >= u_lightCount) {
		pdf = 0.0;
		return vec3(0.0);
	}

	RTLightData light = lights[lightIndex];
	int lightType = int(light.position.w);

	if (lightType == 0) return sampleDirectionalLight(light, hitPos, lightDir, pdf);
	if (lightType == 1) return samplePointLight      (light, hitPos, lightDir, pdf);
	if (lightType == 2) return sampleSpotLight       (light, hitPos, lightDir, pdf);

	pdf = 0.0;
	return vec3(0.0);
}

bool traceShadowRay(vec3 origin, vec3 direction, float maxDist) {
	Ray shadowRay;
	shadowRay.origin    = origin + direction * EPSILON;
	shadowRay.direction = direction;
	shadowRay.tMin      = EPSILON;
	shadowRay.tMax      = maxDist - EPSILON;

	HitInfo hit = traceBVH(shadowRay);
	return !hit.hit;
}

// MIS (power heuristic)

float misPowerHeuristic(float pdfA, float pdfB) {
	float a = pdfA * pdfA;
	float b = pdfB * pdfB;
	return a / (a + b + 0.0001);
}

// Environment sampling for NEE
vec3 sampleEnvironmentDirect(vec3 hitPos, vec3 normal, vec3 viewDir, Material mat, out vec3 lightDir, out float pdf) {
	if (!u_enableIBL) {
		pdf = 0.0;
		return vec3(0.0);
	}
	
	// Importance sample environment map
	// For now, use cosine-weighted hemisphere sampling
	// TODO: Implement proper importance sampling with alias table
	lightDir = randomCosineDirection(normal);
	pdf = max(dot(normal, lightDir), 0.0) * INV_PI;
	
	if (pdf < 0.0001) {
		return vec3(0.0);
	}
	
	// Sample environment
	vec3 envRadiance = texture(u_environmentMap, lightDir).rgb * u_iblIntensity;
	
	return envRadiance;
}

vec3 evaluateDirectLightingMIS(vec3 hitPos, vec3 normal, vec3 viewDir, Material mat, 
	float diffuseAO, float specularAO) {
	vec3 directLight = vec3(0.0);
	
	// Sample lights
	if (u_lightCount > 0) {
		int lightIdx = sampleLightIndex();
		if (lightIdx >= 0) {
			vec3 lightDir;
			float lightPDF;
			vec3 radiance = sampleLight(lightIdx, hitPos, lightDir, lightPDF);

			if (lightPDF > 0.0 && dot(normal, lightDir) > 0.0) {
				float maxDist = (int(lights[lightIdx].position.w) == 0) ? 10000.0 : lights[lightIdx].attenuation.w;
				if (traceShadowRay(hitPos, lightDir, maxDist)) {
					// Evaluate BRDF with separated components for proper AO application
					vec3 diffuseBRDF, specularBRDF;
					evaluateBRDF_Canonical_Separated(mat, normal, viewDir, lightDir,
						diffuseBRDF, specularBRDF);
					
					// Apply AO: diffuse uses diffuseAO, specular uses specularAO
					// This matches deferred lighting: (diffuse * diffuseAO + specular)
					vec3 brdfWithAO = diffuseBRDF * diffuseAO + specularBRDF;

					// For delta distributions (point/spot/directional lights), PDF = 1.0
					// The radiance already includes full attenuation, so we don't divide by PDF
					// We only apply MIS weighting if enabled
					if (u_enableMIS && lightPDF > 1.0001) {
						// Non-delta light source (area lights, etc.)
						float bsdfPDF = max(dot(normal, lightDir), 0.0) * INV_PI;
						float w = misPowerHeuristic(lightPDF, bsdfPDF);
						directLight += radiance * brdfWithAO * w / lightPDF;
					} else {
						// Delta light source (point/spot/directional) - no PDF division
						directLight += radiance * brdfWithAO;
					}

					// Account for probability of selecting this light (1 / lightCount)
					directLight *= float(u_lightCount);
				}
			}
		}
	}
	
	// Sample environment as a light (NEE for IBL)
	if (u_enableIBL && randomFloat() < 0.5) {
		vec3 envLightDir;
		float envPDF;
		vec3 envRadiance = sampleEnvironmentDirect(hitPos, normal, viewDir, mat, envLightDir, envPDF);
		
		if (envPDF > 0.0 && dot(normal, envLightDir) > 0.0) {
			// Evaluate BRDF with separated components for proper AO application
			vec3 diffuseBRDF, specularBRDF;
			evaluateBRDF_Canonical_Separated(mat, normal, viewDir, envLightDir,
				diffuseBRDF, specularBRDF);
			
			// Apply AO matching deferred IBL: diffuse uses diffuseAO, specular uses specularAO
			vec3 brdfWithAO = diffuseBRDF * diffuseAO + specularBRDF * specularAO;
			
			if (u_enableMIS) {
				float bsdfPDF = max(dot(normal, envLightDir), 0.0) * INV_PI;
				float w = misPowerHeuristic(envPDF, bsdfPDF);
				directLight += envRadiance * brdfWithAO * w / envPDF * 2.0; // *2 because we sample 50% of the time
			} else {
				directLight += envRadiance * brdfWithAO / envPDF * 2.0;
			}
		}
	}
	
	return directLight;
}

// Sky

vec3 sampleSky(vec3 direction) {
	if (u_enableIBL) {
		// Sample HDR environment map
		vec3 envColor = texture(u_environmentMap, direction).rgb;
		return envColor * u_iblIntensity;
	} else {
		// Fallback gradient sky
		float t = 0.5 * (direction.y + 1.0);
		vec3 horizon = vec3(1.0, 1.0, 1.0);
		vec3 zenith  = vec3(0.5, 0.7, 1.0);
		return mix(horizon, zenith, t) * 0.3;
	}
}


// BVH Debug Visualization

#define GAMMA 0.8

// Map complexity value to RGB color
vec3 complexityToRGB(uint complexity) { 
	float wavelength = 380.0 + 370.0 * float(complexity) / float(u_heatmapColorLimit);
	vec3 color;
	
	if (wavelength <= 380.0) {
		color = vec3(0.0);
	} else if (wavelength > 380.0 && wavelength <= 440.0) {
		color.r = -(wavelength - 440.0) / (440.0 - 380.0) / 3.0;
		color.g = 0.0;
		color.b = 0.8;
	} else if (wavelength >= 440.0 && wavelength <= 490.0) {
		color.r = 0.0;
		color.g = (wavelength - 440.0) / (490.0 - 440.0);
		color.b = 1.0;
	} else if (wavelength >= 490.0 && wavelength <= 510.0) {
		color.r = 0.0;
		color.g = 1.0;
		color.b = -(wavelength - 510.0) / (510.0 - 490.0);
	} else if (wavelength >= 510.0 && wavelength <= 580.0) {
		color.r = (wavelength - 510.0) / (580.0 - 510.0);
		color.g = 1.0;
		color.b = 0.0;
	} else if (wavelength >= 580.0 && wavelength <= 645.0) {
		color.r = 1.0;
		color.g = -(wavelength - 645.0) / (645.0 - 580.0);
		color.b = 0.0;
	} else if (wavelength >= 645.0 && wavelength <= 780.0) {
		color.r = 1.0;
		color.g = 0.0;
		color.b = 0.0;
	} else {
		color = vec3(1.0);
	}
	
	float factor;
	if (wavelength >= 380.0 && wavelength < 420.0) {
		factor = 0.3 + 0.7 * (wavelength - 380.0) / (420.0 - 380.0);
	} else if (wavelength >= 420.0 && wavelength < 701.0) {
		factor = 1.0;
	} else if (wavelength >= 701.0 && wavelength < 781.0) {
		factor = 0.3 + 0.7 * (780.0 - wavelength) / (780.0 - 700.0);
		return pow((color + factor * vec3(1.0)), vec3(GAMMA));
	} else {
		factor = 1.0;
	}
	
	return pow(factor * color, vec3(GAMMA));
}


// Main

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= int(u_resolution.x) || pixel.y >= int(u_resolution.y)) return;

	initRandom(uvec2(pixel), u_frameIndex);

	// Sub-pixel jitter for anti-aliasing and reducing structured artifacts
	vec2 jitter = vec2(randomFloat(), randomFloat()) - 0.5;
	vec2 uv = (vec2(pixel) + 0.5 + jitter * 0.5) / u_resolution;
	
	float depth = texture(u_gbufferDepth, uv).r;

	// BVH Debug Mode - cast primary ray and visualize traversal
	if (u_displayBVH) {
		vec2 ndc = (vec2(pixel) / u_resolution) * 2.0 - 1.0;
		float aspect = u_resolution.x / u_resolution.y;

		vec3 rayDir = normalize(
			u_cameraFront +
			ndc.x * aspect * tan(u_fov * 0.5) * u_cameraRight +
			ndc.y * tan(u_fov * 0.5) * u_cameraUp
		);

		Ray ray;
		ray.origin = u_cameraPos;
		ray.direction = rayDir;
		ray.tMin = EPSILON;
		ray.tMax = MAX_FLOAT;

		uint aabbCount = 0u;
		uint triCount = 0u;
		HitInfo hit = traceBVHDebug(ray, aabbCount, triCount);

		// Visualize traversal complexity
		uint complexity = aabbCount + 3u * triCount;  // Weight triangle tests more
		vec3 heatmapColor = complexityToRGB(complexity);

		imageStore(u_outputImage, pixel, vec4(heatmapColor, 1.0));
		return;
	}

	// Sky background
	if (depth >= 0.9999) {
		vec2 ndc = (vec2(pixel) / u_resolution) * 2.0 - 1.0;
		float aspect = u_resolution.x / u_resolution.y;

		vec3 rayDir = normalize(
			u_cameraFront +
			ndc.x * aspect * tan(u_fov * 0.5) * u_cameraRight +
			ndc.y * tan(u_fov * 0.5) * u_cameraUp
		);

		vec3 skyColor = sampleSky(rayDir);
		vec3 prevColor = imageLoad(u_outputImage, pixel).rgb;
		float blend = 1.0 / float(u_frameIndex + 1);
		vec3 color = mix(prevColor, skyColor, blend);
		imageStore(u_outputImage, pixel, vec4(color, 1.0));
		return;
	}

	vec3 worldPos = reconstructWorldPosition(uv, depth);

	// Read material ID from G-buffer
	uint materialID = texture(u_gbufferMaterialID, uv).r;

	vec4 packedNormalRM = texture(u_gbufferPackedNormalRM, uv);
	vec2 octNormal = packedNormalRM.rg;
	// Clamp roughness to minimum 0.04 to match deferred lighting
	float roughness = ClampPerceptualRoughness(packedNormalRM.b);
	float metallic  = clamp(packedNormalRM.a, 0.0, 1.0);

	vec4 albedoAO = texture(u_gbufferAlbedoAO, uv);
	vec3 albedo = albedoAO.rgb;
	float aoTex = clamp(albedoAO.a, 0.0, 1.0);

	vec4 specularF0Data = texture(u_gbufferSpecularF0, uv);
	vec3 specular = specularF0Data.rgb;
	float emissiveStrength = specularF0Data.a;

	vec4 emissiveData = texture(u_gbufferEmissive, uv);
	vec3 emissive = emissiveData.rgb * emissiveStrength;
	vec2 clearcoatData = texture(u_gbufferClearCoat, uv).rg;
	vec4 principledData = texture(u_gbufferPrincipledParams, uv);

	vec3 normal = DecodeNormalOct8(octNormal);
	
	// Validate normal (match deferred lighting)
	if (length(normal) < 0.5 || any(isnan(normal))) {
		normal = vec3(0.0, 1.0, 0.0);
	}
	normal = normalize(normal);

	// Material IDs 0 (standard PBR) and 2 (transmission) share the canonical BRDF core.
	// Future IDs (3=SSS, 4=Cloth, 5=Clearcoat) can modify BRDF here

	Material gbufferMat;
	gbufferMat.albedo = albedo;
	gbufferMat.roughness = roughness;
	gbufferMat.metallic  = metallic;
	gbufferMat.specular  = specular;
	gbufferMat.emissive  = emissive;
	gbufferMat.emissiveStrength = emissiveStrength;
	gbufferMat.materialID = materialID;
	// Set default values for extended properties (not stored in G-buffer)
	gbufferMat.specularColorFactor = vec3(1.0);
	gbufferMat.transmissionFactor = clamp(principledData.r, 0.0, 1.0);
	gbufferMat.clearcoatFactor = clearcoatData.r;
	gbufferMat.clearcoatRoughnessFactor = ClampPerceptualRoughness(clearcoatData.g);
	gbufferMat.ior = principledData.g > 0.0 ? principledData.g : 1.5;
	gbufferMat.attenuationDistance = 1e30;
	gbufferMat.attenuationColor = vec3(1.0);
	gbufferMat.thicknessFactor = 0.0;
	gbufferMat.normalScale = 1.0;
	gbufferMat.occlusionStrength = 1.0;
	gbufferMat.specularFactor = 1.0;
	gbufferMat.alpha = 1.0;
	gbufferMat.alphaCutoff = 0.5;
	gbufferMat.alphaMode = 0u;
	gbufferMat.padding0 = 0.0;

	vec3 color = vec3(0.0);
	vec3 V = normalize(u_cameraPos - worldPos);
	float NdotV = max(dot(normal, V), 0.001);

	// AO Calculation (matching deferred lighting)
	// In RT we don't have SSAO, so use texture AO directly as diffuseAO
	// aoStrength would be a uniform in a full implementation, here we assume 1.0
	float diffuseAO = aoTex;
	float specularAO = SpecularOcclusion(NdotV, diffuseAO, roughness);

	// Direct Lighting at Primary Surface
	// Compute direct lighting contribution from lights at the G-buffer surface
	vec3 directLighting = vec3(0.0);
	if (u_enableNEE && u_lightCount > 0) {
		directLighting = evaluateDirectLightingMIS(worldPos, normal, V, gbufferMat, diffuseAO, specularAO);
	}
	
	// Add emissive contribution (only once at primary surface)
	// emissive already includes emissiveStrength multiplication, matching deferred lighting
	directLighting += gbufferMat.emissive;

	// Indirect Lighting via Path Tracing
	vec3 indirectLighting = vec3(0.0);
	
	for (int i = 0; i < u_sampleCount; ++i) {
		// Importance sample the hemisphere based on material
		vec3 rayDir;
		float pdf;
		
		// Use mixed sampling: specular vs diffuse based on material
		// For metals with low roughness, almost all sampling should be specular
		float specularWeight = 1.0 - gbufferMat.roughness * gbufferMat.roughness;
		
		// Metals should have very high specular weight regardless of roughness
		// This ensures mirror-like metals get proper reflections
		specularWeight = mix(specularWeight, 1.0, max(gbufferMat.metallic, gbufferMat.transmissionFactor));
		
		// For near-mirror surfaces, use very high specular probability
		if (gbufferMat.roughness < 0.1 && gbufferMat.metallic > 0.5) {
			specularWeight = 0.98;
		}
		
		float specularProb = clamp(specularWeight, 0.1, 0.98);
		
		if (randomFloat() < specularProb) {
			// GGX importance sampling for specular
			// Use actual roughness for GGX sampling (clamped for numerical stability only)
			float sampleRoughness = max(gbufferMat.roughness, 0.001);
			rayDir = randomGGXDirection(normal, V, sampleRoughness);
			
			// Compute PDF for GGX sampling
			vec3 H = normalize(V + rayDir);
			float NdotH = max(dot(normal, H), 0.0);
			float VdotH = max(dot(V, H), 0.0);
			float D = DistributionGGX(normal, H, sampleRoughness);
			pdf = specularProb * D * NdotH / (4.0 * VdotH + 0.0001);
		} else {
			// Cosine-weighted hemisphere for diffuse
			rayDir = randomCosineDirection(normal);
			float NdotL = max(dot(normal, rayDir), 0.0);
			pdf = (1.0 - specularProb) * NdotL * INV_PI;
		}
		
		// Skip invalid samples
		float NdotL = dot(normal, rayDir);
		if (NdotL <= 0.0 || pdf < 0.0001) continue;

		Ray ray;
		ray.origin = worldPos + normal * EPSILON;
		ray.direction = rayDir;
		ray.tMin = EPSILON;
		ray.tMax = MAX_FLOAT;

		// Trace indirect path (returns incoming radiance from that direction)
		vec3 incomingRadiance = tracePath(ray);

		// Evaluate BRDF for this direction (includes NdotL)
		vec3 brdf = evaluateBRDF(gbufferMat, normal, V, rayDir);
		
		// Monte Carlo estimator: L_indirect = sum(Li * BRDF / pdf) / N
		if (pdf > 0.0001) {
			indirectLighting += incomingRadiance * brdf / pdf;
		}
	}

	// Average over samples
	indirectLighting /= float(max(u_sampleCount, 1));
	
	// Apply ambient occlusion to indirect lighting (matching deferred)
	// Diffuse indirect uses diffuseAO, specular indirect uses specularAO
	// For simplicity, we use a blend based on material roughness
	float indirectAO = mix(specularAO, diffuseAO, roughness);
	indirectLighting *= indirectAO;

	// Combine direct and indirect
	color = directLighting + indirectLighting;
	
	// Clamp to prevent fireflies while preserving HDR range
	color = min(color, vec3(50.0));
	
	// Temporal accumulation with running average
	vec3 prevColor = imageLoad(u_outputImage, pixel).rgb;
	float blend = 1.0 / float(u_frameIndex + 1);
	color = mix(prevColor, color, blend);

	// Final safety clamp
	color = max(color, vec3(0.0));
	color = min(color, vec3(100.0));
	
	imageStore(u_outputImage, pixel, vec4(color, 1.0));
}


// Path tracing with transmission support and correct energy conservation

vec3 tracePath(Ray initialRay) {
	vec3 radiance   = vec3(0.0);
	vec3 throughput = vec3(1.0);

	Ray ray = initialRay;
	bool insideMedium = false;  // Track if we're inside a transmissive material

	for (int bounce = 0; bounce < min(u_maxBounces, MAX_RAY_BOUNCES); ++bounce) {
		HitInfo hit = traceBVH(ray);
		
		// Missed scene - sample environment
		if (!hit.hit) {
			// Always add sky contribution when ray escapes
			// NEE already sampled direct environment, but this catches indirect paths
			radiance += throughput * sampleSky(ray.direction);
			break;
		}
		
		// Emissive surface contribution
		// For NEE: only count emissive on first bounce to avoid double-counting
		if (hit.material.emissiveStrength > 0.0) {
			vec3 emission = hit.material.emissive * hit.material.emissiveStrength;
			// On bounce 0 or when NEE is disabled, add emission
			// With NEE enabled on later bounces, still add for non-light emissive surfaces
			if (bounce == 0 || !u_enableNEE) {
				radiance += throughput * emission;
			}
		}
		
		// Russian roulette for path termination (energy-conserving)
		// Only apply after minimum bounces to avoid bias
		if (bounce > 3) {
			float maxThroughput = max(throughput.x, max(throughput.y, throughput.z));
			// Clamp survival probability to reasonable range
			float survivalProb = clamp(maxThroughput, 0.05, 0.95);
			
			if (randomFloat() > survivalProb) {
				break;  // Terminate path
			}
			// Compensate for termination probability
			throughput /= survivalProb;
		}

		vec3 V = -ray.direction;
		vec3 N = hit.normal;
		
		// Flip normal if we hit backface (inside medium)
		if (insideMedium) {
			N = -N;
		}
		
		// Handle transmissive materials (glass, etc.)
		if (hit.material.materialID == 2u && hit.material.transmissionFactor > 0.0) {
			vec3 transmissionF0 = ResolveMaterialF0(hit.material);
			float NdotV = max(abs(dot(N, V)), 0.001);
			float transmissionWeight = ComputeTransmissionWeight(hit.material.transmissionFactor, NdotV, transmissionF0);
			
			// Stochastic Fresnel: decide between reflection and refraction
			float reflectProb = 1.0 - transmissionWeight;
			
			if (randomFloat() < reflectProb) {
				// Reflect
				vec3 reflectDir = reflect(-V, N);
				
				// Perfect specular reflection for glass
				// No BRDF division needed for delta distribution
				ray.origin = hit.position + N * EPSILON * 2.0;
				ray.direction = normalize(reflectDir);
				
				// Throughput unchanged for perfect mirror (F/F = 1 when sampling proportional to Fresnel)
			} else {
				// Refract
				bool totalInternalReflection = false;
				vec3 refractDir = ComputeRefractionDirection(-V, N, hit.material.ior, totalInternalReflection);

				if (totalInternalReflection) {
					// TIR - reflect instead
					vec3 reflectDir = reflect(-V, N);
					ray.origin = hit.position + N * EPSILON * 2.0;
					ray.direction = normalize(reflectDir);
				} else {
					if (insideMedium) {
						vec3 mediumTransmittance = ComputeVolumeTransmittance(
							hit.material.albedo,
							hit.material.thicknessFactor,
							hit.material.attenuationDistance,
							hit.material.attenuationColor
						);
						throughput *= mix(vec3(1.0), mediumTransmittance, hit.material.transmissionFactor);
					}

					ray.origin = hit.position - N * EPSILON * 2.0;
					ray.direction = normalize(refractDir);
					insideMedium = !insideMedium;
				}
			}
			
			ray.tMin = EPSILON;
			ray.tMax = MAX_FLOAT;
			continue;
		}
		
		// Next Event Estimation (direct lighting) for opaque materials
		if (u_enableNEE && u_lightCount > 0) {
			// For BVH hit materials, use the material's occlusionStrength as AO
			float hitNdotV = max(dot(N, V), 0.001);
			float hitDiffuseAO = hit.material.occlusionStrength;
			float hitSpecularAO = SpecularOcclusion(hitNdotV, hitDiffuseAO, hit.material.roughness);
			
			vec3 directLight = evaluateDirectLightingMIS(hit.position, N, V, hit.material, hitDiffuseAO, hitSpecularAO);
			radiance += throughput * directLight;
		}

		// Sample next direction using BSDF importance sampling
		vec3 newDirection;
		float bsdfPDF;

		// Probability of sampling specular vs diffuse lobe
		float specularWeight = 1.0 - hit.material.roughness * hit.material.roughness;
		specularWeight = mix(specularWeight, 1.0, max(hit.material.metallic, hit.material.transmissionFactor));
		float specularProb = clamp(specularWeight, 0.1, 0.9);
		
		if (randomFloat() < specularProb) {
			// Sample GGX specular lobe
			newDirection = randomGGXDirection(N, V, ClampPerceptualRoughness(hit.material.roughness));
			
			// GGX PDF (approximate for importance sampling)
			vec3 H = normalize(V + newDirection);
			float NdotH = max(dot(N, H), 0.0);
			float VdotH = max(dot(V, H), 0.0);
			float alpha = hit.material.roughness * hit.material.roughness;
			float D = DistributionGGX(N, H, hit.material.roughness);
			bsdfPDF = specularProb * D * NdotH / (4.0 * VdotH + 0.0001);
		} else {
			// Sample cosine-weighted diffuse
			newDirection = randomCosineDirection(N);
			float NdotL = max(dot(N, newDirection), 0.0);
			bsdfPDF = (1.0 - specularProb) * NdotL * INV_PI;
		}

		// Validate new direction
		float NdotL = dot(newDirection, N);
		if (NdotL <= 0.0 || bsdfPDF < 0.0001) {
			break;
		}
		
		// Evaluate full BRDF for the sampled direction
		vec3 brdf = evaluateBRDF(hit.material, N, V, newDirection);
		
		// Update throughput: BRDF * cos(theta) / PDF
		// Note: evaluateBRDF already includes NdotL, so don't multiply again
		throughput *= brdf / bsdfPDF;
		
		// Clamp throughput to prevent fireflies from bad samples
		float maxT = max(throughput.x, max(throughput.y, throughput.z));
		if (maxT > 10.0) {
			throughput *= 10.0 / maxT;
		}

		// Setup next ray
		ray.origin = hit.position + N * EPSILON;
		ray.direction = newDirection;
		ray.tMin = EPSILON;
		ray.tMax = MAX_FLOAT;
	}

	return radiance;
}
vec3 ResolveMaterialF0(Material mat) {
	return max(mat.specular, vec3(0.0));
}
