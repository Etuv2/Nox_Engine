#pragma once
#include <glm/glm.hpp>
#include <GL/glew.h>
#include <vector>

/**
 * @file RTStructures.h
 * @brief GPU-aligned structures for path tracing
 *
 * All structures follow std140 layout rules for SSBO compatibility.
 * Alignment rules:
 * - vec3: 16-byte aligned
 * - float: 4-byte aligned
 * - int: 4-byte aligned
 */

namespace RT {

	/**
	 * @struct Material
	 * @brief Ray tracing material properties (std140 layout)
	 * 
	 * Mirrors the normalized CPU-side material contract:
	 * - Metallic-roughness workflow as the canonical runtime path
	 * - Transmission/refraction (KHR_materials_transmission)
	 * - IOR (KHR_materials_ior)
	 * - Full specular extension (KHR_materials_specular)
	 * - Emissive strength and clearcoat layering
	 * - Alpha transparency (mask and blend modes)
	 */
	struct Material {
		// === Row 0: Albedo + Metallic (16 bytes) ===
		glm::vec3 albedo;        // offset 0   // alignment 16 // size 12
		float metallic;          // offset 12  // alignment 4  // size 4
		
		// === Row 1: Emissive + Roughness (16 bytes) ===
		glm::vec3 emissive;      // offset 16  // alignment 16 // size 12
		float roughness;         // offset 28  // alignment 4  // size 4
		
		// === Row 2: Specular F0 + Emissive Strength (16 bytes) ===
		glm::vec3 specular;      // offset 32  // alignment 16 // size 12
		float emissiveStrength;  // offset 44  // alignment 4  // size 4
		
		// === Row 3: Specular Color Factor + Transmission (16 bytes) ===
		glm::vec3 specularColorFactor;  // offset 48  // alignment 16 // size 12
		float transmissionFactor;       // offset 60  // alignment 4  // size 4
		
		// === Row 4: Clearcoat + IOR (16 bytes) ===
		float clearcoatFactor;          // offset 64  // alignment 4  // size 4
		float clearcoatRoughnessFactor; // offset 68  // alignment 4  // size 4
		float ior;                      // offset 72  // alignment 4  // size 4
		float paddingMedium;            // offset 76  // alignment 4  // size 4
		
		// === Row 5: Reserved for future layered lobes (16 bytes) ===
		glm::vec4 reserved0{ 0.0f };
		
		// === Row 6: Material Flags (16 bytes) ===
		uint32_t materialID;            // offset 96  // alignment 4  // size 4
		                                // 0 = normalized PBR, 2 = transmissive routing
		float normalScale;              // offset 100 // alignment 4  // size 4
		float occlusionStrength;        // offset 104 // alignment 4  // size 4
		float specularFactor;           // offset 108 // alignment 4  // size 4
		
		// === Row 7: Alpha/Transparency (16 bytes) ===
		float alpha;                    // offset 112 // alignment 4  // size 4 // base alpha value
		float alphaCutoff;              // offset 116 // alignment 4  // size 4 // cutoff for MASK mode
		uint32_t alphaMode;             // offset 120 // alignment 4  // size 4 // 0=OPAQUE, 1=MASK, 2=BLEND
		float paddingAlpha;             // offset 124 // alignment 4  // size 4 // padding for 16-byte alignment
		
		// Total: 128 bytes (8 rows x 16 bytes)
		
		Material() 
			: albedo(1.0f), metallic(0.0f)
			, emissive(0.0f), roughness(1.0f)
			, specular(0.04f), emissiveStrength(0.0f)
			, specularColorFactor(1.0f), transmissionFactor(0.0f)
			, clearcoatFactor(0.0f), clearcoatRoughnessFactor(0.0f), ior(1.5f), paddingMedium(0.0f)
			, materialID(0), normalScale(1.0f)
			, occlusionStrength(1.0f), specularFactor(1.0f)
			, alpha(1.0f), alphaCutoff(0.5f), alphaMode(0), paddingAlpha(0.0f) {}
	};

	/**
	 * @struct Triangle
	 * @brief Ray tracing triangle with per-vertex normals and material (std140 layout)
	 */
	struct Triangle {
		// Vertex positions
		glm::vec3 v0;           // offset 0   // alignment 16 // size 12 // total 12 bytes
		float padding0;          // offset 12  // alignment 4  // size 4  // total 16 bytes

		glm::vec3 v1;         // offset 16  // alignment 16 // size 12 // total 28 bytes
		float padding1;         // offset 28  // alignment 4  // size 4  // total 32 bytes

		glm::vec3 v2;        // offset 32  // alignment 16 // size 12 // total 44 bytes
		float padding2;       // offset 44  // alignment 4  // size 4  // total 48 bytes

		// Vertex normals
		glm::vec3 n0;         // offset 48  // alignment 16 // size 12 // total 60 bytes
		float padding3;      // offset 60  // alignment 4  // size 4  // total 64 bytes

		glm::vec3 n1;       // offset 64  // alignment 16 // size 12 // total 76 bytes
		float padding4;        // offset 76  // alignment 4  // size 4  // total 80 bytes

		glm::vec3 n2;  // offset 80  // alignment 16 // size 12 // total 92 bytes
		float padding5;       // offset 92  // alignment 4// size 4  // total 96 bytes

		// Center for BVH partitioning (changed from 'centroid')
		glm::vec3 center;      // offset 96  // alignment 16 // size 12 // total 108 bytes
		float padding6;       // offset 108 // alignment 4  // size 4  // total 112 bytes

		// Precomputed AABB for fast BVH construction
		glm::vec3 aabbMin;     // offset 112 // alignment 16 // size 12 // total 124 bytes
		float padding7;        // offset 124 // alignment 4  // size 4  // total 128 bytes
		
		glm::vec3 aabbMax;     // offset 128 // alignment 16 // size 12 // total 140 bytes
		float padding8;        // offset 140 // alignment 4  // size 4  // total 144 bytes

		// Material properties
		Material material;     // offset 144 // alignment 16 // size 128 // total 272 bytes

		Triangle() : padding0(0), padding1(0), padding2(0), padding3(0), padding4(0), padding5(0), padding6(0), padding7(0), padding8(0) {}
	};

	/**
	 * @struct BVHNode
	 * @brief GPU-friendly BVH node (std140 layout)
	 * this structure uses indices to child nodes and triangle indices for leaf nodes.
	 * 
	 */
	struct BVHNode {
		// AABB bounds
		glm::vec3 minBounds;           // offset 0   // alignment 16 // size 12 // total 12 bytes
		int child0;         // offset 12  // alignment 4// size 4  // total 16 bytes

		glm::vec3 maxBounds;           // offset 16  // alignment 16 // size 12 // total 28 bytes
		int child1;               // offset 28  // alignment 4  // size 4  // total 32 bytes

		// Leaf node data (-1 indicates non-leaf)
		int triangleIndex0;            // offset 32  // alignment 4  // size 4  // total 36 bytes
		int triangleIndex1;      // offset 36  // alignment 4  // size 4  // total 40 bytes
		int triangleIndex2;  // offset 40  // alignment 4  // size 4  // total 44 bytes
		int triangleIndex3;            // offset 44  // alignment 4  // size 4  // total 48 bytes

		BVHNode()
			: child0(-1), child1(-1)
			, triangleIndex0(-1), triangleIndex1(-1)
			, triangleIndex2(-1), triangleIndex3(-1)
		{
		}

		bool IsLeaf() const {
			return child0 == -1 && child1 == -1;
		}
	};

	/**
	 * @struct Ray
	 * @brief Ray structure for GPU ray tracing
	 */
	struct Ray {
		glm::vec3 origin;
		glm::vec3 direction;
		float tMin;
		float tMax;

		Ray() : tMin(0.001f), tMax(1e30f) {}
		Ray(const glm::vec3& o, const glm::vec3& d)
			: origin(o), direction(d), tMin(0.001f), tMax(1e30f) {
		}
	};

	/**
	 * @struct HitInfo
	 * @brief Ray-surface intersection information
	 */
	struct HitInfo {
		bool hit;
		float t;
		glm::vec3 position;
		glm::vec3 normal;
		Material material;

		HitInfo() : hit(false), t(1e30f) {}
	};

	/**
	 * @struct BVHData
	 * @brief Complete BVH structure for a mesh or scene
	 */
	struct BVHData {
		std::vector<BVHNode> nodes;
		std::vector<Triangle> triangles;
		int maxDepth;

		BVHData() : maxDepth(0) {}

		size_t GetNodeCount() const { return nodes.size(); }
		size_t GetTriangleCount() const { return triangles.size(); }
		size_t GetNodeBufferSize() const { return nodes.size() * sizeof(BVHNode); }
		size_t GetTriangleBufferSize() const { return triangles.size() * sizeof(Triangle); }
	};

	/**
	 * @struct RTLightData
	 * @brief GPU-compatible light structure for ray tracing (matches LightManager::LightData)
	 * 
	 * This structure is designed to be compatible with the LightManager's SSBO format
	 * and supports all light types: directional, point, spot, and area lights.
	 */
	struct RTLightData {
		glm::vec4 position;   // xyz = position, w = light type (0=dir, 1=point, 2=spot, 3=area)
		glm::vec4 direction;  // xyz = direction (normalized), w = unused
		glm::vec4 color;          // xyz = color, w = intensity
		glm::vec4 attenuation;    // xyz = constant/linear/quadratic, w = range
		glm::vec4 shadowData;     // x = startSlice, y = sliceCount, z = castsShadows, w = pcss
		glm::vec4 spotData;     // x = inner cone cos, y = outer cone cos, z,w = reserved
		
		// Additional data for ray tracing
		glm::vec4 areaData;       // xyz = size (for area lights), w = reserved
		glm::vec4 sampling;       // x = PDF weight, y = solid angle, z,w = reserved
		
		RTLightData()
		 : position(0.0f), direction(0.0f, -1.0f, 0.0f, 0.0f)
			, color(1.0f), attenuation(1.0f, 0.0f, 0.0f, 100.0f)
			, shadowData(0.0f), spotData(0.0f)
			, areaData(0.0f), sampling(0.0f) {}
	};

	/**
	 * @struct EnvironmentSample
	 * @brief Precomputed importance sampling data for environment map
	 */
	struct EnvironmentSample {
		glm::vec2 uv;  // Environment map UV coordinates
		float pdf;         // Probability density
		float luminance;          // Precomputed luminance
		
		EnvironmentSample()
			: uv(0.0f), pdf(0.0f), luminance(0.0f) {}
	};

} // namespace RT
