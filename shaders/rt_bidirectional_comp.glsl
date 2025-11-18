#version 460 core

/**
 * @file rt_bidirectional_comp.glsl
 * @brief Path Tracing Compute Shader for Nox Engine
 *
 * Implements physically-based bidirectional path tracing starting from G-buffer surfaces.
 * Uses BVH acceleration for efficient ray-scene intersection and supports:
 * - Multiple importance sampling
 * - Temporal accumulation for progressive refinement
 * - PBR materials (metallic-roughness workflow)
 * - Indirect lighting and reflections
 *
 * References:
 * - pbr-book.org: Bidirectional Path Tracing
 * - pbr-book.org: Bounding Volume Hierarchies
 */


// Workgroup

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;


// Constants

#define PI                    3.1415926535897932384626433832795
#define INV_PI                0.31830988618379067154
#define EPSILON               1e-4
#define MAX_FLOAT             1e30
#define MAX_BVH_STACK_SIZE    64
#define MAX_RAY_BOUNCES       8


/* Output image */

layout (rgba16f, binding = 5) uniform image2D u_outputImage;


// G-buffer inputs (packed layout)

// RT0: Oct normal (RG) + Roughness (B) + Metallic (A)
layout (binding = 0) uniform sampler2D u_gbufferPackedNormalRM;
// RT1: Albedo (RGB) + Occlusion (A)
layout (binding = 1) uniform sampler2D u_gbufferAlbedoAO;
// RT2: Emissive (RGB) + Specular luminance (A)
layout (binding = 2) uniform sampler2D u_gbufferEmissiveSpec;
// Depth buffer
layout (binding = 3) uniform sampler2D u_gbufferDepth;

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

struct Material {
	vec3  albedo;
	float metallic;
	vec3  emissive;
	float roughness;
	vec3  specular;
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
	vec3 aabbMin; float pad7;  // Precomputed AABB min bounds (offset 112)
	vec3 aabbMax; float pad8;  // Precomputed AABB max bounds (offset 128)
	Material material;         // Material starts at offset 144
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


// RNG (PCG-ish hash)

uint g_seed;

vec3 tracePath(Ray initialRay);
vec3 sampleSky(vec3 direction);
vec3 reconstructWorldPosition(vec2 uv, float depth);
vec3 DecodeNormalOct8(vec2 oct);
vec3 evaluateBRDF(Material mat, vec3 N, vec3 V, vec3 L);
vec3 randomCosineDirection(vec3 normal);

uint hash(uint x) {
	x += (x << 10u);
	x ^= (x >>  6u);
	x += (x <<  3u);
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

	vec3 b = (abs(normal.x) > abs(normal.y))
		   ? vec3(normal.z, 0.0, -normal.x)
		   : vec3(0.0, -normal.z, normal.y);
	b = normalize(b);
	vec3 t = cross(b, normal);

	return normalize(r * cos(theta) * b +
					 r * sin(theta) * t +
					 sqrt(1.0 - u.x) * normal);
}

vec3 randomGGXDirection(vec3 N, vec3 V, float roughness) {
	float alpha = roughness * roughness;
	vec2  u     = randomVec2();

	float phi      = 2.0 * PI * u.x;
	float cosTheta = sqrt((1.0 - u.y) / (1.0 + (alpha * alpha - 1.0) * u.y));
	float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

	vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

	vec3 up       = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent  = normalize(cross(up, N));
	vec3 bitangent= cross(N, tangent);

	H = tangent * H.x + bitangent * H.y + N * H.z;

	return reflect(-V, H);
}


// Octahedral normal decoding (from G-buffer)

vec3 DecodeNormalOct8(vec2 e) {
	vec3 n;
	n.z = 1.0 - abs(e.x) - abs(e.y);
	n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
	return normalize(n);
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


// BVH traversal (iterative stack)

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
					if (t < hitInfo.t) {
						hitInfo.hit = true;
						hitInfo.t   = t;
						hitInfo.position = ray.origin + ray.direction * t;
						hitInfo.normal   = normalize(tri.n0 * barycentric.x +
													 tri.n1 * barycentric.y +
													 tri.n2 * barycentric.z);
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

// BVH traversal with debug counters
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
					if (t < hitInfo.t) {
						hitInfo.hit = true;
						hitInfo.t   = t;
						hitInfo.position = ray.origin + ray.direction * t;
						hitInfo.normal= normalize(tri.n0 * barycentric.x +
													 tri.n1 * barycentric.y +
													 tri.n2 * barycentric.z);
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


// BRDF (GGX)

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
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx2 = GeometrySchlickGGX(NdotV, roughness);
	float ggx1 = GeometrySchlickGGX(NdotL, roughness);
	return ggx1 * ggx2;
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
	vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

	vec3 numerator   = D * G * F;
	float denominator= 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
	vec3 specular    = numerator / denominator;

	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - mat.metallic);
	vec3 diffuse = kD * mat.albedo * INV_PI;

	return (diffuse + specular) * NdotL;
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

	float att = light.attenuation.x +
				light.attenuation.y * distance +
				light.attenuation.z * distance * distance;

	pdf = (distance * distance) / max(1.0, att);
	return light.color.xyz * light.color.w / max(1.0, att);
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
	float spotEffect= smoothstep(outerCos, innerCos, cosTheta);

	float att = light.attenuation.x +
				light.attenuation.y * distance +
				light.attenuation.z * distance * distance;

	pdf = (distance * distance) / max(1.0, att * spotEffect);
	return light.color.xyz * light.color.w * spotEffect / max(1.0, att);
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

vec3 evaluateDirectLightingMIS(vec3 hitPos, vec3 normal, vec3 viewDir, Material mat) {
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
					vec3 brdf = evaluateBRDF(mat, normal, viewDir, lightDir);

					if (u_enableMIS) {
						float bsdfPDF = max(dot(normal, lightDir), 0.0) * INV_PI;
						float w = misPowerHeuristic(lightPDF, bsdfPDF);
						directLight += radiance * brdf * w / lightPDF;
					} else {
						directLight += radiance * brdf / lightPDF;
					}

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
			// Check if environment is visible (no shadow ray needed for infinite distance)
			vec3 brdf = evaluateBRDF(mat, normal, viewDir, envLightDir);
			
			if (u_enableMIS) {
				float bsdfPDF = max(dot(normal, envLightDir), 0.0) * INV_PI;
				float w = misPowerHeuristic(envPDF, bsdfPDF);
				directLight += envRadiance * brdf * w / envPDF * 2.0; // *2 because we sample 50% of the time
			} else {
				directLight += envRadiance * brdf / envPDF * 2.0;
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

	vec2 uv = (vec2(pixel) + 0.5) / u_resolution;
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

	vec4 packedNormalRM = texture(u_gbufferPackedNormalRM, uv);
	vec2 octNormal = packedNormalRM.rg;
	float roughness = packedNormalRM.b;
	float metallic  = packedNormalRM.a;

	vec4 albedoAO = texture(u_gbufferAlbedoAO, uv);
	vec3 albedo = albedoAO.rgb;
	float occlusion = albedoAO.a;

	vec4 emissiveSpec = texture(u_gbufferEmissiveSpec, uv);
	vec3 emissive = emissiveSpec.rgb;
	float specLuminance = emissiveSpec.a;

	vec3 normal = DecodeNormalOct8(octNormal);

	Material gbufferMat;
	gbufferMat.albedo = albedo;
	gbufferMat.roughness = roughness;
	gbufferMat.metallic  = metallic;
	gbufferMat.specular  = vec3(specLuminance);
	gbufferMat.emissive  = emissive;
	gbufferMat.emissiveStrength = (emissive.r + emissive.g + emissive.b) > 0.0 ? 1.0 : 0.0;

	vec3 color = vec3(0.0);
	vec3 V = normalize(u_cameraPos - worldPos);

	// Path tracing from G-buffer surface
	for (int i = 0; i < u_sampleCount; ++i) {
		vec3 rayDir = randomCosineDirection(normal);

		Ray ray;
		ray.origin = worldPos + normal * EPSILON;
		ray.direction = rayDir;
		ray.tMin = EPSILON;
		ray.tMax = MAX_FLOAT;

		vec3 pathRadiance = tracePath(ray);

		vec3 brdf = evaluateBRDF(gbufferMat, normal, V, rayDir);
		float pdf = max(dot(normal, rayDir), 0.0) * INV_PI;

		if (pdf > 0.0001) color += pathRadiance * brdf / pdf;
	}

	color /= float(u_sampleCount);
	color *= occlusion;

	if (gbufferMat.emissiveStrength > 0.0) {
		color += gbufferMat.emissive * gbufferMat.emissiveStrength;
	}

	vec3 prevColor = imageLoad(u_outputImage, pixel).rgb;
	float blend = 1.0 / float(u_frameIndex + 1);
	color = mix(prevColor, color, blend);

	color = min(color, vec3(10.0));
	imageStore(u_outputImage, pixel, vec4(color, 1.0));
}


// Path tracing

vec3 tracePath(Ray initialRay) {
	vec3 radiance   = vec3(0.0);
	vec3 throughput = vec3(1.0);

	Ray ray = initialRay;

	for (int bounce = 0; bounce < min(u_maxBounces, MAX_RAY_BOUNCES); ++bounce) {
		HitInfo hit = traceBVH(ray);
		// Missed scene
		if (!hit.hit) {
			if (!(u_enableNEE && bounce > 0)) {
				radiance += throughput * sampleSky(ray.direction);
			}
			break;
		}
		// Emissive surface
		if (hit.material.emissiveStrength > 0.0) {
			vec3 emission = hit.material.emissive * hit.material.emissiveStrength;
			radiance += throughput * emission;
		}
		// Russian roulette
		float survivalProb = max(throughput.x, max(throughput.y, throughput.z));
		if (survivalProb < 0.1 && bounce > 2) {
			if (randomFloat() > survivalProb) break;
			throughput /= max(survivalProb, 0.0001);
		}

		vec3 V = -ray.direction;
		// Next Event Estimation
		if (u_enableNEE && u_lightCount > 0) {
			vec3 directLight = evaluateDirectLightingMIS(hit.position, hit.normal, V, hit.material);
			radiance += throughput * directLight;
		}

		vec3 newDirection;
		float bsdfPDF;

		float specularProb = mix(0.1, 0.9, 1.0 - hit.material.roughness * (1.0 - hit.material.metallic));
		// Importance sample between specular and diffuse
		if (randomFloat() < specularProb) {
			newDirection = randomGGXDirection(hit.normal, V, hit.material.roughness);
			bsdfPDF = specularProb;
		} else {
			newDirection = randomCosineDirection(hit.normal);
			bsdfPDF = (1.0 - specularProb) * max(dot(hit.normal, newDirection), 0.0) * INV_PI;
		}

		if (dot(newDirection, hit.normal) <= 0.0) break;
		// Evaluate BRDF
		vec3 brdf = evaluateBRDF(hit.material, hit.normal, V, newDirection);
		if (bsdfPDF < 0.0001) break;

		throughput *= brdf / bsdfPDF;

		ray.origin = hit.position + hit.normal * EPSILON;
		ray.direction = newDirection;
		ray.tMin = EPSILON;
		ray.tMax = MAX_FLOAT;
	}

	return radiance;
}
