#include "BVHBuilder.h"
#include "SceneNode.h"
#include "Scene.h"
#include <algorithm>
#include <queue>
#include <iostream>

RT::BVHData BVHBuilder::BuildFromMesh(
	const MeshComponent& mesh,
	const glm::mat4& worldTransform,
	const BuildParams& params)
{
	RT::BVHData bvhData;

	// Extract triangles from mesh
	bvhData.triangles = ExtractTriangles(mesh, worldTransform);

	if (bvhData.triangles.empty()) {
		std::cerr << "[BVHBuilder] No triangles extracted from mesh" << std::endl;
		return bvhData;
	}

	std::cout << "[BVHBuilder] Building BVH for " << bvhData.triangles.size() << " triangles..." << std::endl;

	// Create initial index list
	std::vector<int> triangleIndices(bvhData.triangles.size());
	for (size_t i = 0; i < triangleIndices.size(); ++i) {
		triangleIndices[i] = static_cast<int>(i);
	}

	// Compute root bounds
	glm::vec3 minBounds, maxBounds;
	ComputeBounds(bvhData.triangles, triangleIndices, minBounds, maxBounds);

	// Build BVH tree
	bvhData.maxDepth = 0;
	BuildRecursive(bvhData.nodes, bvhData.triangles, triangleIndices,
		minBounds, maxBounds, 0, bvhData.maxDepth, params);

	std::cout << "[BVHBuilder] Built BVH with " << bvhData.nodes.size()
		<< " nodes, max depth " << bvhData.maxDepth << std::endl;

	return bvhData;
}

RT::BVHData BVHBuilder::BuildFromScene(
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const BuildParams& params)
{
	RT::BVHData bvhData;

	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[BVHBuilder] Invalid scene graph" << std::endl;
		return bvhData;
	}

	// Traverse scene graph and collect all mesh triangles with proper world transforms
	std::function<void(const std::shared_ptr<SceneNode>&, const glm::mat4&)> collectTriangles;
	collectTriangles = [&](const std::shared_ptr<SceneNode>& node, const glm::mat4& parentTransform) {
		if (!node) return;

		// Compute this node's world transform by combining with parent
		// Use GetGlobalTransform which handles both base transform and animated transform
		glm::mat4 worldTransform = node->GetGlobalTransform(parentTransform);

		// Extract meshes if present
		if (node->GetModel()) {
			auto model = node->GetModel();
			// Scene contains meshes
			for (const auto& mesh : model->meshes) {
				// Extract triangles with world-space transforms applied
				auto meshTriangles = ExtractTriangles(mesh, worldTransform);
				bvhData.triangles.insert(bvhData.triangles.end(),
					meshTriangles.begin(), meshTriangles.end());
			}
		}

		// Recurse to children with accumulated world transform
		for (const auto& child : node->children) {
			collectTriangles(child, worldTransform);
		}
		};

	// Start traversal from root with identity matrix
	collectTriangles(sceneGraph->GetRoot(), glm::mat4(1.0f));

	if (bvhData.triangles.empty()) {
		std::cerr << "[BVHBuilder] No triangles found in scene" << std::endl;
		return bvhData;
	}

	std::cout << "[BVHBuilder] Building scene BVH for " << bvhData.triangles.size()
		<< " triangles (world-space transformed)..." << std::endl;

	// Debug: Print bounds of first triangle to verify world-space coordinates
	if (!bvhData.triangles.empty()) {
		const auto& firstTri = bvhData.triangles[0];
		std::cout << "[BVHBuilder] First triangle vertices (world-space):" << std::endl;
		std::cout << "  v0: (" << firstTri.v0.x << ", " << firstTri.v0.y << ", " << firstTri.v0.z << ")" << std::endl;
		std::cout << "  v1: (" << firstTri.v1.x << ", " << firstTri.v1.y << ", " << firstTri.v1.z << ")" << std::endl;
		std::cout << "  v2: (" << firstTri.v2.x << ", " << firstTri.v2.y << ", " << firstTri.v2.z << ")" << std::endl;
		std::cout << "  center: (" << firstTri.center.x << ", " << firstTri.center.y << ", " << firstTri.center.z << ")" << std::endl;
	}

	// Create initial index list
	std::vector<int> triangleIndices(bvhData.triangles.size());
	for (size_t i = 0; i < triangleIndices.size(); ++i) {
		triangleIndices[i] = static_cast<int>(i);
	}

	// Compute root bounds
	glm::vec3 minBounds, maxBounds;
	ComputeBounds(bvhData.triangles, triangleIndices, minBounds, maxBounds);

	// Build BVH tree
	bvhData.maxDepth = 0;
	BuildRecursive(bvhData.nodes, bvhData.triangles, triangleIndices,
		minBounds, maxBounds, 0, bvhData.maxDepth, params);

	std::cout << "[BVHBuilder] Built scene BVH with " << bvhData.nodes.size()
		<< " nodes, max depth " << bvhData.maxDepth << std::endl;

	return bvhData;
}

void BVHBuilder::ComputeBounds(
	const std::vector<RT::Triangle>& triangles,
	const std::vector<int>& indices,
	glm::vec3& minBounds,
	glm::vec3& maxBounds)
{
	minBounds = glm::vec3(std::numeric_limits<float>::max());
	maxBounds = glm::vec3(std::numeric_limits<float>::lowest());

	for (int idx : indices) {
		const RT::Triangle& tri = triangles[idx];

		// Expand bounds to include all three vertices
		minBounds = glm::min(minBounds, tri.v0);
		minBounds = glm::min(minBounds, tri.v1);
		minBounds = glm::min(minBounds, tri.v2);

		maxBounds = glm::max(maxBounds, tri.v0);
		maxBounds = glm::max(maxBounds, tri.v1);
		maxBounds = glm::max(maxBounds, tri.v2);
	}
}

BVHBuilder::PartitionResult BVHBuilder::PartitionSAH(
	const std::vector<RT::Triangle>& triangles,
	const std::vector<int>& indices,
	const glm::vec3& parentMin,
	const glm::vec3& parentMax,
	size_t maxLeafPrimitives,
	size_t sahBuckets)
{
	PartitionResult result;

	// Check if we should make this a leaf
	if (indices.size() <= maxLeafPrimitives) {
		result.leftIndices = indices;
		result.isLeftLeaf = true;
		return result;
	}

	float bestCost = std::numeric_limits<float>::max();
	int bestAxis = -1;
	int bestSplit = -1;

	float parentArea = SurfaceArea(parentMin, parentMax);

	// Try each axis
	for (int axis = 0; axis < 3; ++axis) {
		// Sort indices by center along this axis
		std::vector<int> sortedIndices = indices;
		std::sort(sortedIndices.begin(), sortedIndices.end(),
			[&](int a, int b) {
				return triangles[a].center[axis] < triangles[b].center[axis];
			});

		// Use binning for large primitive counts
		if (indices.size() > sahBuckets * 2) {
			// Binned SAH
			float axisMin = parentMin[axis];
			float axisMax = parentMax[axis];
			float extent = axisMax - axisMin;

			if (extent < 1e-6f) continue;

			for (size_t bucket = 1; bucket < sahBuckets; ++bucket) {
				float splitPos = axisMin + (extent * bucket) / sahBuckets;

				// Partition at bucket boundary
				auto it = std::partition(sortedIndices.begin(), sortedIndices.end(),
					[&](int idx) {
						return triangles[idx].center[axis] < splitPos;
					});

				if (it == sortedIndices.begin() || it == sortedIndices.end()) {
					continue;
				}

				// Compute bounds and SAH cost
				std::vector<int> leftIndices(sortedIndices.begin(), it);
				std::vector<int> rightIndices(it, sortedIndices.end());

				glm::vec3 leftMin, leftMax, rightMin, rightMax;
				ComputeBounds(triangles, leftIndices, leftMin, leftMax);
				ComputeBounds(triangles, rightIndices, rightMin, rightMax);

				float leftArea = SurfaceArea(leftMin, leftMax);
				float rightArea = SurfaceArea(rightMin, rightMax);

				// SAH cost: traversal cost + probability * intersection cost
				float cost = 0.125f + (leftIndices.size() * leftArea +
					rightIndices.size() * rightArea) / parentArea;

				if (cost < bestCost) {
					bestCost = cost;
					bestAxis = axis;
					bestSplit = static_cast<int>(bucket);
				}
			}
		}
		else {
			// Full sweep SAH for small primitive counts
			for (size_t split = 1; split < indices.size(); ++split) {
				std::vector<int> leftIndices(sortedIndices.begin(),
					sortedIndices.begin() + split);
				std::vector<int> rightIndices(sortedIndices.begin() + split,
					sortedIndices.end());

				glm::vec3 leftMin, leftMax, rightMin, rightMax;
				ComputeBounds(triangles, leftIndices, leftMin, leftMax);
				ComputeBounds(triangles, rightIndices, rightMin, rightMax);

				float leftArea = SurfaceArea(leftMin, leftMax);
				float rightArea = SurfaceArea(rightMin, rightMax);

				float cost = 0.125f + (leftIndices.size() * leftArea +
					rightIndices.size() * rightArea) / parentArea;

				if (cost < bestCost) {
					bestCost = cost;
					bestAxis = axis;
					bestSplit = static_cast<int>(split);
				}
			}
		}
	}

	// If no good split found, make leaf
	if (bestAxis == -1) {
		result.leftIndices = indices;
		result.isLeftLeaf = true;
		return result;
	}

	// Perform best split
	std::vector<int> sortedIndices = indices;
	std::sort(sortedIndices.begin(), sortedIndices.end(),
		[&](int a, int b) {
			return triangles[a].center[bestAxis] < triangles[b].center[bestAxis];
		});

	if (indices.size() > sahBuckets * 2) {
		// Binned split
		float axisMin = parentMin[bestAxis];
		float axisMax = parentMax[bestAxis];
		float extent = axisMax - axisMin;
		float splitPos = axisMin + (extent * bestSplit) / sahBuckets;

		auto it = std::partition(sortedIndices.begin(), sortedIndices.end(),
			[&](int idx) {
				return triangles[idx].center[bestAxis] < splitPos;
			});

		result.leftIndices.assign(sortedIndices.begin(), it);
		result.rightIndices.assign(it, sortedIndices.end());
	}
	else {
		// Direct split
		result.leftIndices.assign(sortedIndices.begin(),
			sortedIndices.begin() + bestSplit);
		result.rightIndices.assign(sortedIndices.begin() + bestSplit,
			sortedIndices.end());
	}

	// Compute bounds for children
	ComputeBounds(triangles, result.leftIndices, result.leftMin, result.leftMax);
	ComputeBounds(triangles, result.rightIndices, result.rightMin, result.rightMax);

	// Check if children should be leaves
	result.isLeftLeaf = (result.leftIndices.size() <= maxLeafPrimitives);
	result.isRightLeaf = (result.rightIndices.size() <= maxLeafPrimitives);

	return result;
}

void BVHBuilder::BuildRecursive(
	std::vector<RT::BVHNode>& nodes,
	const std::vector<RT::Triangle>& triangles,
	std::vector<int>& triangleIndices,
	const glm::vec3& minBounds,
	const glm::vec3& maxBounds,
	int depth,
	int& maxDepthOut,
	const BuildParams& params)
{
	maxDepthOut = std::max(maxDepthOut, depth);

	// Create current node
	int nodeIndex = static_cast<int>(nodes.size());
	nodes.emplace_back();
	RT::BVHNode& node = nodes[nodeIndex];

	node.minBounds = minBounds;
	node.maxBounds = maxBounds;

	// Check for leaf node conditions
	if (triangleIndices.size() <= params.maxLeafPrimitives || depth >= (int)params.maxDepth) {
		// Make leaf node
		size_t count = std::min(triangleIndices.size(), (size_t)4);
		for (size_t i = 0; i < count; ++i) {
			switch (i) {
			case 0: node.triangleIndex0 = triangleIndices[i]; break;
			case 1: node.triangleIndex1 = triangleIndices[i]; break;
			case 2: node.triangleIndex2 = triangleIndices[i]; break;
			case 3: node.triangleIndex3 = triangleIndices[i]; break;
			}
		}
		return;
	}

	// Partition using SAH
	PartitionResult partition = PartitionSAH(triangles, triangleIndices,
		minBounds, maxBounds,
		params.maxLeafPrimitives,
		params.sahBuckets);

	// If partition failed, make leaf
	if (partition.leftIndices.empty() || partition.rightIndices.empty()) {
		size_t count = std::min(triangleIndices.size(), (size_t)4);
		for (size_t i = 0; i < count; ++i) {
			switch (i) {
			case 0: node.triangleIndex0 = triangleIndices[i]; break;
			case 1: node.triangleIndex1 = triangleIndices[i]; break;
			case 2: node.triangleIndex2 = triangleIndices[i]; break;
			case 3: node.triangleIndex3 = triangleIndices[i]; break;
			}
		}
		return;
	}

	// Create left child
	int leftChildIndex = static_cast<int>(nodes.size());
	node.child0 = leftChildIndex;

	if (partition.isLeftLeaf) {
		nodes.emplace_back();
		RT::BVHNode& leftNode = nodes[leftChildIndex];
		leftNode.minBounds = partition.leftMin;
		leftNode.maxBounds = partition.leftMax;
		size_t count = std::min(partition.leftIndices.size(), (size_t)4);
		for (size_t i = 0; i < count; ++i) {
			switch (i) {
			case 0: leftNode.triangleIndex0 = partition.leftIndices[i]; break;
			case 1: leftNode.triangleIndex1 = partition.leftIndices[i]; break;
			case 2: leftNode.triangleIndex2 = partition.leftIndices[i]; break;
			case 3: leftNode.triangleIndex3 = partition.leftIndices[i]; break;
			}
		}
	}
	else {
		BuildRecursive(nodes, triangles, partition.leftIndices,
			partition.leftMin, partition.leftMax,
			depth + 1, maxDepthOut, params);
	}

	// Update node reference (might have been invalidated by vector reallocation)
	RT::BVHNode& currentNode = nodes[nodeIndex];

	// Create right child
	int rightChildIndex = static_cast<int>(nodes.size());
	currentNode.child1 = rightChildIndex;

	if (partition.isRightLeaf) {
		nodes.emplace_back();
		RT::BVHNode& rightNode = nodes[rightChildIndex];
		rightNode.minBounds = partition.rightMin;
		rightNode.maxBounds = partition.rightMax;
		size_t count = std::min(partition.rightIndices.size(), (size_t)4);
		for (size_t i = 0; i < count; ++i) {
			switch (i) {
			case 0: rightNode.triangleIndex0 = partition.rightIndices[i]; break;
			case 1: rightNode.triangleIndex1 = partition.rightIndices[i]; break;
			case 2: rightNode.triangleIndex2 = partition.rightIndices[i]; break;
			case 3: rightNode.triangleIndex3 = partition.rightIndices[i]; break;
			}
		}
	}
	else {
		BuildRecursive(nodes, triangles, partition.rightIndices,
			partition.rightMin, partition.rightMax,
			depth + 1, maxDepthOut, params);
	}
}

std::vector<RT::Triangle> BVHBuilder::ExtractTriangles(
	const MeshComponent& mesh,
	const glm::mat4& worldTransform)
{
	std::vector<RT::Triangle> triangles;

	if (mesh.rawVertices.empty() || mesh.rawIndices.empty()) {
		return triangles;
	}

	// Extract material
	RT::Material material = ExtractMaterial(mesh);

	// Convert triangles
	for (size_t i = 0; i < mesh.rawIndices.size(); i += 3) {
		if (i + 2 >= mesh.rawIndices.size()) break;

		RT::Triangle tri;

		// Get vertices
		const Vertex& v0 = mesh.rawVertices[mesh.rawIndices[i]];
		const Vertex& v1 = mesh.rawVertices[mesh.rawIndices[i + 1]];
		const Vertex& v2 = mesh.rawVertices[mesh.rawIndices[i + 2]];

		// Transform positions
		tri.v0 = glm::vec3(worldTransform * glm::vec4(v0.position, 1.0f));
		tri.v1 = glm::vec3(worldTransform * glm::vec4(v1.position, 1.0f));
		tri.v2 = glm::vec3(worldTransform * glm::vec4(v2.position, 1.0f));

		// Transform normals (use inverse transpose for non-uniform scaling)
		glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldTransform)));
		tri.n0 = glm::normalize(normalMatrix * v0.normal);
		tri.n1 = glm::normalize(normalMatrix * v1.normal);
		tri.n2 = glm::normalize(normalMatrix * v2.normal);

		// Compute center (changed from centroid)
		tri.center = (tri.v0 + tri.v1 + tri.v2) / 3.0f;

		// Assign material
		tri.material = material;

		triangles.push_back(tri);
	}

	return triangles;
}

RT::Material BVHBuilder::ExtractMaterial(const MeshComponent& mesh)
{
	RT::Material material;

	// Base color
	material.albedo = glm::vec3(mesh.baseColorFactor);

	// PBR parameters
	material.metallic = mesh.metallicFactor;
	material.roughness = mesh.roughnessFactor;

	// Emissive
	material.emissive = mesh.emissiveFactor;
	material.emissiveStrength = (mesh.emissiveFactor.r + mesh.emissiveFactor.g + mesh.emissiveFactor.b) > 0.0f ? 1.0f : 0.0f;

	// Specular
	material.specular = mesh.specularFactor;

	return material;
}

float BVHBuilder::SurfaceArea(const glm::vec3& min, const glm::vec3& max)
{
	glm::vec3 extent = max - min;
	return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}
