#include "Scene.h"
#include "GLBuffer.h"
#include "TextureUnits.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_interpolation.hpp>
#include <glm/gtx/string_cast.hpp>
#include <limits>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <array>
#include <functional>
#include <unordered_map>
#include "stb_image.h"


// HELPER CONSTANTS & STRUCTS

static constexpr int MAX_INFLUENCES = 4;
static constexpr bool kVerboseSceneImportLogging = false;

static void AppendLoadDiagnosticsCsv(const std::string& modelName,
	float totalMs,
	float parseMs,
	float nodeBuildMs,
	float primitiveBuildMs,
	float materialSetupMs,
	float animationLoadMs,
	size_t nodeCount,
	size_t meshCount,
	size_t primitiveCount,
	size_t vertexCount,
	size_t indexCount)
{
	namespace fs = std::filesystem;
	const fs::path outputDir = fs::path("performance_data");
	const fs::path outputPath = outputDir / "load_diagnostics.csv";

	std::error_code ec;
	fs::create_directories(outputDir, ec);
	const bool hasExistingFile = fs::exists(outputPath, ec);

	std::ofstream out(outputPath.string(), std::ios::app);
	if (!out.is_open()) {
		return;
	}

	if (!hasExistingFile) {
		out << "Model,TotalMs,ParseMs,NodeBuildMs,PrimitiveBuildMs,MaterialSetupMs,AnimationLoadMs,NodeCount,MeshCount,PrimitiveCount,VertexCount,IndexCount\n";
	}

	out << modelName << ","
		<< totalMs << ","
		<< parseMs << ","
		<< nodeBuildMs << ","
		<< primitiveBuildMs << ","
		<< materialSetupMs << ","
		<< animationLoadMs << ","
		<< nodeCount << ","
		<< meshCount << ","
		<< primitiveCount << ","
		<< vertexCount << ","
		<< indexCount << "\n";
}

static uint32_t HashStableMaterialKey(const std::string& modelPath, int meshIndex, int primitiveIndex, int materialIndex)
{
	constexpr uint32_t fnvOffset = 2166136261u;
	constexpr uint32_t fnvPrime = 16777619u;
	uint32_t hash = fnvOffset;

	auto mixByte = [&hash](uint8_t byte) {
		hash ^= static_cast<uint32_t>(byte);
		hash *= fnvPrime;
	};

	for (unsigned char c : modelPath) {
		mixByte(c);
	}

	const uint32_t values[3] = {
		static_cast<uint32_t>(meshIndex + 1),
		static_cast<uint32_t>(primitiveIndex + 1),
		static_cast<uint32_t>(materialIndex + 2)
	};
	for (uint32_t value : values) {
		for (int shift = 0; shift < 32; shift += 8) {
			mixByte(static_cast<uint8_t>((value >> shift) & 0xffu));
		}
	}

	return hash == 0u ? 1u : hash;
}

static glm::mat4 BuildNodeLocalTransform(const tinygltf::Node& gltfNode)
{
	glm::mat4 localMat(1.0f);
	if (!gltfNode.matrix.empty()) {
		for (int i = 0; i < 16; ++i) {
			glm::value_ptr(localMat)[i] = static_cast<float>(gltfNode.matrix[i]);
		}
		return localMat;
	}

	if (!gltfNode.translation.empty()) {
		localMat = glm::translate(localMat, glm::vec3(
			static_cast<float>(gltfNode.translation[0]),
			static_cast<float>(gltfNode.translation[1]),
			static_cast<float>(gltfNode.translation[2])
		));
	}
	if (!gltfNode.rotation.empty()) {
		glm::quat q(
			static_cast<float>(gltfNode.rotation[3]),
			static_cast<float>(gltfNode.rotation[0]),
			static_cast<float>(gltfNode.rotation[1]),
			static_cast<float>(gltfNode.rotation[2])
		);
		localMat *= glm::mat4_cast(q);
	}
	if (!gltfNode.scale.empty()) {
		localMat = glm::scale(localMat, glm::vec3(
			static_cast<float>(gltfNode.scale[0]),
			static_cast<float>(gltfNode.scale[1]),
			static_cast<float>(gltfNode.scale[2])
		));
	}
	return localMat;
}

static bool IsNearlyIdentityTransform(const glm::mat4& transform, float epsilon = 1e-5f)
{
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			const float expected = (c == r) ? 1.0f : 0.0f;
			if (std::abs(transform[c][r] - expected) > epsilon) {
				return false;
			}
		}
	}
	return true;
}

static void PopulateSceneNodesAndWorldTransforms(const tinygltf::Model& model, std::vector<Scene::NodeInfo>& outNodes, std::vector<glm::mat4>& outWorldTransforms)
{
	const int nodeCount = static_cast<int>(model.nodes.size());
	outNodes.resize(model.nodes.size());
	outWorldTransforms.assign(model.nodes.size(), glm::mat4(1.0f));

	for (int i = 0; i < nodeCount; ++i) {
		const tinygltf::Node& gltfNode = model.nodes[static_cast<size_t>(i)];
		Scene::NodeInfo& nodeInfo = outNodes[static_cast<size_t>(i)];
		nodeInfo.name = gltfNode.name;
		nodeInfo.localTransform = BuildNodeLocalTransform(gltfNode);
		nodeInfo.parent = -1;
		nodeInfo.children.clear();
	}

	std::vector<int> sceneRoots;
	sceneRoots.reserve(model.nodes.size());
	std::vector<bool> rootSeen(model.nodes.size(), false);
	std::vector<bool> canonicalSceneRoot(model.nodes.size(), false);
	auto addSceneRoot = [&](int nodeIdx) {
		if (nodeIdx < 0 || nodeIdx >= nodeCount) {
			return;
		}
		if (!rootSeen[static_cast<size_t>(nodeIdx)]) {
			rootSeen[static_cast<size_t>(nodeIdx)] = true;
			canonicalSceneRoot[static_cast<size_t>(nodeIdx)] = true;
			sceneRoots.push_back(nodeIdx);
		}
	};

	if (!model.scenes.empty()) {
		int activeSceneIndex = model.defaultScene;
		if (activeSceneIndex < 0 || activeSceneIndex >= static_cast<int>(model.scenes.size())) {
			activeSceneIndex = 0;
		}
		for (int rootNodeIdx : model.scenes[static_cast<size_t>(activeSceneIndex)].nodes) {
			addSceneRoot(rootNodeIdx);
		}
	}

	if (sceneRoots.empty()) {
		std::vector<int> inDegree(model.nodes.size(), 0);
		for (const tinygltf::Node& gltfNode : model.nodes) {
			for (int childIdx : gltfNode.children) {
				if (childIdx >= 0 && childIdx < nodeCount) {
					++inDegree[static_cast<size_t>(childIdx)];
				}
			}
		}
		for (int nodeIdx = 0; nodeIdx < nodeCount; ++nodeIdx) {
			if (inDegree[static_cast<size_t>(nodeIdx)] == 0) {
				addSceneRoot(nodeIdx);
			}
		}
		if (sceneRoots.empty() && nodeCount > 0) {
			addSceneRoot(0);
		}
	}

	std::vector<uint8_t> traversalState(model.nodes.size(), 0); // 0=unvisited, 1=visiting, 2=done
	size_t skippedCycleEdges = 0;
	size_t skippedMultiParentEdges = 0;
	size_t skippedInvalidChildEdges = 0;
	size_t skippedIncomingRootEdges = 0;
	bool loggedCycleWarning = false;
	bool loggedMultiParentWarning = false;
	bool loggedInvalidChildWarning = false;
	bool loggedIncomingRootWarning = false;

	std::function<void(int)> attachNodeChildren = [&](int nodeIndex) {
		if (nodeIndex < 0 || nodeIndex >= nodeCount) {
			return;
		}
		if (traversalState[static_cast<size_t>(nodeIndex)] == 1 || traversalState[static_cast<size_t>(nodeIndex)] == 2) {
			return;
		}

		traversalState[static_cast<size_t>(nodeIndex)] = 1;
		const tinygltf::Node& gltfNode = model.nodes[static_cast<size_t>(nodeIndex)];

		for (int childIdx : gltfNode.children) {
			if (childIdx < 0 || childIdx >= nodeCount) {
				++skippedInvalidChildEdges;
				if (!loggedInvalidChildWarning) {
					std::cout << "[Scene] Ignoring invalid glTF child node index while building hierarchy." << std::endl;
					loggedInvalidChildWarning = true;
				}
				continue;
			}

			if (canonicalSceneRoot[static_cast<size_t>(childIdx)] && childIdx != nodeIndex) {
				++skippedIncomingRootEdges;
				if (!loggedIncomingRootWarning) {
					std::cout << "[Scene] Ignoring incoming edges to canonical glTF scene root node(s)." << std::endl;
					loggedIncomingRootWarning = true;
				}
				continue;
			}

			if (traversalState[static_cast<size_t>(childIdx)] == 1) {
				++skippedCycleEdges;
				if (!loggedCycleWarning) {
					std::cout << "[Scene] Detected cyclic child edge in glTF hierarchy; skipping cyclic edge(s)." << std::endl;
					loggedCycleWarning = true;
				}
				continue;
			}

			Scene::NodeInfo& childInfo = outNodes[static_cast<size_t>(childIdx)];
			if (childInfo.parent >= 0 && childInfo.parent != nodeIndex) {
				++skippedMultiParentEdges;
				if (!loggedMultiParentWarning) {
					std::cout << "[Scene] Detected multi-parent glTF node usage; keeping first parent assignment for stable transforms." << std::endl;
					loggedMultiParentWarning = true;
				}
				continue;
			}

			Scene::NodeInfo& parentInfo = outNodes[static_cast<size_t>(nodeIndex)];
			if (std::find(parentInfo.children.begin(), parentInfo.children.end(), childIdx) == parentInfo.children.end()) {
				parentInfo.children.push_back(childIdx);
			}
			if (childInfo.parent < 0) {
				childInfo.parent = nodeIndex;
			}

			attachNodeChildren(childIdx);
		}

		traversalState[static_cast<size_t>(nodeIndex)] = 2;
	};

	for (int rootNodeIdx : sceneRoots) {
		attachNodeChildren(rootNodeIdx);
	}

	for (int nodeIdx = 0; nodeIdx < nodeCount; ++nodeIdx) {
		if (traversalState[static_cast<size_t>(nodeIdx)] == 0) {
			attachNodeChildren(nodeIdx);
		}
	}

	if (skippedCycleEdges > 0 || skippedMultiParentEdges > 0 || skippedInvalidChildEdges > 0 || skippedIncomingRootEdges > 0) {
		std::cout << "[Scene] Hierarchy sanitization for glTF import: "
			<< "cyclesSkipped=" << skippedCycleEdges
			<< ", multiParentSkipped=" << skippedMultiParentEdges
			<< ", invalidChildSkipped=" << skippedInvalidChildEdges
			<< ", incomingRootSkipped=" << skippedIncomingRootEdges
			<< std::endl;
	}

	std::vector<uint8_t> worldState(model.nodes.size(), 0); // 0=unvisited, 1=visiting, 2=done
	bool loggedWorldCycleWarning = false;

	std::function<glm::mat4(int)> computeWorldTransform = [&](int nodeIndex) -> glm::mat4 {
		if (nodeIndex < 0 || nodeIndex >= nodeCount) {
			return glm::mat4(1.0f);
		}

		if (worldState[static_cast<size_t>(nodeIndex)] == 2) {
			return outWorldTransforms[static_cast<size_t>(nodeIndex)];
		}
		if (worldState[static_cast<size_t>(nodeIndex)] == 1) {
			if (!loggedWorldCycleWarning) {
				std::cout << "[Scene] Cycle encountered during world transform build; using local transform fallback for affected node(s)." << std::endl;
				loggedWorldCycleWarning = true;
			}
			return outNodes[static_cast<size_t>(nodeIndex)].localTransform;
		}

		worldState[static_cast<size_t>(nodeIndex)] = 1;
		Scene::NodeInfo& nodeInfo = outNodes[static_cast<size_t>(nodeIndex)];
		if (nodeInfo.parent < 0 || nodeInfo.parent >= nodeCount) {
			outWorldTransforms[static_cast<size_t>(nodeIndex)] = nodeInfo.localTransform;
		}
		else {
			glm::mat4 parentWorld = computeWorldTransform(nodeInfo.parent);
			outWorldTransforms[static_cast<size_t>(nodeIndex)] = parentWorld * nodeInfo.localTransform;
		}

		worldState[static_cast<size_t>(nodeIndex)] = 2;
		return outWorldTransforms[static_cast<size_t>(nodeIndex)];
	};

	for (int nodeIdx = 0; nodeIdx < nodeCount; ++nodeIdx) {
		computeWorldTransform(nodeIdx);
	}
}

// A helper struct for sorting influences
struct Influence {
	int boneID;
	float weight;
};


// HELPER FUNCTIONS


/**
 * Merges JOINTS_0/JOINTS_1 and WEIGHTS_0/WEIGHTS_1 into a single top-4 set.
 */
static void MergeBoneData(
	const unsigned short* joints0,
	const unsigned short* joints1,
	const float* weights0,
	const float* weights1,
	size_t i,
	glm::ivec4& outBoneIDs,
	glm::vec4& outBoneWeights,
	int totalJoints
) {
	// Step 1: Collect up to 8 influences
	std::vector<Influence> influences;
	influences.reserve(8);

	// First 4 from JOINTS_0/WEIGHTS_0
	for (int c = 0; c < 4; c++) {
		Influence inf;
		inf.boneID = joints0 ? joints0[i * 4 + c] : 0;
		inf.weight = weights0 ? weights0[i * 4 + c] : 0.0f;
		if (inf.boneID < 0) inf.boneID = 0;
		if (inf.boneID >= totalJoints) {
			std::cerr << "[WARNING] out-of-range boneID " << inf.boneID
				<< ", clamping to [0.." << (totalJoints - 1) << "]\n";
			inf.boneID = std::min(inf.boneID, totalJoints - 1);
		}
		influences.push_back(inf);
	}
	// Next 4 from JOINTS_1/WEIGHTS_1 if present
	if (joints1 && weights1) {
		for (int c = 0; c < 4; c++) {
			Influence inf;
			inf.boneID = joints1[i * 4 + c];
			inf.weight = weights1 ? weights1[i * 4 + c] : 0.0f;
			if (inf.boneID < 0) inf.boneID = 0;
			if (inf.boneID >= totalJoints) {
				std::cerr << "[WARNING] out-of-range boneID " << inf.boneID
					<< ", clamping.\n";
				inf.boneID = std::min(inf.boneID, totalJoints - 1);
			}
			influences.push_back(inf);
		}
	}

	// Step 2: Sort by descending weight
	std::sort(influences.begin(), influences.end(),
		[](const Influence& a, const Influence& b) {
			return a.weight > b.weight;
		});

	// Step 3: Pick top 4
	float sum = 0.0f;
	glm::ivec4 finalIDs(0);
	glm::vec4 finalWs(0.0f);
	for (int c = 0; c < 4 && c < (int)influences.size(); c++) {
		finalIDs[c] = influences[c].boneID;
		finalWs[c] = influences[c].weight;
		sum += influences[c].weight;
	}

	// Step 4: If sum < tiny, set default
	if (sum < 1e-6f) {
		finalIDs = glm::ivec4(0, 0, 0, 0);
		finalWs = glm::vec4(1, 0, 0, 0);
		sum = 1.0f;
	}
	// Step 5: Normalize
	finalWs /= sum;

	outBoneIDs = finalIDs;
	outBoneWeights = finalWs;
}

/**
 * Ensures that the scale component of a matrix is non-negative.
 */
static void SanitizeScale(glm::mat4& m) {
	glm::vec3 trans, scl, skew;
	glm::quat rot;
	glm::vec4 persp;
	glm::decompose(m, scl, rot, trans, skew, persp);

	bool changed = false;
	if (scl.x < 0.0f) { scl.x = -scl.x; changed = true; }
	if (scl.y < 0.0f) { scl.y = -scl.y; changed = true; }
	if (scl.z < 0.0f) { scl.z = -scl.z; changed = true; }
	if (scl.x < 1e-5f) { scl.x = 1e-5f; changed = true; }
	if (scl.y < 1e-5f) { scl.y = 1e-5f; changed = true; }
	if (scl.z < 1e-5f) { scl.z = 1e-5f; changed = true; }

	if (changed) {
		std::cout << "[DEBUG] Fixing negative/zero scale => "
			<< scl.x << "," << scl.y << "," << scl.z << "\n";
		glm::mat4 T = glm::translate(glm::mat4(1.0f), trans);
		glm::mat4 R = glm::toMat4(rot);
		glm::mat4 S = glm::scale(glm::mat4(1.0f), scl);
		m = T * R * S;
	}
}

static float ReadAccessorComponentAsFloat(const unsigned char* componentPtr, int componentType, bool normalized)
{
	switch (componentType) {
	case TINYGLTF_COMPONENT_TYPE_FLOAT:
		return *reinterpret_cast<const float*>(componentPtr);
	case TINYGLTF_COMPONENT_TYPE_DOUBLE:
		return static_cast<float>(*reinterpret_cast<const double*>(componentPtr));
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
		const auto value = *reinterpret_cast<const uint8_t*>(componentPtr);
		return normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
	}
	case TINYGLTF_COMPONENT_TYPE_BYTE: {
		const auto value = *reinterpret_cast<const int8_t*>(componentPtr);
		if (!normalized) return static_cast<float>(value);
		return std::max(static_cast<float>(value) / 127.0f, -1.0f);
	}
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
		const auto value = *reinterpret_cast<const uint16_t*>(componentPtr);
		return normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
	}
	case TINYGLTF_COMPONENT_TYPE_SHORT: {
		const auto value = *reinterpret_cast<const int16_t*>(componentPtr);
		if (!normalized) return static_cast<float>(value);
		return std::max(static_cast<float>(value) / 32767.0f, -1.0f);
	}
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
		const auto value = *reinterpret_cast<const uint32_t*>(componentPtr);
		return normalized ? static_cast<float>(static_cast<double>(value) / 4294967295.0) : static_cast<float>(value);
	}
	case TINYGLTF_COMPONENT_TYPE_INT: {
		const auto value = *reinterpret_cast<const int32_t*>(componentPtr);
		if (!normalized) return static_cast<float>(value);
		return std::max(static_cast<float>(static_cast<double>(value) / 2147483647.0), -1.0f);
	}
	default:
		return 0.0f;
	}
}

static uint32_t ReadAccessorComponentAsUInt(const unsigned char* componentPtr, int componentType)
{
	switch (componentType) {
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
		return *reinterpret_cast<const uint8_t*>(componentPtr);
	case TINYGLTF_COMPONENT_TYPE_BYTE:
		return static_cast<uint32_t>(std::max<int32_t>(*reinterpret_cast<const int8_t*>(componentPtr), 0));
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
		return *reinterpret_cast<const uint16_t*>(componentPtr);
	case TINYGLTF_COMPONENT_TYPE_SHORT:
		return static_cast<uint32_t>(std::max<int32_t>(*reinterpret_cast<const int16_t*>(componentPtr), 0));
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
		return *reinterpret_cast<const uint32_t*>(componentPtr);
	case TINYGLTF_COMPONENT_TYPE_INT:
		return static_cast<uint32_t>(std::max<int32_t>(*reinterpret_cast<const int32_t*>(componentPtr), 0));
	default:
		return 0u;
	}
}


/**
 * Reads a float-based attribute (e.g. "POSITION", "NORMAL", "TEXCOORD_0")
 * from a glTF primitive and calls 'setVertexData(i, dataPtr)' for each element.
 *
 * @param numComponents number of components (3 for position/normal, 2 for UV, etc.)
 * @param setVertexData callback that receives (vertexIndex, floatPtrOfAttribute)
 */
static bool ReadFloatAttribute(
	const tinygltf::Model& model,
	const tinygltf::Primitive& primitive,
	const std::string& attributeName,
	size_t numComponents,
	std::function<void(size_t, const float*)> setVertexData)
{
	auto it = primitive.attributes.find(attributeName);
	if (it == primitive.attributes.end()) {
		return false;
	}
	int accessorIndex = it->second;
	if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()) {
		return false;
	}

	const tinygltf::Accessor& acc = model.accessors[accessorIndex];
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const unsigned char* dataPtr = &buf.data[bv.byteOffset + acc.byteOffset];
	const int accessorComponents = tinygltf::GetNumComponentsInType(acc.type);
	const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
	const size_t stride = bv.byteStride > 0
		? static_cast<size_t>(bv.byteStride)
		: static_cast<size_t>(accessorComponents * componentSize);

	if (accessorComponents < static_cast<int>(numComponents) || componentSize <= 0) {
		return false;
	}

	for (size_t i = 0; i < acc.count; i++) {
		float values[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		const unsigned char* elementPtr = dataPtr + i * stride;
		for (size_t c = 0; c < numComponents; ++c) {
			values[c] = ReadAccessorComponentAsFloat(
				elementPtr + c * componentSize,
				acc.componentType,
				acc.normalized);
		}
		setVertexData(i, values);
	}

	return true;
}

static bool ReadUIntAttribute(
	const tinygltf::Model& model,
	const tinygltf::Primitive& primitive,
	const std::string& attributeName,
	size_t numComponents,
	std::function<void(size_t, const uint32_t*)> setVertexData)
{
	auto it = primitive.attributes.find(attributeName);
	if (it == primitive.attributes.end()) {
		return false;
	}
	int accessorIndex = it->second;
	if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
		return false;
	}

	const tinygltf::Accessor& acc = model.accessors[accessorIndex];
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const unsigned char* dataPtr = &buf.data[bv.byteOffset + acc.byteOffset];
	const int accessorComponents = tinygltf::GetNumComponentsInType(acc.type);
	const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
	const size_t stride = bv.byteStride > 0
		? static_cast<size_t>(bv.byteStride)
		: static_cast<size_t>(accessorComponents * componentSize);

	if (accessorComponents < static_cast<int>(numComponents) || componentSize <= 0) {
		return false;
	}

	for (size_t i = 0; i < acc.count; ++i) {
		uint32_t values[4] = { 0u, 0u, 0u, 0u };
		const unsigned char* elementPtr = dataPtr + i * stride;
		for (size_t c = 0; c < numComponents; ++c) {
			values[c] = ReadAccessorComponentAsUInt(elementPtr + c * componentSize, acc.componentType);
		}
		setVertexData(i, values);
	}

	return true;
}

/**
 * Reads indices into a std::vector<unsigned int>.
 */
static std::vector<unsigned int> ReadIndices(
	const tinygltf::Model& model,
	int accessorIndex)
{
	std::vector<unsigned int> out;
	if (accessorIndex < 0) {
		// No index accessor => empty
		return out;
	}

	const tinygltf::Accessor& acc = model.accessors[accessorIndex];
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];

	const unsigned char* indexData = &buf.data[bv.byteOffset + acc.byteOffset];
	size_t count = acc.count;
	const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
	const size_t stride = bv.byteStride > 0 ? static_cast<size_t>(bv.byteStride) : static_cast<size_t>(componentSize);
	out.resize(count);
	for (size_t i = 0; i < count; ++i) {
		out[i] = ReadAccessorComponentAsUInt(indexData + i * stride, acc.componentType);
	}
	return out;
}

static void ComputeVertexNormalsFromIndexedTriangles(std::vector<Vertex>& vertices,
	const std::vector<unsigned int>& indices)
{
	if (vertices.empty()) {
		return;
	}

	for (Vertex& v : vertices) {
		v.normal = glm::vec3(0.0f);
	}

	if (indices.size() >= 3) {
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			const unsigned int i0 = indices[i + 0];
			const unsigned int i1 = indices[i + 1];
			const unsigned int i2 = indices[i + 2];
			if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
				continue;
			}

			const glm::vec3 e1 = vertices[i1].position - vertices[i0].position;
			const glm::vec3 e2 = vertices[i2].position - vertices[i0].position;
			const glm::vec3 faceNormal = glm::cross(e1, e2);
			const float lenSq = glm::dot(faceNormal, faceNormal);
			if (lenSq <= 1e-12f) {
				continue;
			}

			vertices[i0].normal += faceNormal;
			vertices[i1].normal += faceNormal;
			vertices[i2].normal += faceNormal;
		}
	}

	for (Vertex& v : vertices) {
		const float lenSq = glm::dot(v.normal, v.normal);
		if (lenSq <= 1e-12f) {
			v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		else {
			v.normal = glm::normalize(v.normal);
		}
	}
}

static bool ReadMat4Attribute(
	const tinygltf::Model& model,
	int accessorIndex,
	std::function<void(size_t, const glm::mat4&)> setMatrixData)
{
	if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
		return false;
	}

	const tinygltf::Accessor& acc = model.accessors[accessorIndex];
	if (acc.type != TINYGLTF_TYPE_MAT4) {
		return false;
	}

	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const unsigned char* dataPtr = &buf.data[bv.byteOffset + acc.byteOffset];
	const int accessorComponents = tinygltf::GetNumComponentsInType(acc.type);
	const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
	const size_t stride = bv.byteStride > 0
		? static_cast<size_t>(bv.byteStride)
		: static_cast<size_t>(accessorComponents * componentSize);

	if (accessorComponents != 16 || componentSize <= 0) {
		return false;
	}

	for (size_t i = 0; i < acc.count; ++i) {
		glm::mat4 matrix(1.0f);
		const unsigned char* elementPtr = dataPtr + i * stride;
		for (int c = 0; c < 16; ++c) {
			glm::value_ptr(matrix)[c] = ReadAccessorComponentAsFloat(
				elementPtr + c * componentSize,
				acc.componentType,
				acc.normalized);
		}
		setMatrixData(i, matrix);
	}

	return true;
}

/**
 * Creates a MeshComponent with a VAO, VBO, EBO, etc. from the given vertices & indices.
 */
static MeshComponent CreateMesh(const std::vector<Vertex>& vertices,
	const std::vector<unsigned int>& indices,
	bool hasAlpha)
{
	MeshComponent mesh;
	mesh.rawVertices = vertices;
	mesh.rawIndices = std::vector<uint32_t>(indices.begin(), indices.end());
	mesh.indexCount = indices.size();
	mesh.hasAlpha = hasAlpha;
	mesh.doubleSided = false;
	mesh.diffuseTexture = nullptr;
	mesh.normalTexture = nullptr;
	mesh.roughnessTexture = nullptr;
	mesh.emissiveTexture = nullptr;
	mesh.occlusionTexture = nullptr;
	mesh.specularTexture = nullptr;

	glGenVertexArrays(1, &mesh.VAO);
	glBindVertexArray(mesh.VAO);

	// Create vertex buffer using GLBuffer wrapper
	mesh.vertexBuffer = std::make_unique<GLBuffer>(
		BufferType::Vertex,
		vertices.size() * sizeof(Vertex),
		vertices.data(),
		BufferUsage::StaticDraw
	);

	// Create index buffer using GLBuffer wrapper
	mesh.indexBuffer = std::make_unique<GLBuffer>(
		BufferType::Index,
		indices.size() * sizeof(unsigned int),
		indices.data(),
		BufferUsage::StaticDraw
	);

	// Vertex attribute pointers
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, normal));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, texCoord));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, texCoord1));
	glEnableVertexAttribArray(10);
	// **Tangent with 4 components (XYZ + handedness):**
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, tangent));
	glEnableVertexAttribArray(3);
	// Bone IDs (integer attributes for skinning)
	glVertexAttribIPointer(4, 4, GL_INT, sizeof(Vertex),
		(void*)offsetof(Vertex, boneIDs));
	glEnableVertexAttribArray(4);
	// Bone weights
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
		(void*)offsetof(Vertex, boneWeights));
	glEnableVertexAttribArray(5);

	glBindVertexArray(0);
	return mesh;
}


/**
 * Loads a glTF texture at texIndex, returning a shared_ptr<Texture>.
 * Returns nullptr on failure.
 *
 * glTF color textures (base-color/emissive/specular-color) are sampled in sRGB,
 * while data textures (normal/metal-rough/occlusion/etc.) remain linear.
 */
static std::shared_ptr<Texture> LoadTextureFromGLTF(const tinygltf::Model& model, int texIndex, bool useSRGB = false)
{
	if (texIndex < 0 || texIndex >= (int)model.textures.size()) {
		return nullptr;
	}
	const tinygltf::Texture& gltfTex = model.textures[texIndex];
	int imageIndex = gltfTex.source;
	if (imageIndex < 0 || imageIndex >= (int)model.images.size()) {
		return nullptr;
	}

	const tinygltf::Image& image = model.images[imageIndex];
	if (image.image.empty()) {
		return nullptr;
	}

	// Determine format based on channels
	GLenum format = GL_RGB;
	GLenum internalFormat = GL_RGB8;
	if (image.component == 1) {
		format = GL_RED;
		internalFormat = GL_R8;
	}
	else if (image.component == 2) {
		format = GL_RG;
		internalFormat = GL_RG8;
	}
	else if (image.component == 3) {
		format = GL_RGB;
		internalFormat = useSRGB ? GL_SRGB8 : GL_RGB8;
	}
	else if (image.component == 4) {
		format = GL_RGBA;
		internalFormat = useSRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
	}

	// Get sampler settings from glTF if available
	GLenum wrapS = GL_REPEAT;
	GLenum wrapT = GL_REPEAT;
	GLenum minFilter = GL_LINEAR_MIPMAP_LINEAR;
	GLenum magFilter = GL_LINEAR;

	if (gltfTex.sampler >= 0 && gltfTex.sampler < (int)model.samplers.size()) {
		const tinygltf::Sampler& sampler = model.samplers[gltfTex.sampler];

		// Map glTF wrap modes to OpenGL
		auto mapWrap = [](int gltfWrap) -> GLenum {
			switch (gltfWrap) {
			case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return GL_CLAMP_TO_EDGE;
			case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
			case TINYGLTF_TEXTURE_WRAP_REPEAT:
			default: return GL_REPEAT;
			}
			};

		wrapS = mapWrap(sampler.wrapS);
		wrapT = mapWrap(sampler.wrapT);

		// Map glTF filter modes to OpenGL
		if (sampler.minFilter != -1) {
			switch (sampler.minFilter) {
			case TINYGLTF_TEXTURE_FILTER_NEAREST: minFilter = GL_NEAREST; break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR: minFilter = GL_LINEAR; break;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST: minFilter = GL_NEAREST_MIPMAP_NEAREST; break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST: minFilter = GL_LINEAR_MIPMAP_NEAREST; break;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR: minFilter = GL_NEAREST_MIPMAP_LINEAR; break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
			default: minFilter = GL_LINEAR_MIPMAP_LINEAR; break;
			}
		}

		if (sampler.magFilter != -1) {
			switch (sampler.magFilter) {
			case TINYGLTF_TEXTURE_FILTER_NEAREST: magFilter = GL_NEAREST; break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR:
			default: magFilter = GL_LINEAR; break;
			}
		}
	}

	// Create texture using builder pattern
	auto texture = Texture::Builder::Texture2D(image.width, image.height, internalFormat)
		.Format(format)
		.DataType(GL_UNSIGNED_BYTE)
		.Data(image.image.data())
		.GenerateMipmaps(true)
		.FilterMode(minFilter, magFilter)
		.WrapMode(wrapS, wrapT)
		.Build();

	return texture;
}

bool Scene::LoadFromGLTF(const std::string& path) {
	using Clock = std::chrono::high_resolution_clock;
	const auto loadStart = Clock::now();
	float parseMs = 0.0f;
	float nodeBuildMs = 0.0f;
	float primitiveBuildMs = 0.0f;
	float materialSetupMs = 0.0f;
	float animationLoadMs = 0.0f;
	size_t totalVertexCount = 0;
	size_t totalIndexCount = 0;

	tinygltf::TinyGLTF loader;
	tinygltf::Model model;
	std::string err, warn;
	bool ret = false;
	const auto parseStart = Clock::now();

	// Determine extension to choose ASCII vs binary loading
	std::string extension = path.substr(path.find_last_of('.') + 1);
	std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
	if (extension == "glb") {
		ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
	}
	else {
		ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
	}
	if (!warn.empty()) {
		std::cout << "[GLTF Warning] " << warn << std::endl;
	}
	if (!err.empty()) {
		std::cerr << "[GLTF Error] " << err << std::endl;
	}
	if (!ret) {
		std::cerr << "[GLTF Error] Failed to load: " << path << std::endl;
		return false;
	}
	const auto parseEnd = Clock::now();
	parseMs = std::chrono::duration<float, std::milli>(parseEnd - parseStart).count();

	// Clear any existing data in this Scene
	meshes.clear();
	animations.clear();
	nodes.clear();
	nodeWorldTransforms.clear();
	hasSkin = false;
	hasSkinMetadata = false;
	hasWeightedSkinData = false;
	hasTangents = false;
	skin.joints.clear();
	skin.inverseBindMatrices.clear();
	m_model_name = path.substr(path.find_last_of("/\\") + 1);

	const auto nodeBuildStart = Clock::now();
	PopulateSceneNodesAndWorldTransforms(model, nodes, nodeWorldTransforms);
	const auto nodeBuildEnd = Clock::now();
	nodeBuildMs = std::chrono::duration<float, std::milli>(nodeBuildEnd - nodeBuildStart).count();

	// If there's a skin, note the total joint count for blending weights
	int totalJoints = 0;
	bool hasDeclaredSkinMetadata = false;
	if (!model.skins.empty()) {
		const tinygltf::Skin& gltfSkin = model.skins[0]; // Use first skin
		totalJoints = static_cast<int>(gltfSkin.joints.size());
		hasDeclaredSkinMetadata = true;
		hasSkinMetadata = true;

		// Load skin data
		skin.joints = gltfSkin.joints;

		// Load inverse bind matrices
		if (gltfSkin.inverseBindMatrices >= 0) {
			const tinygltf::Accessor& accessor = model.accessors[gltfSkin.inverseBindMatrices];
			skin.inverseBindMatrices.reserve(accessor.count);
			ReadMat4Attribute(model, gltfSkin.inverseBindMatrices,
				[&](size_t i, const glm::mat4& matrix) {
				if (i >= skin.inverseBindMatrices.size()) {
					skin.inverseBindMatrices.resize(i + 1);
				}
				skin.inverseBindMatrices[i] = matrix;
			});
			for (size_t i = skin.inverseBindMatrices.size(); i < accessor.count; ++i) {
				glm::mat4 matrix(1.0f);
				skin.inverseBindMatrices.push_back(matrix);
			}

			std::cout << "[Scene] Loaded skin with " << skin.joints.size()
				<< " joints and " << skin.inverseBindMatrices.size()
				<< " inverse bind matrices" << std::endl;
		}
	}

	size_t primitiveCount = 0;
	size_t primitivesWithJointAttributes = 0;
	size_t primitivesWithWeightAttributes = 0;
	size_t primitivesWithMeaningfulSkinData = 0;
	size_t primitivesWithImportedNormals = 0;
	size_t primitivesWithGeneratedNormals = 0;
	size_t primitivesNormalMapSuppressed = 0;
	size_t primitivesWithTexCoord1 = 0;
	size_t primitivesUsingUVSet1 = 0;

	// (1) Parse all meshes/primitives once, then attach them to authored nodes.
	// The glTF node table is the source of truth for ownership and transforms.
	std::vector<std::vector<uint32_t>> meshPrimitivesByGltfMesh(model.meshes.size());
	std::unordered_map<uint64_t, std::shared_ptr<Texture>> textureCache;
	textureCache.reserve(model.textures.size());
	size_t textureLoadRequests = 0;
	size_t uniqueTextureLoads = 0;
	struct CachedMaterialData {
		bool initialized = false;
		MeshComponent::AlphaMode alphaMode = MeshComponent::ALPHA_OPAQUE;
		bool hasAlpha = false;
		bool doubleSided = false;
		float alphaCutoff = 0.5f;
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		glm::vec3 emissiveFactor = glm::vec3(0.0f);
		float specularFactor = 1.0f;
		glm::vec3 specularColorFactor = glm::vec3(1.0f);
		float emissiveStrength = 1.0f;
		float clearcoatFactor = 0.0f;
		float clearcoatRoughnessFactor = 0.0f;
		float occlusionStrength = 1.0f;
		float normalScale = 1.0f;
		float transmissionFactor = 0.0f;
		float thicknessFactor = 0.0f;
		float attenuationDistance = std::numeric_limits<float>::infinity();
		glm::vec3 attenuationColor = glm::vec3(1.0f);
		float ior = 1.5f;
		int diffuseTextureIndex = -1;
		int normalTextureIndex = -1;
		int roughnessTextureIndex = -1;
		int emissiveTextureIndex = -1;
		int occlusionTextureIndex = -1;
		int specularTextureIndex = -1;
		int specularColorTextureIndex = -1;
		int transmissionTextureIndex = -1;
		int baseColorTexCoord = 0;
		int normalTexCoord = 0;
		int metallicRoughnessTexCoord = 0;
		int emissiveTexCoord = 0;
		int occlusionTexCoord = 0;
		int specularTexCoord = 0;
		int specularColorTexCoord = 0;
		int transmissionTexCoord = 0;
	};
	std::vector<CachedMaterialData> materialCache(model.materials.size());
	size_t materialCacheHits = 0;
	size_t materialCacheMisses = 0;
	auto buildCachedMaterialData = [](const tinygltf::Material& mat, CachedMaterialData& cached) {
		cached = CachedMaterialData{};

		if (mat.alphaMode == "BLEND") {
			cached.hasAlpha = true;
			cached.alphaMode = MeshComponent::ALPHA_BLEND;
		}
		else if (mat.alphaMode == "MASK") {
			cached.alphaMode = MeshComponent::ALPHA_MASK;
			if (mat.alphaCutoff >= 0.0) {
				cached.alphaCutoff = static_cast<float>(mat.alphaCutoff);
			}
		}

		cached.doubleSided = mat.doubleSided;
		cached.diffuseTextureIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
		cached.normalTextureIndex = mat.normalTexture.index;
		cached.baseColorTexCoord = mat.pbrMetallicRoughness.baseColorTexture.texCoord;
		cached.normalTexCoord = mat.normalTexture.texCoord;
		if (cached.normalTextureIndex >= 0) {
			cached.normalScale = static_cast<float>(mat.normalTexture.scale);
		}
		cached.roughnessTextureIndex = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
		cached.metallicRoughnessTexCoord = mat.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
		cached.emissiveTextureIndex = mat.emissiveTexture.index;
		cached.emissiveTexCoord = mat.emissiveTexture.texCoord;
		cached.occlusionTextureIndex = mat.occlusionTexture.index;
		cached.occlusionTexCoord = mat.occlusionTexture.texCoord;

		if (mat.extensions.count("KHR_materials_specular")) {
			const auto& specExt = mat.extensions.at("KHR_materials_specular");
			if (specExt.Has("specularTexture") && specExt.Get("specularTexture").IsObject()) {
				const auto& specTex = specExt.Get("specularTexture");
				if (specTex.Has("index") && specTex.Get("index").IsInt()) {
					cached.specularTextureIndex = specTex.Get("index").Get<int>();
				}
				if (specTex.Has("texCoord") && specTex.Get("texCoord").IsInt()) {
					cached.specularTexCoord = specTex.Get("texCoord").Get<int>();
				}
			}
			if (specExt.Has("specularColorTexture") && specExt.Get("specularColorTexture").IsObject()) {
				const auto& specColorTex = specExt.Get("specularColorTexture");
				if (specColorTex.Has("index") && specColorTex.Get("index").IsInt()) {
					cached.specularColorTextureIndex = specColorTex.Get("index").Get<int>();
				}
				if (specColorTex.Has("texCoord") && specColorTex.Get("texCoord").IsInt()) {
					cached.specularColorTexCoord = specColorTex.Get("texCoord").Get<int>();
				}
			}
			if (specExt.Has("specularFactor") && specExt.Get("specularFactor").IsNumber()) {
				cached.specularFactor = static_cast<float>(specExt.Get("specularFactor").Get<double>());
			}
			if (specExt.Has("specularColorFactor") && specExt.Get("specularColorFactor").IsArray()) {
				const auto& colorArray = specExt.Get("specularColorFactor");
				if (colorArray.ArrayLen() >= 3) {
					cached.specularColorFactor = glm::vec3(
						static_cast<float>(colorArray.Get(0).Get<double>()),
						static_cast<float>(colorArray.Get(1).Get<double>()),
						static_cast<float>(colorArray.Get(2).Get<double>())
					);
				}
			}
		}

		if (mat.extensions.count("KHR_materials_transmission")) {
			const auto& transExt = mat.extensions.at("KHR_materials_transmission");
			if (transExt.Has("transmissionFactor") && transExt.Get("transmissionFactor").IsNumber()) {
				cached.transmissionFactor = static_cast<float>(transExt.Get("transmissionFactor").Get<double>());
				if (cached.transmissionFactor > 0.0f) {
					cached.hasAlpha = true;
					if (cached.alphaMode == MeshComponent::ALPHA_OPAQUE) {
						cached.alphaMode = MeshComponent::ALPHA_BLEND;
					}
				}
			}
			if (transExt.Has("transmissionTexture") && transExt.Get("transmissionTexture").IsObject()) {
				const auto& transTex = transExt.Get("transmissionTexture");
				if (transTex.Has("index") && transTex.Get("index").IsInt()) {
					cached.transmissionTextureIndex = transTex.Get("index").Get<int>();
				}
				if (transTex.Has("texCoord") && transTex.Get("texCoord").IsInt()) {
					cached.transmissionTexCoord = transTex.Get("texCoord").Get<int>();
				}
			}
		}

		if (mat.extensions.count("KHR_materials_ior")) {
			const auto& iorExt = mat.extensions.at("KHR_materials_ior");
			if (iorExt.Has("ior") && iorExt.Get("ior").IsNumber()) {
				cached.ior = static_cast<float>(iorExt.Get("ior").Get<double>());
			}
		}

		if (mat.extensions.count("KHR_materials_volume")) {
			const auto& volumeExt = mat.extensions.at("KHR_materials_volume");
			if (volumeExt.Has("thicknessFactor") && volumeExt.Get("thicknessFactor").IsNumber()) {
				cached.thicknessFactor = static_cast<float>(volumeExt.Get("thicknessFactor").Get<double>());
			}
			if (volumeExt.Has("attenuationDistance") && volumeExt.Get("attenuationDistance").IsNumber()) {
				cached.attenuationDistance = static_cast<float>(volumeExt.Get("attenuationDistance").Get<double>());
			}
			if (volumeExt.Has("attenuationColor") && volumeExt.Get("attenuationColor").IsArray()) {
				const auto& colorArray = volumeExt.Get("attenuationColor");
				if (colorArray.ArrayLen() >= 3) {
					cached.attenuationColor = glm::vec3(
						static_cast<float>(colorArray.Get(0).Get<double>()),
						static_cast<float>(colorArray.Get(1).Get<double>()),
						static_cast<float>(colorArray.Get(2).Get<double>())
					);
				}
			}
		}

		if (mat.extensions.count("KHR_materials_emissive_strength")) {
			const auto& emissiveExt = mat.extensions.at("KHR_materials_emissive_strength");
			if (emissiveExt.Has("emissiveStrength") && emissiveExt.Get("emissiveStrength").IsNumber()) {
				cached.emissiveStrength = static_cast<float>(emissiveExt.Get("emissiveStrength").Get<double>());
			}
		}

		if (mat.extensions.count("KHR_materials_clearcoat")) {
			const auto& clearcoatExt = mat.extensions.at("KHR_materials_clearcoat");
			if (clearcoatExt.Has("clearcoatFactor") && clearcoatExt.Get("clearcoatFactor").IsNumber()) {
				cached.clearcoatFactor = static_cast<float>(clearcoatExt.Get("clearcoatFactor").Get<double>());
			}
			if (clearcoatExt.Has("clearcoatRoughnessFactor") && clearcoatExt.Get("clearcoatRoughnessFactor").IsNumber()) {
				cached.clearcoatRoughnessFactor = static_cast<float>(clearcoatExt.Get("clearcoatRoughnessFactor").Get<double>());
			}
		}

		if (mat.occlusionTexture.index >= 0) {
			cached.occlusionStrength = static_cast<float>(mat.occlusionTexture.strength);
		}

		if (!mat.pbrMetallicRoughness.baseColorFactor.empty()) {
			cached.baseColorFactor = glm::vec4(
				static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
				static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
				static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
				static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3])
			);
			if (cached.baseColorFactor.a < 1.0f) {
				cached.hasAlpha = true;
			}
		}

		cached.metallicFactor = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
		cached.roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
		if (!mat.emissiveFactor.empty()) {
			cached.emissiveFactor = glm::vec3(
				static_cast<float>(mat.emissiveFactor[0]),
				static_cast<float>(mat.emissiveFactor[1]),
				static_cast<float>(mat.emissiveFactor[2])
			);
		}

		cached.initialized = true;
	};
	auto loadTextureCached = [&](int texIndex, bool useSRGB = false) -> std::shared_ptr<Texture> {
		++textureLoadRequests;
		if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size())) {
			return nullptr;
		}

		const uint64_t cacheKey = (static_cast<uint64_t>(static_cast<uint32_t>(texIndex)) << 1ull) | (useSRGB ? 1ull : 0ull);

		auto it = textureCache.find(cacheKey);
		if (it != textureCache.end()) {
			return it->second;
		}

		std::shared_ptr<Texture> loadedTexture = LoadTextureFromGLTF(model, texIndex, useSRGB);
		textureCache.emplace(cacheKey, loadedTexture);
		if (loadedTexture) {
			++uniqueTextureLoads;
		}
		return loadedTexture;
	};
	const auto primitiveBuildStart = Clock::now();
	for (size_t mm = 0; mm < model.meshes.size(); mm++) {
		const tinygltf::Mesh& gltfMesh = model.meshes[mm];
		for (size_t p = 0; p < gltfMesh.primitives.size(); p++) {
			const tinygltf::Primitive& primitive = gltfMesh.primitives[p];
			++primitiveCount;
			if (primitive.attributes.find("POSITION") == primitive.attributes.end()) {
				// Skip primitive with no position data
				continue;
			}
			// Get vertex count from POSITION accessor
			int posAccessorIndex = primitive.attributes.at("POSITION");
			const tinygltf::Accessor& posAcc = model.accessors[posAccessorIndex];
			size_t vertexCount = posAcc.count;
			if (vertexCount == 0) continue;

			// Prepare vertex buffer
			std::vector<Vertex> vertices(vertexCount);
			for (Vertex& v : vertices) {
				v.position = glm::vec3(0.0f);
				v.normal = glm::vec3(0.0f);
				v.texCoord = glm::vec2(0.0f);
				v.texCoord1 = glm::vec2(0.0f);
				v.tangent = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				v.color = glm::vec4(1.0f);
			}

			// A) Read vertex attributes (positions, normals, tangents, texcoords, skin joints/weights)
			ReadFloatAttribute(model, primitive, "POSITION", 3, [&](size_t i, const float* data) {
				vertices[i].position = glm::vec3(data[0], data[1], data[2]);
				});
			size_t validImportedNormalCount = 0;
			const bool hasNormalsForPrimitive = ReadFloatAttribute(model, primitive, "NORMAL", 3, [&](size_t i, const float* data) {
				const glm::vec3 n(data[0], data[1], data[2]);
				const float lenSq = glm::dot(n, n);
				if (lenSq > 1e-12f) {
					vertices[i].normal = glm::normalize(n);
					++validImportedNormalCount;
				}
				else {
					vertices[i].normal = glm::vec3(0.0f);
				}
				});
			const bool useImportedNormals = hasNormalsForPrimitive && validImportedNormalCount > 0;
			if (useImportedNormals) {
				++primitivesWithImportedNormals;
			}
			const bool hasTangentsForPrimitive = ReadFloatAttribute(model, primitive, "TANGENT", 4, [&](size_t i, const float* data) {
				// Store tangent XYZ and handedness (data[3]) in the vertex
				vertices[i].tangent = glm::vec4(data[0], data[1], data[2], data[3]);
				});
			ReadFloatAttribute(model, primitive, "TEXCOORD_0", 2, [&](size_t i, const float* data) {
				vertices[i].texCoord = glm::vec2(data[0], data[1]);
				});
			const bool hasTexCoord1 = ReadFloatAttribute(model, primitive, "TEXCOORD_1", 2, [&](size_t i, const float* data) {
				vertices[i].texCoord1 = glm::vec2(data[0], data[1]);
				});
			if (hasTexCoord1) {
				++primitivesWithTexCoord1;
			}

			// Skinning data (up to 8 weights, merged into 4)
			std::vector<glm::u32vec4> joints0Values(vertexCount, glm::u32vec4(0u));
			std::vector<glm::u32vec4> joints1Values(vertexCount, glm::u32vec4(0u));
			std::vector<glm::vec4> weights0Values(vertexCount, glm::vec4(0.0f));
			std::vector<glm::vec4> weights1Values(vertexCount, glm::vec4(0.0f));

			const bool hasJoints0 = ReadUIntAttribute(model, primitive, "JOINTS_0", 4,
				[&](size_t i, const uint32_t* data) {
					joints0Values[i] = glm::u32vec4(data[0], data[1], data[2], data[3]);
				});
			const bool hasJoints1 = ReadUIntAttribute(model, primitive, "JOINTS_1", 4,
				[&](size_t i, const uint32_t* data) {
					joints1Values[i] = glm::u32vec4(data[0], data[1], data[2], data[3]);
				});
			const bool hasWeights0 = ReadFloatAttribute(model, primitive, "WEIGHTS_0", 4,
				[&](size_t i, const float* data) {
					weights0Values[i] = glm::vec4(data[0], data[1], data[2], data[3]);
				});
			const bool hasWeights1 = ReadFloatAttribute(model, primitive, "WEIGHTS_1", 4,
				[&](size_t i, const float* data) {
					weights1Values[i] = glm::vec4(data[0], data[1], data[2], data[3]);
				});

			if (hasJoints0 || hasJoints1) {
				++primitivesWithJointAttributes;
			}
			if (hasWeights0 || hasWeights1) {
				++primitivesWithWeightAttributes;
			}

			bool primitiveHasMeaningfulSkinning = false;

			if (hasJoints0 || hasJoints1) {
				for (size_t i = 0; i < vertexCount; ++i) {
					std::array<Influence, 8> influences{};
					int influenceCount = 0;

					auto appendInfluenceSet = [&](const glm::u32vec4& jointSet, const glm::vec4& weightSet, bool enabled) {
						if (!enabled) {
							return;
						}
						for (int c = 0; c < 4; ++c) {
							if (influenceCount >= static_cast<int>(influences.size())) {
								break;
							}
							Influence inf;
							inf.boneID = static_cast<int>(jointSet[c]);
							inf.weight = weightSet[c];
							if (inf.boneID < 0) {
								inf.boneID = 0;
							}
							if (totalJoints > 0 && inf.boneID >= totalJoints) {
								inf.boneID = totalJoints - 1;
							}
							influences[static_cast<size_t>(influenceCount++)] = inf;
						}
					};

					appendInfluenceSet(joints0Values[i], weights0Values[i], hasJoints0);
					appendInfluenceSet(joints1Values[i], weights1Values[i], hasJoints1);

					if (influenceCount > 1) {
						std::sort(influences.begin(), influences.begin() + influenceCount,
							[](const Influence& a, const Influence& b) {
								return a.weight > b.weight;
							});
					}

					glm::ivec4 finalIDs(0);
					glm::vec4 finalWeights(0.0f);
					float totalWeight = 0.0f;
					for (int c = 0; c < 4 && c < influenceCount; ++c) {
						finalIDs[c] = influences[static_cast<size_t>(c)].boneID;
						finalWeights[c] = influences[static_cast<size_t>(c)].weight;
						totalWeight += influences[static_cast<size_t>(c)].weight;
					}

					if (totalWeight < 1e-6f) {
						finalIDs = glm::ivec4(0, 0, 0, 0);
						finalWeights = glm::vec4(1, 0, 0, 0);
					}
					else {
						finalWeights /= totalWeight;
						if ((hasWeights0 || hasWeights1) && totalJoints > 0) {
							primitiveHasMeaningfulSkinning = true;
						}
					}

					vertices[i].boneIDs = finalIDs;
					vertices[i].boneWeights = finalWeights;
				}
			}

			if (primitiveHasMeaningfulSkinning) {
				++primitivesWithMeaningfulSkinData;
			}
			if (hasTangentsForPrimitive) {
				hasTangents = true;
			}
			else {
				// Initialize tangent.w to 1 by default to avoid undefined data
				for (auto& v : vertices) {
					v.tangent = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				}
			}
			// Read index data for this primitive
			std::vector<unsigned int> indices = ReadIndices(model, primitive.indices);
			totalVertexCount += vertexCount;
			totalIndexCount += indices.size();
			if (!useImportedNormals) {
				ComputeVertexNormalsFromIndexedTriangles(vertices, indices);
				++primitivesWithGeneratedNormals;
			}
			else if (validImportedNormalCount < vertexCount) {
				std::vector<Vertex> generatedNormalsVertices = vertices;
				ComputeVertexNormalsFromIndexedTriangles(generatedNormalsVertices, indices);
				for (size_t vi = 0; vi < vertices.size(); ++vi) {
					if (glm::dot(vertices[vi].normal, vertices[vi].normal) <= 1e-12f) {
						vertices[vi].normal = generatedNormalsVertices[vi].normal;
					}
				}
			}

			// **Compute tangents if normal mapping is used but tangents were not provided:**
			int materialIndex = primitive.material;
			bool needsTangents = !hasTangentsForPrimitive;
			bool hasNormalMapTexture = false;
			if (materialIndex >= 0 && materialIndex < model.materials.size()) {
				const tinygltf::Material& mat = model.materials[materialIndex];
				if (mat.normalTexture.index >= 0) {
					hasNormalMapTexture = true;
				}
			}
			if (needsTangents && hasNormalMapTexture) {
				if constexpr (kVerboseSceneImportLogging) { std::cout << "[INFO] Computing tangents for normal mapping...\n"; }
				ComputeTangents(vertices, indices);
			}

			// Create the GPU mesh (VAO, VBO, EBO) without textures yet
			bool hasAlpha = false;
			MeshComponent mesh = CreateMesh(vertices, indices, hasAlpha);

			// OPTIMIZATION: Compute and cache bounding volume once at load time
			mesh.ComputeBoundingVolume();
			mesh.localTransform = glm::mat4(1.0f);
			mesh.sourceNodeIndex = -1;

			// Load material textures and properties for this primitive
			mesh.hasAlpha = false;
			mesh.doubleSided = false;
			mesh.alphaMode = MeshComponent::ALPHA_OPAQUE;
			mesh.alphaCutoff = 0.5f;
			// Create variables to hold the material factors with proper glTF defaults.
			glm::vec4 baseColorFactor(1.0f);  // White
			float metallicFactor = 1.0f;      // glTF metallic-roughness default
			float roughnessFactor = 1.0f;     // Fully rough by default (glTF spec)
			glm::vec3 emissiveFactor(0.0f);   // No emission
			int baseColorTexCoord = 0;
			int normalTexCoord = 0;
			int metallicRoughnessTexCoord = 0;
			int emissiveTexCoord = 0;
			int occlusionTexCoord = 0;
			int specularTexCoord = 0;
			int specularColorTexCoord = 0;
			int transmissionTexCoord = 0;
			const auto materialSetupStart = Clock::now();

			auto setTextureIfPresent = [&](int texIndex, std::shared_ptr<Texture>& destination, bool useSRGB = false) {
				if (texIndex >= 0) {
					destination = loadTextureCached(texIndex, useSRGB);
				}
			};

			if (primitive.material >= 0 && primitive.material < static_cast<int>(materialCache.size())) {
				CachedMaterialData& cached = materialCache[primitive.material];
				if (!cached.initialized) {
					buildCachedMaterialData(model.materials[primitive.material], cached);
					++materialCacheMisses;
				}
				else {
					++materialCacheHits;
				}

				mesh.hasAlpha = cached.hasAlpha;
				mesh.doubleSided = cached.doubleSided;
				mesh.alphaMode = cached.alphaMode;
				mesh.alphaCutoff = cached.alphaCutoff;
				mesh.specularFactor = cached.specularFactor;
				mesh.specularColorFactor = cached.specularColorFactor;
				mesh.emissiveStrength = cached.emissiveStrength;
				mesh.clearcoatFactor = cached.clearcoatFactor;
				mesh.clearcoatRoughnessFactor = cached.clearcoatRoughnessFactor;
				mesh.occlusionStrength = cached.occlusionStrength;
				mesh.normalScale = cached.normalScale;
				mesh.transmissionFactor = cached.transmissionFactor;
				mesh.thicknessFactor = cached.thicknessFactor;
				mesh.attenuationDistance = cached.attenuationDistance;
				mesh.attenuationColor = cached.attenuationColor;
				mesh.ior = cached.ior;

				baseColorFactor = cached.baseColorFactor;
				metallicFactor = cached.metallicFactor;
				roughnessFactor = cached.roughnessFactor;
				emissiveFactor = cached.emissiveFactor;
				baseColorTexCoord = cached.baseColorTexCoord;
				normalTexCoord = cached.normalTexCoord;
				metallicRoughnessTexCoord = cached.metallicRoughnessTexCoord;
				emissiveTexCoord = cached.emissiveTexCoord;
				occlusionTexCoord = cached.occlusionTexCoord;
				specularTexCoord = cached.specularTexCoord;
				specularColorTexCoord = cached.specularColorTexCoord;
				transmissionTexCoord = cached.transmissionTexCoord;
				if (baseColorTexCoord > 0 || normalTexCoord > 0 || metallicRoughnessTexCoord > 0 ||
					emissiveTexCoord > 0 || occlusionTexCoord > 0 || specularTexCoord > 0 ||
					specularColorTexCoord > 0 || transmissionTexCoord > 0) {
					++primitivesUsingUVSet1;
				}

				setTextureIfPresent(cached.diffuseTextureIndex, mesh.diffuseTexture, true);
				setTextureIfPresent(cached.normalTextureIndex, mesh.normalTexture, false);
				setTextureIfPresent(cached.roughnessTextureIndex, mesh.roughnessTexture, false);
				setTextureIfPresent(cached.emissiveTextureIndex, mesh.emissiveTexture, true);
				setTextureIfPresent(cached.occlusionTextureIndex, mesh.occlusionTexture, false);
				setTextureIfPresent(cached.specularTextureIndex, mesh.specularTexture, false);
				setTextureIfPresent(cached.specularColorTextureIndex, mesh.specularColorTexture, true);
				setTextureIfPresent(cached.transmissionTextureIndex, mesh.transmissionTexture, false);
			}
			else {
				// No material specified: use default glTF PBR values
				baseColorFactor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // White diffuse
				metallicFactor = 1.0f;    // glTF metallic-roughness default
				roughnessFactor = 1.0f;   // Fully rough by default
				emissiveFactor = glm::vec3(0.0f);  // No emission
			}

			// Missing textures are intentionally left null and resolved at render time via
			// global default texture bindings plus has*Texture flags.
			const auto materialSetupEnd = Clock::now();
			materialSetupMs += std::chrono::duration<float, std::milli>(materialSetupEnd - materialSetupStart).count();

			//Assign the normalized material contract to the mesh and keep the legacy fields mirrored.
			MaterialDesc normalizedMaterial;
			normalizedMaterial.stableMaterialID = HashStableMaterialKey(path, static_cast<int>(mm), static_cast<int>(p), primitive.material);
			normalizedMaterial.alphaMode = static_cast<MaterialDesc::AlphaMode>(static_cast<uint32_t>(mesh.alphaMode));
			normalizedMaterial.doubleSided = mesh.doubleSided;
			normalizedMaterial.hasAlpha = mesh.hasAlpha;
			normalizedMaterial.baseColorFactor = baseColorFactor;
			normalizedMaterial.metallicFactor = metallicFactor;
			normalizedMaterial.roughnessFactor = roughnessFactor;
			normalizedMaterial.alphaCutoff = mesh.alphaCutoff;
			normalizedMaterial.emissiveFactor = emissiveFactor;
			normalizedMaterial.emissiveStrength = mesh.emissiveStrength;
			normalizedMaterial.specularFactor = mesh.specularFactor;
			normalizedMaterial.specularColorFactor = mesh.specularColorFactor;
			normalizedMaterial.clearcoatFactor = mesh.clearcoatFactor;
			normalizedMaterial.clearcoatRoughnessFactor = mesh.clearcoatRoughnessFactor;
			normalizedMaterial.transmissionFactor = mesh.transmissionFactor;
			normalizedMaterial.thicknessFactor = mesh.thicknessFactor;
			normalizedMaterial.attenuationDistance = mesh.attenuationDistance;
			normalizedMaterial.attenuationColor = mesh.attenuationColor;
			normalizedMaterial.ior = mesh.ior;
			normalizedMaterial.occlusionStrength = mesh.occlusionStrength;
			normalizedMaterial.normalScale = mesh.normalScale;
			normalizedMaterial.baseColorTexCoord = baseColorTexCoord;
			normalizedMaterial.normalTexCoord = normalTexCoord;
			normalizedMaterial.metallicRoughnessTexCoord = metallicRoughnessTexCoord;
			normalizedMaterial.emissiveTexCoord = emissiveTexCoord;
			normalizedMaterial.occlusionTexCoord = occlusionTexCoord;
			normalizedMaterial.specularTexCoord = specularTexCoord;
			normalizedMaterial.specularColorTexCoord = specularColorTexCoord;
			normalizedMaterial.transmissionTexCoord = transmissionTexCoord;
			normalizedMaterial.Normalize();
			mesh.SetMaterialDesc(normalizedMaterial);

			if constexpr (kVerboseSceneImportLogging) {
				std::cout << "[INFO] material factors: "
					<< "baseColorFactor: " << mesh.material.baseColorFactor.x << "," << mesh.material.baseColorFactor.y << "," << mesh.material.baseColorFactor.z << "," << mesh.material.baseColorFactor.w
					<< " metallicFactor: " << mesh.material.metallicFactor
					<< " roughnessFactor: " << mesh.material.roughnessFactor
					<< " emissiveFactor: " << mesh.material.emissiveFactor.x << "," << mesh.material.emissiveFactor.y << "," << mesh.material.emissiveFactor.z
					<< " emissiveStrength: " << mesh.material.emissiveStrength
					<< " specularFactor: " << mesh.material.specularFactor
					<< " clearcoatFactor: " << mesh.material.clearcoatFactor
					<< " clearcoatRoughnessFactor: " << mesh.material.clearcoatRoughnessFactor
					<< " occlusionStrength: " << mesh.material.occlusionStrength
					<< " transmissionFactor: " << mesh.material.transmissionFactor
					<< " thicknessFactor: " << mesh.material.thicknessFactor
					<< " attenuationDistance: " << mesh.material.attenuationDistance
					<< " ior: " << mesh.material.ior
					<< "\n";
			}

			//Add the mesh to the Scene (use move since MeshComponent is move-only)
			meshPrimitivesByGltfMesh[mm].push_back(static_cast<uint32_t>(meshes.size()));
			meshes.emplace_back(std::move(mesh));
		}
	}
	const auto primitiveBuildEnd = Clock::now();
	primitiveBuildMs = std::chrono::duration<float, std::milli>(primitiveBuildEnd - primitiveBuildStart).count();

	hasWeightedSkinData = (primitivesWithMeaningfulSkinData > 0);
	hasSkin = hasDeclaredSkinMetadata && hasWeightedSkinData && !skin.joints.empty();

	std::cout << "[Scene] Import summary for " << m_model_name
		<< ": nodes=" << nodes.size()
		<< ", meshes=" << model.meshes.size()
		<< ", primitives=" << primitiveCount
		<< ", importedNormals=" << primitivesWithImportedNormals
		<< ", generatedNormals=" << primitivesWithGeneratedNormals
		<< ", normalMapsSuppressed=" << primitivesNormalMapSuppressed
		<< ", texCoord1Primitives=" << primitivesWithTexCoord1
		<< ", uvSet1Materials=" << primitivesUsingUVSet1
		<< ", skins=" << model.skins.size()
		<< ", joints=" << totalJoints
		<< ", animations=" << model.animations.size()
		<< ", jointPrimitives=" << primitivesWithJointAttributes
		<< ", weightPrimitives=" << primitivesWithWeightAttributes
		<< ", weightedSkinPrimitives=" << primitivesWithMeaningfulSkinData
		<< ", runtimeSkinned=" << (hasSkin ? "yes" : "no")
		<< std::endl;

	if (hasDeclaredSkinMetadata && !hasSkin) {
		std::cout << "[Scene] Skin metadata found but no meaningful weighted skin data; treating model as static hierarchy for runtime transforms." << std::endl;
	}
	if (!hasSkin) {
		std::cout << "[Scene] Model loaded as non-skinned geometry (runtime skinning disabled)." << std::endl;
	}

	for (size_t nodeIdx = 0; nodeIdx < model.nodes.size() && nodeIdx < nodes.size(); ++nodeIdx) {
		const tinygltf::Node& gltfNode = model.nodes[nodeIdx];
		Scene::NodeInfo& nodeInfo = nodes[nodeIdx];
		nodeInfo.gltfNodeIndex = static_cast<int>(nodeIdx);
		nodeInfo.skinIndex = gltfNode.skin;
		nodeInfo.meshIndices.clear();

		if (gltfNode.mesh >= 0 && gltfNode.mesh < static_cast<int>(meshPrimitivesByGltfMesh.size())) {
			const auto& attachedMeshes = meshPrimitivesByGltfMesh[gltfNode.mesh];
			nodeInfo.meshIndices.insert(nodeInfo.meshIndices.end(), attachedMeshes.begin(), attachedMeshes.end());
		}
	}

	for (size_t nodeIdx = 0; nodeIdx < nodes.size() && nodeIdx < nodeWorldTransforms.size(); ++nodeIdx) {
		const Scene::NodeInfo& nodeInfo = nodes[nodeIdx];
		for (uint32_t meshIndex : nodeInfo.meshIndices) {
			if (meshIndex >= meshes.size()) {
				continue;
			}

			MeshComponent& mesh = meshes[meshIndex];
			if (mesh.sourceNodeIndex >= 0) {
				continue;
			}

			mesh.sourceNodeIndex = static_cast<int>(nodeIdx);
			mesh.localTransform = nodeWorldTransforms[nodeIdx];
		}
	}

	// Some exporters add identity-only wrapper nodes (e.g. material/object shells) under the
	// authored transform node. Promote mesh ownership to the nearest non-identity ancestor so
	// imported renderables keep meaningful local offsets (instead of landing on identity leaves).
	size_t promotedMeshAttachmentCount = 0;
	for (size_t nodeIdx = 0; nodeIdx < nodes.size(); ++nodeIdx) {
		if (nodes[nodeIdx].meshIndices.empty()) {
			continue;
		}

		int currentIndex = static_cast<int>(nodeIdx);
		while (currentIndex >= 0 && currentIndex < static_cast<int>(nodes.size())) {
			Scene::NodeInfo& currentNode = nodes[static_cast<size_t>(currentIndex)];
			if (currentNode.meshIndices.empty()) {
				break;
			}

			if (!IsNearlyIdentityTransform(currentNode.localTransform)) {
				break;
			}

			const int parentIndex = currentNode.parent;
			if (parentIndex < 0 || parentIndex >= static_cast<int>(nodes.size())) {
				break;
			}

			Scene::NodeInfo& parentNode = nodes[static_cast<size_t>(parentIndex)];
			parentNode.meshIndices.insert(parentNode.meshIndices.end(),
				currentNode.meshIndices.begin(),
				currentNode.meshIndices.end());
			promotedMeshAttachmentCount += currentNode.meshIndices.size();
			currentNode.meshIndices.clear();

			currentIndex = parentIndex;
		}
	}

	if (promotedMeshAttachmentCount > 0) {
		std::cout << "[Scene] Promoted " << promotedMeshAttachmentCount
			<< " mesh attachment(s) from identity wrapper nodes to transform-carrying ancestors." << std::endl;
	}

	// (2) Load animations (if any) from the glTF
	const auto animationLoadStart = Clock::now();
	if (!model.animations.empty()) {
		for (const auto& gltfAnim : model.animations) {
			Animation anim;
			anim.name = gltfAnim.name;
			for (const auto& gltfChannel : gltfAnim.channels) {
				int samplerIndex = gltfChannel.sampler;
				if (samplerIndex < 0 || samplerIndex >= (int)gltfAnim.samplers.size()) continue;
				const tinygltf::AnimationSampler& sampler = gltfAnim.samplers[samplerIndex];
				// Retrieve keyframe times
				const tinygltf::Accessor& timeAcc = model.accessors[sampler.input];
				const tinygltf::BufferView& timeView = model.bufferViews[timeAcc.bufferView];
				const tinygltf::Buffer& timeBuf = model.buffers[timeView.buffer];
				const float* timeData = reinterpret_cast<const float*>(&timeBuf.data[timeView.byteOffset + timeAcc.byteOffset]);
				std::vector<float> keyframes(timeData, timeData + timeAcc.count);
				if (!keyframes.empty()) {
					if (anim.channels.empty()) {
						anim.startTime = keyframes.front();
						anim.endTime = keyframes.back();
					}
					else {
						anim.startTime = std::min(anim.startTime, keyframes.front());
						anim.endTime = std::max(anim.endTime, keyframes.back());
					}
				}
				// Retrieve keyframe values (vec3 or vec4)
				const tinygltf::Accessor& valueAcc = model.accessors[sampler.output];
				const tinygltf::BufferView& valueView = model.bufferViews[valueAcc.bufferView];
				const tinygltf::Buffer& valueBuf = model.buffers[valueView.buffer];
				const float* valueData = reinterpret_cast<const float*>(&valueBuf.data[valueView.byteOffset + valueAcc.byteOffset]);
				size_t numComponents = (valueAcc.type == TINYGLTF_TYPE_VEC3 ? 3 : 4);
				std::vector<glm::vec4> values;
				values.reserve(valueAcc.count);
				for (size_t i = 0; i < valueAcc.count; ++i) {
					if (numComponents == 3) {
						values.emplace_back(valueData[i * 3 + 0], valueData[i * 3 + 1], valueData[i * 3 + 2], 0.0f);
					}
					else {
						values.emplace_back(valueData[i * 4 + 0], valueData[i * 4 + 1], valueData[i * 4 + 2], valueData[i * 4 + 3]);
					}
				}

				// Create animation channel
				Animation::Channel channel;
				channel.targetNode = gltfChannel.target_node;

				// Map glTF path to channel type
				if (gltfChannel.target_path == "translation") {
					channel.type = Animation::ChannelType::TRANSLATION;
					channel.path = "translation";
				}
				else if (gltfChannel.target_path == "rotation") {
					channel.type = Animation::ChannelType::ROTATION;
					channel.path = "rotation";
				}
				else if (gltfChannel.target_path == "scale") {
					channel.type = Animation::ChannelType::SCALE;
					channel.path = "scale";
				}
				else if (gltfChannel.target_path == "weights") {
					channel.type = Animation::ChannelType::WEIGHTS;
					channel.path = "weights";
					// For weights, copy data to morphWeights instead of values
					channel.morphWeights.reserve(valueAcc.count * numComponents);
					for (size_t i = 0; i < valueAcc.count; ++i) {
						for (size_t c = 0; c < numComponents; ++c) {
							channel.morphWeights.push_back(valueData[i * numComponents + c]);
						}
					}
				}
				else {
					// Unknown path, skip this channel
					continue;
				}

				// Set interpolation mode
				if (sampler.interpolation == "LINEAR") {
					channel.interpolation = Animation::InterpolationMode::LINEAR;
				}
				else if (sampler.interpolation == "STEP") {
					channel.interpolation = Animation::InterpolationMode::STEP;
				}
				else if (sampler.interpolation == "CUBICSPLINE") {
					channel.interpolation = Animation::InterpolationMode::CUBICSPLINE;
				}
				else {
					// Default to linear
					channel.interpolation = Animation::InterpolationMode::LINEAR;
				}

				channel.keyframes = std::move(keyframes);
				channel.values = std::move(values);
				channel.samplerIndex = samplerIndex;

				anim.channels.push_back(std::move(channel));
			}

			// Update animation duration
			anim.UpdateDuration();
			animations.push_back(std::move(anim));
		}
	}
	const auto animationLoadEnd = Clock::now();
	animationLoadMs = std::chrono::duration<float, std::milli>(animationLoadEnd - animationLoadStart).count();

	const auto loadEnd = Clock::now();
	const float totalLoadMs = std::chrono::duration<float, std::milli>(loadEnd - loadStart).count();
	std::cout << "[Scene] Load diagnostics for " << m_model_name
		<< ": totalMs=" << totalLoadMs
		<< ", parseMs=" << parseMs
		<< ", nodeBuildMs=" << nodeBuildMs
		<< ", primitiveBuildMs=" << primitiveBuildMs
		<< ", materialSetupMs=" << materialSetupMs
		<< ", animationLoadMs=" << animationLoadMs
		<< ", materialCacheHits=" << materialCacheHits
		<< ", materialCacheMisses=" << materialCacheMisses
		<< ", textureRefs=" << textureLoadRequests
		<< ", uniqueTextureLoads=" << uniqueTextureLoads
		<< ", totalVertices=" << totalVertexCount
		<< ", totalIndices=" << totalIndexCount
		<< std::endl;

	AppendLoadDiagnosticsCsv(m_model_name,
		totalLoadMs,
		parseMs,
		nodeBuildMs,
		primitiveBuildMs,
		materialSetupMs,
		animationLoadMs,
		nodes.size(),
		model.meshes.size(),
		primitiveCount,
		totalVertexCount,
		totalIndexCount);

	return true;
}

void Scene::Draw() {
	GLint currentProgram = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);

	// Get uniform locations for material factors
	GLint locBaseColor = glGetUniformLocation(currentProgram, "baseColorFactor");
	GLint locMetallic = glGetUniformLocation(currentProgram, "metallicFactor");
	GLint locRoughness = glGetUniformLocation(currentProgram, "roughnessFactor");
	GLint locEmissive = glGetUniformLocation(currentProgram, "emissiveFactor");
	GLint locEmissiveStrength = glGetUniformLocation(currentProgram, "emissiveStrength");
	GLint locOcclusionStrength = glGetUniformLocation(currentProgram, "occlusionStrength");
	GLint locNormalScale = glGetUniformLocation(currentProgram, "normalScale");
	GLint locAlphaCutoff = glGetUniformLocation(currentProgram, "alphaCutoff");

	// KHR_materials_specular extension
	GLint locSpecularFactor = glGetUniformLocation(currentProgram, "specularFactor");
	GLint locSpecularColorFactor = glGetUniformLocation(currentProgram, "specularColorFactor");
	GLint locClearcoat = glGetUniformLocation(currentProgram, "clearcoatFactor");
	GLint locClearcoatRoughness = glGetUniformLocation(currentProgram, "clearcoatRoughnessFactor");

	// KHR_materials_transmission extension
	GLint locTransmission = glGetUniformLocation(currentProgram, "transmissionFactor");
	GLint locThickness = glGetUniformLocation(currentProgram, "thicknessFactor");
	GLint locAttenuationDistance = glGetUniformLocation(currentProgram, "attenuationDistance");
	GLint locAttenuationColor = glGetUniformLocation(currentProgram, "attenuationColor");

	// KHR_materials_ior extension
	GLint locIOR = glGetUniformLocation(currentProgram, "ior");

	// Texture presence flags
	GLint locHasBaseColor = glGetUniformLocation(currentProgram, "hasBaseColorTexture");
	GLint locHasNormal = glGetUniformLocation(currentProgram, "hasNormalTexture");
	GLint locHasMR = glGetUniformLocation(currentProgram, "hasMetallicRoughnessTexture");
	GLint locHasEmissive = glGetUniformLocation(currentProgram, "hasEmissiveTexture");
	GLint locHasOcclusion = glGetUniformLocation(currentProgram, "hasOcclusionTexture");
	GLint locHasSpecular = glGetUniformLocation(currentProgram, "hasSpecularTexture");
	GLint locHasSpecularColor = glGetUniformLocation(currentProgram, "hasSpecularColorTexture");
	GLint locHasTransmission = glGetUniformLocation(currentProgram, "hasTransmissionTexture");

	// Get texture sampler locations
	GLint locDiffuse = glGetUniformLocation(currentProgram, "texture_diffuse");
	GLint locNormal = glGetUniformLocation(currentProgram, "texture_normal");
	GLint locMetallicRoughness = glGetUniformLocation(currentProgram, "texture_metallic_roughness");
	GLint locEmissiveTex = glGetUniformLocation(currentProgram, "texture_emissive");
	GLint locOcclusion = glGetUniformLocation(currentProgram, "texture_occlusion");
	GLint locSpecularTex = glGetUniformLocation(currentProgram, "texture_specular");
	GLint locSpecularColorTex = glGetUniformLocation(currentProgram, "texture_specular_color");
	GLint locTransmissionTex = glGetUniformLocation(currentProgram, "texture_transmission");

	// Set texture samplers once
	if (locDiffuse != -1) glUniform1i(locDiffuse, 0);
	if (locNormal != -1) glUniform1i(locNormal, 1);
	if (locMetallicRoughness != -1) glUniform1i(locMetallicRoughness, 2);
	if (locEmissiveTex != -1) glUniform1i(locEmissiveTex, 3);
	if (locOcclusion != -1) glUniform1i(locOcclusion, 4);
	if (locSpecularTex != -1) glUniform1i(locSpecularTex, 5);
	if (locSpecularColorTex != -1) glUniform1i(locSpecularColorTex, 6);
	if (locTransmissionTex != -1) glUniform1i(locTransmissionTex, 7);

	for (auto& mesh : meshes) {
		const MaterialDesc material = mesh.GetMaterialDesc();

		// Upload material factor uniforms
		if (locBaseColor != -1)
			glUniform4fv(locBaseColor, 1, glm::value_ptr(material.baseColorFactor));
		if (locMetallic != -1)
			glUniform1f(locMetallic, material.metallicFactor);
		if (locRoughness != -1)
			glUniform1f(locRoughness, material.roughnessFactor);
		if (locEmissive != -1)
			glUniform3fv(locEmissive, 1, glm::value_ptr(material.emissiveFactor));
		if (locEmissiveStrength != -1)
			glUniform1f(locEmissiveStrength, material.emissiveStrength);
		if (locOcclusionStrength != -1)
			glUniform1f(locOcclusionStrength, material.occlusionStrength);
		if (locNormalScale != -1)
			glUniform1f(locNormalScale, material.normalScale);
		if (locAlphaCutoff != -1)
			glUniform1f(locAlphaCutoff, material.alphaCutoff);

		// KHR_materials_specular
		if (locSpecularFactor != -1)
			glUniform1f(locSpecularFactor, material.specularFactor);
		if (locSpecularColorFactor != -1)
			glUniform3fv(locSpecularColorFactor, 1, glm::value_ptr(material.specularColorFactor));
		if (locClearcoat != -1)
			glUniform1f(locClearcoat, material.clearcoatFactor);
		if (locClearcoatRoughness != -1)
			glUniform1f(locClearcoatRoughness, material.clearcoatRoughnessFactor);

		// KHR_materials_transmission
		if (locTransmission != -1)
			glUniform1f(locTransmission, material.transmissionFactor);
		if (locThickness != -1)
			glUniform1f(locThickness, material.thicknessFactor);
		if (locAttenuationDistance != -1)
			glUniform1f(locAttenuationDistance, material.attenuationDistance);
		if (locAttenuationColor != -1)
			glUniform3fv(locAttenuationColor, 1, glm::value_ptr(material.attenuationColor));

		// KHR_materials_ior
		if (locIOR != -1)
			glUniform1f(locIOR, material.ior);

		// Set texture presence flags
		if (locHasBaseColor != -1)
			glUniform1i(locHasBaseColor, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
		if (locHasNormal != -1)
			glUniform1i(locHasNormal, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
		if (locHasMR != -1)
			glUniform1i(locHasMR, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
		if (locHasEmissive != -1)
			glUniform1i(locHasEmissive, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
		if (locHasOcclusion != -1)
			glUniform1i(locHasOcclusion, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
		if (locHasSpecular != -1)
			glUniform1i(locHasSpecular, (mesh.specularTexture && mesh.specularTexture->IsValid()) ? 1 : 0);
		if (locHasSpecularColor != -1)
			glUniform1i(locHasSpecularColor, (mesh.specularColorTexture && mesh.specularColorTexture->IsValid()) ? 1 : 0);
		if (locHasTransmission != -1)
			glUniform1i(locHasTransmission, (mesh.transmissionTexture && mesh.transmissionTexture->IsValid()) ? 1 : 0);

		// Set render states
		glEnable(GL_DEPTH_TEST);
		if (mesh.hasAlpha) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		else {
			glDisable(GL_BLEND);
		}
		if (mesh.doubleSided) {
			glDisable(GL_CULL_FACE);
		}
		else {
			glEnable(GL_CULL_FACE);
		}

		// Bind textures to specific units for glTF PBR using new Texture API
		if (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) {
			mesh.diffuseTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_BASE_COLOR);
		}

		if (mesh.normalTexture && mesh.normalTexture->IsValid()) {
			mesh.normalTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_NORMAL);
		}

		if (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) {
			mesh.roughnessTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_METALLIC_ROUGHNESS);
		}

		if (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) {
			mesh.emissiveTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_EMISSIVE);
		}

		if (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) {
			mesh.occlusionTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_OCCLUSION);
		}

		if (mesh.specularTexture && mesh.specularTexture->IsValid()) {
			mesh.specularTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_SPECULAR);
		}

		if (mesh.specularColorTexture && mesh.specularColorTexture->IsValid()) {
			mesh.specularColorTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_SPECULAR_COLOR);
		}

		if (mesh.transmissionTexture && mesh.transmissionTexture->IsValid()) {
			mesh.transmissionTexture->Bind(GL_TEXTURE0 + TextureUnits::MATERIAL_TRANSMISSION);
		}

		glBindVertexArray(mesh.VAO);
		glDrawElements(GL_TRIANGLES, (GLsizei)mesh.indexCount, GL_UNSIGNED_INT, 0);
		glBindVertexArray(0);

		// Reset cull face state
		if (mesh.doubleSided) {
			glEnable(GL_CULL_FACE);
		}
	}
}


void Scene::ComputeTangents(std::vector<Vertex>& vertices,
	const std::vector<unsigned int>& indices) {
	// Initialize containers for accumulated tangents/bitangents
	std::vector<glm::vec3> tanSum(vertices.size(), glm::vec3(0.0f));
	std::vector<glm::vec3> bitanSum(vertices.size(), glm::vec3(0.0f));

	// Iterate over each triangle and accumulate tangents
	for (size_t idx = 0; idx < indices.size(); idx += 3) {
		unsigned int i0 = indices[idx];
		unsigned int i1 = indices[idx + 1];
		unsigned int i2 = indices[idx + 2];
		const glm::vec3& p0 = vertices[i0].position;
		const glm::vec3& p1 = vertices[i1].position;
		const glm::vec3& p2 = vertices[i2].position;
		const glm::vec2& uv0 = vertices[i0].texCoord;
		const glm::vec2& uv1 = vertices[i1].texCoord;
		const glm::vec2& uv2 = vertices[i2].texCoord;
		// Edges and UV deltas
		glm::vec3 e1 = p1 - p0;
		glm::vec3 e2 = p2 - p0;
		glm::vec2 dUV1 = uv1 - uv0;
		glm::vec2 dUV2 = uv2 - uv0;
		float f = 1.0f;
		float det = dUV1.x * dUV2.y - dUV2.x * dUV1.y;
		if (fabs(det) > 1e-6f) {
			f = 1.0f / det;
		}
		// Tangent and bitangent for this face
		glm::vec3 faceTangent = f * (e1 * dUV2.y - e2 * dUV1.y);
		glm::vec3 faceBitangent = f * (-e1 * dUV2.x + e2 * dUV1.x);
		// Accumulate into each vertex of the triangle
		tanSum[i0] += faceTangent;
		tanSum[i1] += faceTangent;
		tanSum[i2] += faceTangent;
		bitanSum[i0] += faceBitangent;
		bitanSum[i1] += faceBitangent;
		bitanSum[i2] += faceBitangent;
	}

	// Normalize and orthogonalize tangents, compute handedness (w)
	for (size_t i = 0; i < vertices.size(); ++i) {
		glm::vec3 N = glm::normalize(vertices[i].normal);
		glm::vec3 T = glm::normalize(tanSum[i] - N * glm::dot(N, tanSum[i])); // orthonormalize
		// Compute bitangent from N and T, then determine if we need to flip it
		glm::vec3 B = glm::normalize(glm::cross(N, T));
		float handedness = (glm::dot(B, glm::normalize(bitanSum[i])) < 0.0f) ? -1.0f : 1.0f;
		vertices[i].tangent = glm::vec4(T, handedness);
	}
	if constexpr (kVerboseSceneImportLogging) { std::cout << "[INFO] Tangents computed for normal mapping.\n"; }
}
std::pair<glm::vec3, glm::vec3> Scene::GetBoundingBox() const {
	if (meshes.empty()) {
		return { glm::vec3(0), glm::vec3(0) };
	}
	glm::vec3 minB(std::numeric_limits<float>::max());
	glm::vec3 maxB(-std::numeric_limits<float>::max());
	bool valid = false;

   for (const auto& mc : meshes) {
		if (!mc.boundingVolumeValid) {
			continue;
		}

		if (!std::isfinite(mc.boundingMin.x) || !std::isfinite(mc.boundingMin.y) || !std::isfinite(mc.boundingMin.z) ||
			!std::isfinite(mc.boundingMax.x) || !std::isfinite(mc.boundingMax.y) || !std::isfinite(mc.boundingMax.z)) {
			continue;
		}

		minB = glm::min(minB, mc.boundingMin);
		maxB = glm::max(maxB, mc.boundingMax);
		valid = true;
	}

	if (!valid) {
		return { glm::vec3(0), glm::vec3(0) };
	}
	return { minB, maxB };
}

void Scene::Cleanup() {
	for (auto& mesh : meshes) {
		mesh.Cleanup();
	}
	meshes.clear();
}
