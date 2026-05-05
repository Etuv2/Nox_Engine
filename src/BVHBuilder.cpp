#include "BVHBuilder.h"
#include "SceneNode.h"
#include "Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <iostream>

namespace {
	template <typename Fn>
	void ForEachRenderableMesh(const RenderableComponent& renderable, Fn&& fn)
	{
		if (!renderable.model) {
			return;
		}

		if (renderable.renderWholeModel) {
			for (const auto& mesh : renderable.model->meshes) {
				fn(mesh);
			}
			return;
		}

		for (uint32_t meshIndex : renderable.meshIndices) {
			if (meshIndex < renderable.model->meshes.size()) {
				fn(renderable.model->meshes[meshIndex]);
			}
		}
	}

	glm::mat4 ResolveRenderableMeshLocalTransform(const RenderableComponent& renderable, const MeshComponent& mesh)
	{
		if (!renderable.renderWholeModel || mesh.sourceNodeIndex < 0 || !renderable.model) {
			return glm::mat4(1.0f);
		}

		const int referenceNodeIndex = renderable.nodeIndex;
		if (referenceNodeIndex < 0 || referenceNodeIndex == mesh.sourceNodeIndex) {
			return mesh.localTransform;
		}

		const auto& nodeWorldTransforms = renderable.model->GetNodeWorldTransforms();
		if (referenceNodeIndex >= static_cast<int>(nodeWorldTransforms.size()) ||
			mesh.sourceNodeIndex >= static_cast<int>(nodeWorldTransforms.size())) {
			return mesh.localTransform;
		}

		const glm::mat4 referenceInverse = glm::inverse(nodeWorldTransforms[referenceNodeIndex]);
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (!std::isfinite(referenceInverse[column][row])) {
					return mesh.localTransform;
				}
			}
		}

		return referenceInverse * mesh.localTransform;
	}
}

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

	if (params.verbose) {
		std::cout << "[BVHBuilder] Building BVH for " << bvhData.triangles.size() << " triangles..." << std::endl;
	}

	BuildFromExtractedTriangles(bvhData, params);

	if (params.verbose) {
		std::cout << "[BVHBuilder] Built BVH with " << bvhData.nodes.size()
			<< " nodes, max depth " << bvhData.maxDepth << std::endl;
	}

	return bvhData;
}

RT::BVHData BVHBuilder::BuildFromScene(
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const BuildParams& params)
{
	RT::BVHData bvhData;

	ComponentManager* componentManager = sceneGraph ? sceneGraph->GetComponentManager() : nullptr;
	TransformSystem* transformSystem = sceneGraph ? sceneGraph->GetTransformSystem() : nullptr;
	if (!sceneGraph || !componentManager || !transformSystem) {
		std::cerr << "[BVHBuilder] Invalid scene graph" << std::endl;
		return bvhData;
	}

	const auto& renderablePool = componentManager->GetRenderablePool();
	for (const auto& entry : renderablePool) {
		const RenderableComponent& renderable = entry.component;
		if (!renderable.model) {
			continue;
		}

		const glm::mat4 entityWorldTransform = transformSystem->GetWorldTransform(entry.entity);
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			if (mesh.rawVertices.empty() || mesh.rawIndices.empty()) {
				return;
			}

			const glm::mat4 meshWorldTransform =
				entityWorldTransform * ResolveRenderableMeshLocalTransform(renderable, mesh);
			auto meshTriangles = ExtractTriangles(mesh, meshWorldTransform);
			bvhData.triangles.insert(bvhData.triangles.end(),
				meshTriangles.begin(),
				meshTriangles.end());
		});
	}

	if (bvhData.triangles.empty()) {
		std::cerr << "[BVHBuilder] No triangles found in scene" << std::endl;
		return bvhData;
	}

	if (params.verbose) {
		std::cout << "[BVHBuilder] Building scene BVH for " << bvhData.triangles.size()
			<< " triangles (world-space transformed)..." << std::endl;
	}

	// Debug: Print bounds of first triangle to verify world-space coordinates
	if (params.verbose && !bvhData.triangles.empty()) {
		const auto& firstTri = bvhData.triangles[0];
		std::cout << "[BVHBuilder] First triangle vertices (world-space):" << std::endl;
		std::cout << "  v0: (" << firstTri.v0.x << ", " << firstTri.v0.y << ", " << firstTri.v0.z << ")" << std::endl;
		std::cout << "  v1: (" << firstTri.v1.x << ", " << firstTri.v1.y << ", " << firstTri.v1.z << ")" << std::endl;
		std::cout << "  v2: (" << firstTri.v2.x << ", " << firstTri.v2.y << ", " << firstTri.v2.z << ")" << std::endl;
		std::cout << "  center: (" << firstTri.center.x << ", " << firstTri.center.y << ", " << firstTri.center.z << ")" << std::endl;
	}

	BuildFromExtractedTriangles(bvhData, params);

	if (params.verbose) {
		std::cout << "[BVHBuilder] Built scene BVH with " << bvhData.nodes.size()
			<< " nodes, max depth " << bvhData.maxDepth << std::endl;
	}

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

		// Use precomputed AABB instead of recomputing from three vertices
		minBounds = glm::min(minBounds, tri.aabbMin);
		maxBounds = glm::max(maxBounds, tri.aabbMax);
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

	// Reusable sorted indices buffer to avoid repeated allocations
	std::vector<int> sortedIndices = indices;

	// Try each axis
	for (int axis = 0; axis < 3; ++axis) {
		// Sort indices by center along this axis (sort once per axis)
		std::sort(sortedIndices.begin(), sortedIndices.end(),
			[&](int a, int b) {
				return triangles[a].center[axis] < triangles[b].center[axis];
			});

		// Use binning for large primitive counts
		if (indices.size() > sahBuckets * 2) {
			// Binned SAH with bucket aggregation
			float axisMin = parentMin[axis];
			float axisMax = parentMax[axis];
			float extent = axisMax - axisMin;

			if (extent < 1e-6f) continue;

			// Bucket aggregation structures
			struct Bucket {
				glm::vec3 minBounds = glm::vec3(std::numeric_limits<float>::max());
				glm::vec3 maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
				int count = 0;
			};

			std::vector<Bucket> buckets(sahBuckets);

			// Assign triangles to buckets and accumulate bounds
			for (int idx : sortedIndices) {
				float centroid = triangles[idx].center[axis];
				int bucketIdx = static_cast<int>((centroid - axisMin) / extent * sahBuckets);
				bucketIdx = glm::clamp(bucketIdx, 0, static_cast<int>(sahBuckets) - 1);

				buckets[bucketIdx].minBounds = glm::min(buckets[bucketIdx].minBounds, triangles[idx].aabbMin);
				buckets[bucketIdx].maxBounds = glm::max(buckets[bucketIdx].maxBounds, triangles[idx].aabbMax);
				buckets[bucketIdx].count++;
			}

			// Evaluate split costs using bucket aggregates
			for (size_t splitBucket = 1; splitBucket < sahBuckets; ++splitBucket) {
				// Accumulate left side
				glm::vec3 leftMin = glm::vec3(std::numeric_limits<float>::max());
				glm::vec3 leftMax = glm::vec3(std::numeric_limits<float>::lowest());
				int leftCount = 0;

				for (size_t i = 0; i < splitBucket; ++i) {
					if (buckets[i].count > 0) {
						leftMin = glm::min(leftMin, buckets[i].minBounds);
						leftMax = glm::max(leftMax, buckets[i].maxBounds);
						leftCount += buckets[i].count;
					}
				}

				// Accumulate right side
				glm::vec3 rightMin = glm::vec3(std::numeric_limits<float>::max());
				glm::vec3 rightMax = glm::vec3(std::numeric_limits<float>::lowest());
				int rightCount = 0;

				for (size_t i = splitBucket; i < sahBuckets; ++i) {
					if (buckets[i].count > 0) {
						rightMin = glm::min(rightMin, buckets[i].minBounds);
						rightMax = glm::max(rightMax, buckets[i].maxBounds);
						rightCount += buckets[i].count;
					}
				}

				// Skip invalid splits
				if (leftCount == 0 || rightCount == 0) continue;

				float leftArea = SurfaceArea(leftMin, leftMax);
				float rightArea = SurfaceArea(rightMin, rightMax);

				// SAH cost
				float cost = 0.125f + (leftCount * leftArea + rightCount * rightArea) / parentArea;

				if (cost < bestCost) {
					bestCost = cost;
					bestAxis = axis;
					bestSplit = static_cast<int>(splitBucket);
				}
			}
		}
		else {
			// Full sweep SAH with prefix/suffix bounds (no repeated allocations or ComputeBounds calls)
			size_t n = sortedIndices.size();

			// Build prefix bounds (left side accumulation)
			std::vector<glm::vec3> prefixMin(n);
			std::vector<glm::vec3> prefixMax(n);

			glm::vec3 accMin = glm::vec3(std::numeric_limits<float>::max());
			glm::vec3 accMax = glm::vec3(std::numeric_limits<float>::lowest());

			for (size_t i = 0; i < n; ++i) {
				const RT::Triangle& tri = triangles[sortedIndices[i]];
				accMin = glm::min(accMin, tri.aabbMin);
				accMax = glm::max(accMax, tri.aabbMax);
				prefixMin[i] = accMin;
				prefixMax[i] = accMax;
			}

			// Build suffix bounds (right side accumulation)
			std::vector<glm::vec3> suffixMin(n);
			std::vector<glm::vec3> suffixMax(n);

			accMin = glm::vec3(std::numeric_limits<float>::max());
			accMax = glm::vec3(std::numeric_limits<float>::lowest());

			for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
				const RT::Triangle& tri = triangles[sortedIndices[i]];
				accMin = glm::min(accMin, tri.aabbMin);
				accMax = glm::max(accMax, tri.aabbMax);
				suffixMin[i] = accMin;
				suffixMax[i] = accMax;
			}

			// Evaluate all splits in O(1) per split using precomputed bounds
			for (size_t split = 1; split < n; ++split) {
				glm::vec3 leftMin = prefixMin[split - 1];
				glm::vec3 leftMax = prefixMax[split - 1];
				glm::vec3 rightMin = suffixMin[split];
				glm::vec3 rightMax = suffixMax[split];

				float leftArea = SurfaceArea(leftMin, leftMax);
				float rightArea = SurfaceArea(rightMin, rightMax);

				float cost = 0.125f + (split * leftArea + (n - split) * rightArea) / parentArea;

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

	// Perform best split using already-sorted indices for best axis
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

		// Partition using split position
		auto it = std::partition(sortedIndices.begin(), sortedIndices.end(),
			[&](int idx) {
				return triangles[idx].center[bestAxis] < splitPos;
			});

		result.leftIndices.assign(sortedIndices.begin(), it);
		result.rightIndices.assign(it, sortedIndices.end());
	}
	else {
		// Direct split using precomputed split index
		result.leftIndices.assign(sortedIndices.begin(), sortedIndices.begin() + bestSplit);
		result.rightIndices.assign(sortedIndices.begin() + bestSplit, sortedIndices.end());
	}

	// Compute final bounds for children (only once, after split is determined)
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

void BVHBuilder::ComputeBoundsRange(
	const std::vector<RT::Triangle>& triangles,
	const std::vector<int>& indices,
	size_t begin,
	size_t end,
	glm::vec3& minBounds,
	glm::vec3& maxBounds)
{
	minBounds = glm::vec3(std::numeric_limits<float>::max());
	maxBounds = glm::vec3(std::numeric_limits<float>::lowest());

	for (size_t i = begin; i < end; ++i) {
		const RT::Triangle& tri = triangles[indices[i]];
		minBounds = glm::min(minBounds, tri.aabbMin);
		maxBounds = glm::max(maxBounds, tri.aabbMax);
	}
}

bool BVHBuilder::PartitionRangeSAH(
	const std::vector<RT::Triangle>& triangles,
	std::vector<int>& triangleIndices,
	size_t begin,
	size_t end,
	const glm::vec3& parentMin,
	const glm::vec3& parentMax,
	size_t sahBuckets,
	size_t& splitOut,
	glm::vec3& leftMin,
	glm::vec3& leftMax,
	glm::vec3& rightMin,
	glm::vec3& rightMax)
{
	const size_t count = end - begin;
	if (count <= 1 || sahBuckets < 2) {
		return false;
	}

	struct Bucket {
		glm::vec3 minBounds = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
		size_t count = 0;
	};

	const float parentArea = std::max(SurfaceArea(parentMin, parentMax), 1e-8f);
	float bestCost = std::numeric_limits<float>::max();
	int bestAxis = -1;
	size_t bestBucket = 0;
	std::vector<Bucket> buckets(std::max<size_t>(sahBuckets, 2));

	for (int axis = 0; axis < 3; ++axis) {
		const float axisMin = parentMin[axis];
		const float axisMax = parentMax[axis];
		const float extent = axisMax - axisMin;
		if (extent < 1e-6f) {
			continue;
		}

		for (Bucket& bucket : buckets) {
			bucket = Bucket{};
		}

		for (size_t i = begin; i < end; ++i) {
			const int triIndex = triangleIndices[i];
			const RT::Triangle& tri = triangles[triIndex];
			int bucketIndex = static_cast<int>(((tri.center[axis] - axisMin) / extent) * static_cast<float>(buckets.size()));
			bucketIndex = glm::clamp(bucketIndex, 0, static_cast<int>(buckets.size()) - 1);
			Bucket& bucket = buckets[static_cast<size_t>(bucketIndex)];
			bucket.minBounds = glm::min(bucket.minBounds, tri.aabbMin);
			bucket.maxBounds = glm::max(bucket.maxBounds, tri.aabbMax);
			++bucket.count;
		}

		std::vector<glm::vec3> prefixMin(buckets.size());
		std::vector<glm::vec3> prefixMax(buckets.size());
		std::vector<glm::vec3> suffixMin(buckets.size());
		std::vector<glm::vec3> suffixMax(buckets.size());
		std::vector<size_t> prefixCount(buckets.size(), 0);
		std::vector<size_t> suffixCount(buckets.size(), 0);

		glm::vec3 accMin(std::numeric_limits<float>::max());
		glm::vec3 accMax(std::numeric_limits<float>::lowest());
		size_t accCount = 0;
		for (size_t i = 0; i < buckets.size(); ++i) {
			if (buckets[i].count > 0) {
				accMin = glm::min(accMin, buckets[i].minBounds);
				accMax = glm::max(accMax, buckets[i].maxBounds);
				accCount += buckets[i].count;
			}
			prefixMin[i] = accMin;
			prefixMax[i] = accMax;
			prefixCount[i] = accCount;
		}

		accMin = glm::vec3(std::numeric_limits<float>::max());
		accMax = glm::vec3(std::numeric_limits<float>::lowest());
		accCount = 0;
		for (int i = static_cast<int>(buckets.size()) - 1; i >= 0; --i) {
			const size_t bucketIndex = static_cast<size_t>(i);
			if (buckets[bucketIndex].count > 0) {
				accMin = glm::min(accMin, buckets[bucketIndex].minBounds);
				accMax = glm::max(accMax, buckets[bucketIndex].maxBounds);
				accCount += buckets[bucketIndex].count;
			}
			suffixMin[bucketIndex] = accMin;
			suffixMax[bucketIndex] = accMax;
			suffixCount[bucketIndex] = accCount;
		}

		for (size_t splitBucket = 1; splitBucket < buckets.size(); ++splitBucket) {
			const size_t leftCount = prefixCount[splitBucket - 1];
			const size_t rightCount = suffixCount[splitBucket];
			if (leftCount == 0 || rightCount == 0) {
				continue;
			}

			const float leftArea = SurfaceArea(prefixMin[splitBucket - 1], prefixMax[splitBucket - 1]);
			const float rightArea = SurfaceArea(suffixMin[splitBucket], suffixMax[splitBucket]);
			const float cost = 0.125f + (static_cast<float>(leftCount) * leftArea +
				static_cast<float>(rightCount) * rightArea) / parentArea;

			if (cost < bestCost) {
				bestCost = cost;
				bestAxis = axis;
				bestBucket = splitBucket;
			}
		}
	}

	if (bestAxis < 0) {
		return false;
	}

	const float axisMin = parentMin[bestAxis];
	const float axisMax = parentMax[bestAxis];
	const float extent = axisMax - axisMin;
	if (extent < 1e-6f) {
		return false;
	}

	const float splitPos = axisMin + extent * (static_cast<float>(bestBucket) / static_cast<float>(std::max<size_t>(sahBuckets, 2)));
	auto splitIt = std::partition(
		triangleIndices.begin() + static_cast<std::ptrdiff_t>(begin),
		triangleIndices.begin() + static_cast<std::ptrdiff_t>(end),
		[&](int triIndex) {
			return triangles[triIndex].center[bestAxis] < splitPos;
		});

	splitOut = static_cast<size_t>(std::distance(triangleIndices.begin(), splitIt));
	if (splitOut <= begin || splitOut >= end) {
		const glm::vec3 extentVec = parentMax - parentMin;
		const int fallbackAxis = (extentVec.x >= extentVec.y && extentVec.x >= extentVec.z) ? 0 :
			(extentVec.y >= extentVec.z ? 1 : 2);
		const size_t mid = begin + count / 2;
		std::nth_element(
			triangleIndices.begin() + static_cast<std::ptrdiff_t>(begin),
			triangleIndices.begin() + static_cast<std::ptrdiff_t>(mid),
			triangleIndices.begin() + static_cast<std::ptrdiff_t>(end),
			[&](int a, int b) {
				return triangles[a].center[fallbackAxis] < triangles[b].center[fallbackAxis];
			});
		splitOut = mid;
	}

	ComputeBoundsRange(triangles, triangleIndices, begin, splitOut, leftMin, leftMax);
	ComputeBoundsRange(triangles, triangleIndices, splitOut, end, rightMin, rightMax);
	return splitOut > begin && splitOut < end;
}

void BVHBuilder::BuildRecursiveRange(
	std::vector<RT::BVHNode>& nodes,
	const std::vector<RT::Triangle>& triangles,
	std::vector<int>& triangleIndices,
	size_t begin,
	size_t end,
	const glm::vec3& minBounds,
	const glm::vec3& maxBounds,
	int depth,
	int& maxDepthOut,
	const BuildParams& params)
{
	maxDepthOut = std::max(maxDepthOut, depth);

	const int nodeIndex = static_cast<int>(nodes.size());
	nodes.emplace_back();
	RT::BVHNode& node = nodes[nodeIndex];
	node.minBounds = minBounds;
	node.maxBounds = maxBounds;

	const size_t count = end - begin;
	if (count <= params.maxLeafPrimitives || depth >= static_cast<int>(params.maxDepth)) {
		const size_t leafCount = std::min(count, static_cast<size_t>(4));
		for (size_t i = 0; i < leafCount; ++i) {
			switch (i) {
			case 0: node.triangleIndex0 = triangleIndices[begin + i]; break;
			case 1: node.triangleIndex1 = triangleIndices[begin + i]; break;
			case 2: node.triangleIndex2 = triangleIndices[begin + i]; break;
			case 3: node.triangleIndex3 = triangleIndices[begin + i]; break;
			}
		}
		return;
	}

	size_t split = begin;
	glm::vec3 leftMin, leftMax, rightMin, rightMax;
	if (!PartitionRangeSAH(triangles, triangleIndices, begin, end,
		minBounds, maxBounds, params.sahBuckets, split,
		leftMin, leftMax, rightMin, rightMax)) {
		const size_t leafCount = std::min(count, static_cast<size_t>(4));
		for (size_t i = 0; i < leafCount; ++i) {
			switch (i) {
			case 0: node.triangleIndex0 = triangleIndices[begin + i]; break;
			case 1: node.triangleIndex1 = triangleIndices[begin + i]; break;
			case 2: node.triangleIndex2 = triangleIndices[begin + i]; break;
			case 3: node.triangleIndex3 = triangleIndices[begin + i]; break;
			}
		}
		return;
	}

	node.child0 = static_cast<int>(nodes.size());
	BuildRecursiveRange(nodes, triangles, triangleIndices, begin, split,
		leftMin, leftMax, depth + 1, maxDepthOut, params);

	RT::BVHNode& currentNode = nodes[nodeIndex];
	currentNode.child1 = static_cast<int>(nodes.size());
	BuildRecursiveRange(nodes, triangles, triangleIndices, split, end,
		rightMin, rightMax, depth + 1, maxDepthOut, params);
}

void BVHBuilder::BuildFromExtractedTriangles(
	RT::BVHData& bvhData,
	const BuildParams& params)
{
	bvhData.nodes.clear();
	if (bvhData.triangles.empty()) {
		bvhData.maxDepth = 0;
		return;
	}

	std::vector<int> triangleIndices(bvhData.triangles.size());
	for (size_t i = 0; i < triangleIndices.size(); ++i) {
		triangleIndices[i] = static_cast<int>(i);
	}

	glm::vec3 minBounds, maxBounds;
	ComputeBoundsRange(bvhData.triangles, triangleIndices, 0, triangleIndices.size(), minBounds, maxBounds);

	bvhData.nodes.reserve(std::max<size_t>(1, bvhData.triangles.size() * 2));
	bvhData.maxDepth = 0;
	BuildRecursiveRange(bvhData.nodes, bvhData.triangles, triangleIndices,
		0, triangleIndices.size(),
		minBounds, maxBounds,
		0, bvhData.maxDepth, params);
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

		// Precompute AABB - compute once here instead of repeatedly during BVH build
		tri.aabbMin = glm::min(glm::min(tri.v0, tri.v1), tri.v2);
		tri.aabbMax = glm::max(glm::max(tri.v0, tri.v1), tri.v2);

		// Assign material
		tri.material = material;

		triangles.push_back(tri);
	}

	return triangles;
}

RT::Material BVHBuilder::ExtractMaterial(const MeshComponent& mesh)
{
	RT::Material material;
	const MaterialDesc source = mesh.GetMaterialDesc();

	// Normalize RT packing around the canonical metallic-roughness contract.
	material.materialID = source.transmissionFactor > 0.01f ? 2u : 0u;

	// === Base PBR Properties ===
	material.albedo = glm::vec3(source.baseColorFactor);
	material.metallic = source.metallicFactor;
	material.roughness = source.roughnessFactor;

	// === Alpha/Transparency ===
	material.alpha = source.baseColorFactor.a;
	material.alphaCutoff = source.alphaCutoff;
	// Map MeshComponent::AlphaMode to RT material alphaMode
	switch (static_cast<MeshComponent::AlphaMode>(static_cast<uint32_t>(source.alphaMode))) {
		case MeshComponent::ALPHA_OPAQUE: material.alphaMode = 0; break;
		case MeshComponent::ALPHA_MASK:   material.alphaMode = 1; break;
		case MeshComponent::ALPHA_BLEND:  material.alphaMode = 2; break;
		default: material.alphaMode = 0; break;
	}

	// === Emissive ===
	material.emissive = source.emissiveFactor;
	material.emissiveStrength = source.emissiveStrength;

	// === Specular Extension (KHR_materials_specular) ===
	// Calculate F0 from IOR (Schlick approximation)
	float f = (source.ior - 1.0f) / (source.ior + 1.0f);
	float baseF0 = f * f;
	
	// Apply specular factor and color
	const float specFactorValue = source.specularFactor;
	glm::vec3 dielectricF0 = glm::vec3(baseF0) * specFactorValue * source.specularColorFactor;
	material.specular = glm::mix(dielectricF0, material.albedo, material.metallic);
	material.specularFactor = specFactorValue;
	material.specularColorFactor = source.specularColorFactor;

	// === Transmission (KHR_materials_transmission) ===
	material.transmissionFactor = source.transmissionFactor;
	material.clearcoatFactor = source.clearcoatFactor;
	material.clearcoatRoughnessFactor = source.clearcoatRoughnessFactor;
	material.ior = source.ior;
	material.thicknessFactor = source.thicknessFactor;
	material.attenuationDistance = source.attenuationDistance;
	material.attenuationColor = source.attenuationColor;

	// === Additional Properties ===
	material.normalScale = source.normalScale;
	material.occlusionStrength = source.occlusionStrength;

	return material;
}

float BVHBuilder::SurfaceArea(const glm::vec3& min, const glm::vec3& max)
{
	glm::vec3 extent = max - min;
	return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}
