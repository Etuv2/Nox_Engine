#pragma once
#include "RTStructures.h"
#include "MeshComponent.h"
#include "SceneGraph.h"
#include <memory>
#include <vector>

/**
 * @class BVHBuilder
 * @brief Constructs BVH acceleration structures for ray tracing
 *
 * Implements Surface Area Heuristic (SAH) based BVH construction
 * for efficient ray-scene intersection acceleration.
 *
 * References:
 * - pbr-book.org: Bounding Volume Hierarchies
 * - Wald (2007): "On fast Construction of SAH-based Bounding Volume Hierarchies"
 */
class BVHBuilder {
public:
	/**
	 * @brief Build parameters for BVH construction
	 */
	struct BuildParams {
		size_t maxLeafPrimitives = 4;    // Max triangles per leaf node
		size_t maxDepth = 30;           // Max tree depth
		size_t sahBuckets = 16;           // Number of buckets for SAH binning

		BuildParams() = default;
	};

	/**
	 * @brief Build BVH from a single mesh
	 * @param mesh Mesh component containing triangle data
	 * @param worldTransform World transformation matrix
	 * @param params Build parameters
	 * @return Complete BVH structure ready for GPU upload
	 */
	static RT::BVHData BuildFromMesh(
		const MeshComponent& mesh,
		const glm::mat4& worldTransform,
		const BuildParams& params = BuildParams()
	);

	/**
	 * @brief Build scene-level BVH from multiple meshes (TLAS)
	 * @param sceneGraph Scene graph containing all meshes
	 * @param params Build parameters
	 * @return Complete scene BVH structure
	 */
	static RT::BVHData BuildFromScene(
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const BuildParams& params = BuildParams()
	);

private:
	/**
	 * @brief Internal node for BVH construction
 */
	struct BuildNode {
		RT::BVHNode gpuNode;
		std::vector<int> triangleIndices;
		int depth;

		BuildNode() : depth(0) {}
	};

	/**
	 * @brief Compute AABB for a set of triangles
	 */
	static void ComputeBounds(
		const std::vector<RT::Triangle>& triangles,
		const std::vector<int>& indices,
		glm::vec3& minBounds,
		glm::vec3& maxBounds
	);

	/**
	   * @brief Partition node using Surface Area Heuristic
	 */
	struct PartitionResult {
		std::vector<int> leftIndices;
		std::vector<int> rightIndices;
		glm::vec3 leftMin, leftMax;
		glm::vec3 rightMin, rightMax;
		bool isLeftLeaf = false;
		bool isRightLeaf = false;
	};

	static PartitionResult PartitionSAH(
		const std::vector<RT::Triangle>& triangles,
		const std::vector<int>& indices,
		const glm::vec3& parentMin,
		const glm::vec3& parentMax,
		size_t maxLeafPrimitives,
		size_t sahBuckets
	);

	/**
	 * @brief Recursively build BVH tree
	 */
	static void BuildRecursive(
		std::vector<RT::BVHNode>& nodes,
		const std::vector<RT::Triangle>& triangles,
		std::vector<int>& triangleIndices,
		const glm::vec3& minBounds,
		const glm::vec3& maxBounds,
		int depth,
		int& maxDepthOut,
		const BuildParams& params
	);

	/**
	 * @brief Convert mesh to RT triangles
  */
	static std::vector<RT::Triangle> ExtractTriangles(
		const MeshComponent& mesh,
		const glm::mat4& worldTransform
	);

	/**
	 * @brief Extract material from mesh component
	 */
	static RT::Material ExtractMaterial(const MeshComponent& mesh);

	/**
	 * @brief Calculate surface area of AABB
	 */
	static float SurfaceArea(const glm::vec3& min, const glm::vec3& max);
};
