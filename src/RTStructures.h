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
	 */
	struct Material {
		glm::vec3 albedo;         // offset 0   // alignment 16 // size 12 // total 12 bytes
		float metallic;       // offset 12  // alignment 4  // size 4  // total 16 bytes

		glm::vec3 emissive;    // offset 16  // alignment 16 // size 12 // total 28 bytes
		float roughness;      // offset 28  // alignment 4  // size 4  // total 32 bytes

		glm::vec3 specular;     // offset 32  // alignment 16 // size 12 // total 44 bytes
		float emissiveStrength;    // offset 44  // alignment 4  // size 4  // total 48 bytes

		Material()
			: albedo(1.0f), metallic(0.0f)
			, emissive(0.0f), roughness(1.0f)
			, specular(0.0f), emissiveStrength(0.0f)
		{
		}
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

		// Material properties
		Material material;        // offset 112 // alignment 16 // size 48 // total 160 bytes

		Triangle() : padding0(0), padding1(0), padding2(0), padding3(0), padding4(0), padding5(0), padding6(0) {}
	};

	/**
	 * @struct BVHNode
	 * @brief GPU-friendly BVH node (std140 layout)
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

} // namespace RT
