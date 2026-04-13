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
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cstdint>
#include "stb_image.h"


// HELPER CONSTANTS & STRUCTS

static constexpr int MAX_INFLUENCES = 4;

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
		std::memcpy(glm::value_ptr(localMat), gltfNode.matrix.data(), 16 * sizeof(float));
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

static void PopulateSceneNodesAndWorldTransforms(const tinygltf::Model& model, std::vector<Scene::NodeInfo>& outNodes, std::vector<glm::mat4>& outWorldTransforms)
{
	outNodes.resize(model.nodes.size());
	outWorldTransforms.assign(model.nodes.size(), glm::mat4(1.0f));
	std::vector<bool> worldComputed(model.nodes.size(), false);

	for (size_t i = 0; i < model.nodes.size(); ++i) {
		const tinygltf::Node& gltfNode = model.nodes[i];
		Scene::NodeInfo& nodeInfo = outNodes[i];
		nodeInfo.name = gltfNode.name;
		nodeInfo.localTransform = BuildNodeLocalTransform(gltfNode);
		nodeInfo.parent = -1;
		nodeInfo.children.clear();
	}

	for (size_t i = 0; i < model.nodes.size(); ++i) {
		const tinygltf::Node& gltfNode = model.nodes[i];
		for (int childIdx : gltfNode.children) {
			if (childIdx >= 0 && childIdx < static_cast<int>(model.nodes.size())) {
				outNodes[i].children.push_back(childIdx);
				outNodes[childIdx].parent = static_cast<int>(i);
			}
		}
	}

	std::function<glm::mat4(int)> computeWorldTransform = [&](int nodeIndex) -> glm::mat4 {
		Scene::NodeInfo& nodeInfo = outNodes[nodeIndex];
		if (worldComputed[nodeIndex]) {
			return outWorldTransforms[nodeIndex];
		}
		if (nodeInfo.parent < 0) {
			outWorldTransforms[nodeIndex] = nodeInfo.localTransform;
			worldComputed[nodeIndex] = true;
			return outWorldTransforms[nodeIndex];
		}
		glm::mat4 parentWorld = computeWorldTransform(nodeInfo.parent);
		outWorldTransforms[nodeIndex] = parentWorld * nodeInfo.localTransform;
		worldComputed[nodeIndex] = true;
		return outWorldTransforms[nodeIndex];
	};

	for (size_t i = 0; i < model.nodes.size(); ++i) {
		computeWorldTransform(static_cast<int>(i));
	}
}

static int FindSceneRootNodeIndex(const std::vector<Scene::NodeInfo>& nodes, int nodeIndex)
{
	int current = nodeIndex;
	while (current >= 0 && current < static_cast<int>(nodes.size()) && nodes[current].parent >= 0) {
		current = nodes[current].parent;
	}
	return current;
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
	const float* dataPtr = reinterpret_cast<const float*>(
		&buf.data[bv.byteOffset + acc.byteOffset]);

	for (size_t i = 0; i < acc.count; i++) {
		setVertexData(i, &dataPtr[i * numComponents]);
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
	out.resize(count);

	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		const unsigned int* p = reinterpret_cast<const unsigned int*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		const unsigned short* p = reinterpret_cast<const unsigned short*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		const unsigned char* p = reinterpret_cast<const unsigned char*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_INT) {
		const int* p = reinterpret_cast<const int*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
		const short* p = reinterpret_cast<const short*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
		const char* p = reinterpret_cast<const char*>(indexData);
		for (size_t i = 0; i < count; i++) out[i] = p[i];
	}
	else {
		// Unsupported index type
		out.clear();
	}
	return out;
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
 */
static std::shared_ptr<Texture> LoadTextureFromGLTF(const tinygltf::Model& model, int texIndex, bool isNormalMap = false)
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
		internalFormat = GL_RGB8;
	}
	else if (image.component == 4) {
		format = GL_RGBA;
		internalFormat = GL_RGBA8;
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
	tinygltf::TinyGLTF loader;
	tinygltf::Model model;
	std::string err, warn;
	bool ret = false;

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

	// Clear any existing data in this Scene
	meshes.clear();
	animations.clear();
	nodes.clear();
	hasSkin = false;
	hasTangents = false;
	skin.joints.clear();
	skin.inverseBindMatrices.clear();
	m_model_name = path.substr(path.find_last_of("/\\") + 1);

	std::vector<glm::mat4> nodeWorldTransforms;
	PopulateSceneNodesAndWorldTransforms(model, nodes, nodeWorldTransforms);

	// If there's a skin, note the total joint count for blending weights
	int totalJoints = 0;
	if (!model.skins.empty()) {
		const tinygltf::Skin& gltfSkin = model.skins[0]; // Use first skin
		totalJoints = static_cast<int>(gltfSkin.joints.size());
		hasSkin = true;

		// Load skin data
		skin.joints = gltfSkin.joints;

		// Load inverse bind matrices
		if (gltfSkin.inverseBindMatrices >= 0) {
			const tinygltf::Accessor& accessor = model.accessors[gltfSkin.inverseBindMatrices];
			const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
			const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

			const float* matrixData = reinterpret_cast<const float*>(
				&buffer.data[bufferView.byteOffset + accessor.byteOffset]);

			skin.inverseBindMatrices.reserve(accessor.count);
			for (size_t i = 0; i < accessor.count; ++i) {
				glm::mat4 matrix;
				std::memcpy(glm::value_ptr(matrix), &matrixData[i * 16], 16 * sizeof(float));
				skin.inverseBindMatrices.push_back(matrix);
			}

			std::cout << "[Scene] Loaded skin with " << skin.joints.size()
				<< " joints and " << skin.inverseBindMatrices.size()
				<< " inverse bind matrices" << std::endl;
		}
	}

	// (1) Parse all meshes/primitives from the glTF model.
	// We expand them per glTF node so node-local transforms survive import.
	for (size_t mm = 0; mm < model.meshes.size(); mm++) {
		const tinygltf::Mesh& gltfMesh = model.meshes[mm];
		std::vector<int> referencingNodes;
		for (size_t nodeIdx = 0; nodeIdx < model.nodes.size(); ++nodeIdx) {
			if (model.nodes[nodeIdx].mesh == static_cast<int>(mm)) {
				referencingNodes.push_back(static_cast<int>(nodeIdx));
			}
		}
		if (referencingNodes.empty()) {
			referencingNodes.push_back(-1);
		}

		for (int nodeIndex : referencingNodes) {
			glm::mat4 meshLocalTransform = glm::mat4(1.0f);
			if (nodeIndex >= 0 && nodeIndex < static_cast<int>(nodeWorldTransforms.size())) {
				meshLocalTransform = nodeWorldTransforms[nodeIndex];
				const int rootNodeIndex = FindSceneRootNodeIndex(nodes, nodeIndex);
				if (rootNodeIndex >= 0 && rootNodeIndex < static_cast<int>(nodeWorldTransforms.size())) {
					glm::mat4 rootInverse = glm::inverse(nodeWorldTransforms[rootNodeIndex]);
					bool validInverse = true;
					for (int c = 0; c < 4 && validInverse; ++c) {
						for (int r = 0; r < 4 && validInverse; ++r) {
							if (!std::isfinite(rootInverse[c][r])) {
								validInverse = false;
							}
						}
					}
					if (validInverse) {
						meshLocalTransform = rootInverse * meshLocalTransform;
					}
				}
			}

			for (size_t p = 0; p < gltfMesh.primitives.size(); p++) {
			const tinygltf::Primitive& primitive = gltfMesh.primitives[p];
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
			vertices.resize(vertexCount);
			vertices.shrink_to_fit();

			// A) Read vertex attributes (positions, normals, tangents, texcoords, skin joints/weights)
			ReadFloatAttribute(model, primitive, "POSITION", 3, [&](size_t i, const float* data) {
				vertices[i].position = glm::vec3(data[0], data[1], data[2]);
				});
			ReadFloatAttribute(model, primitive, "NORMAL", 3, [&](size_t i, const float* data) {
				vertices[i].normal = glm::vec3(data[0], data[1], data[2]);
				});
			ReadFloatAttribute(model, primitive, "TANGENT", 4, [&](size_t i, const float* data) {
				// Store tangent XYZ and handedness (data[3]) in the vertex
				vertices[i].tangent = glm::vec4(data[0], data[1], data[2], data[3]);
				});
			ReadFloatAttribute(model, primitive, "TEXCOORD_0", 2, [&](size_t i, const float* data) {
				vertices[i].texCoord = glm::vec2(data[0], data[1]);
				});

			// Skinning data (up to 8 weights, merged into 4)
			const unsigned short* joints0Data = nullptr;
			const unsigned short* joints1Data = nullptr;
			const float* weights0Data = nullptr;
			const float* weights1Data = nullptr;
			// JOINTS_0
			auto j0It = primitive.attributes.find("JOINTS_0");
			if (j0It != primitive.attributes.end()) {
				const auto& acc = model.accessors[j0It->second];
				const auto& view = model.bufferViews[acc.bufferView];
				joints0Data = reinterpret_cast<const unsigned short*>(&model.buffers[view.buffer].data[view.byteOffset + acc.byteOffset]);
			}
			// JOINTS_1
			auto j1It = primitive.attributes.find("JOINTS_1");
			if (j1It != primitive.attributes.end()) {
				const auto& acc = model.accessors[j1It->second];
				const auto& view = model.bufferViews[acc.bufferView];
				joints1Data = reinterpret_cast<const unsigned short*>(&model.buffers[view.buffer].data[view.byteOffset + acc.byteOffset]);
			}
			// WEIGHTS_0
			auto w0It = primitive.attributes.find("WEIGHTS_0");
			if (w0It != primitive.attributes.end()) {
				const auto& acc = model.accessors[w0It->second];
				const auto& view = model.bufferViews[acc.bufferView];
				weights0Data = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + acc.byteOffset]);
			}
			// WEIGHTS_1
			auto w1It = primitive.attributes.find("WEIGHTS_1");
			if (w1It != primitive.attributes.end()) {
				const auto& acc = model.accessors[w1It->second];
				const auto& view = model.bufferViews[acc.bufferView];
				weights1Data = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + acc.byteOffset]);
			}
			// Merge up to 8 bone influences into 4 (standard)
			if (joints0Data || joints1Data) {
				for (size_t i = 0; i < vertexCount; i++) {
					glm::ivec4 finalIDs(0);
					glm::vec4 finalWeights(0.0f);
					MergeBoneData(joints0Data, joints1Data,
						weights0Data, weights1Data,
						i, finalIDs, finalWeights,
						totalJoints);
					vertices[i].boneIDs = finalIDs;
					vertices[i].boneWeights = finalWeights;
				}
			}
			if (primitive.attributes.find("TANGENT") != primitive.attributes.end()) {
				hasTangents = true;
				ReadFloatAttribute(model, primitive, "TANGENT", 4,
					[&](size_t i, const float* data) {
						vertices[i].tangent = glm::vec4(data[0], data[1], data[2], data[3]);
					});
			}
			else {
				// Initialize tangent.w to 1 by default to avoid undefined data
				for (auto& v : vertices) {
					v.tangent = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				}
			}
			// Read index data for this primitive
			std::vector<unsigned int> indices = ReadIndices(model, primitive.indices);

			// **Compute tangents if normal mapping is used but tangents were not provided:**
			int materialIndex = primitive.material;
			bool needsTangents = !hasTangents;
			bool hasNormalMapTexture = false;
			if (materialIndex >= 0 && materialIndex < model.materials.size()) {
				const tinygltf::Material& mat = model.materials[materialIndex];
				if (mat.normalTexture.index >= 0) {
					hasNormalMapTexture = true;
				}
			}
			if (needsTangents && hasNormalMapTexture) {
				std::cout << "[INFO] Computing tangents for normal mapping...\n";
				ComputeTangents(vertices, indices);
			}

			// Create the GPU mesh (VAO, VBO, EBO) without textures yet
			bool hasAlpha = false;
			MeshComponent mesh = CreateMesh(vertices, indices, hasAlpha);

			// OPTIMIZATION: Compute and cache bounding volume once at load time
			mesh.ComputeBoundingVolume();
			mesh.localTransform = meshLocalTransform;
			mesh.sourceNodeIndex = nodeIndex;

			// Load material textures and properties for this primitive
			mesh.hasAlpha = false;
			mesh.doubleSided = false;
			const tinygltf::Material* pMaterial = nullptr;
			//Create variables to hold the material factors with proper glTF defaults.
			glm::vec4 baseColorFactor(1.0f);  // White
			float metallicFactor = 0.0f;      // Non-metallic by default (glTF spec)
			float roughnessFactor = 1.0f;     // Fully rough by default (glTF spec)
			glm::vec3 emissiveFactor(0.0f);   // No emission

			if (primitive.material >= 0 && primitive.material < (int)model.materials.size()) {
				pMaterial = &model.materials[primitive.material];
				const tinygltf::Material& mat = *pMaterial;
				// Alpha mode: mark transparent objects
				if (mat.alphaMode == "BLEND") {
					mesh.hasAlpha = true;
					mesh.alphaMode = MeshComponent::ALPHA_BLEND;
					std::cout << "[INFO] Material uses alpha blending.\n";
				}
				else if (mat.alphaMode == "MASK") {
					mesh.alphaMode = MeshComponent::ALPHA_MASK;
					if (mat.alphaCutoff >= 0.0) {
						mesh.alphaCutoff = static_cast<float>(mat.alphaCutoff);
					}
					std::cout << "[INFO] Material uses alpha masking with cutoff: " << mesh.alphaCutoff << "\n";
				}
				else {
					mesh.alphaMode = MeshComponent::ALPHA_OPAQUE;
				}

				// Double-sided material (disables backface culling)
				if (mat.doubleSided) {
					mesh.doubleSided = true;
				}
				// Base Color (albedo) texture
				if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
					int texIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
					mesh.diffuseTexture = LoadTextureFromGLTF(model, texIndex);
					std::cout << "[INFO] Using base color texture for diffuse map.\n";
				}
				// Normal map texture
				if (mat.normalTexture.index >= 0) {
					int texIndex = mat.normalTexture.index;
					mesh.normalTexture = LoadTextureFromGLTF(model, texIndex);
					std::cout << "[INFO] Using normal map texture.\n";
				}
				// Metallic-Roughness texture (single texture containing both)
				if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
					int texIndex = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
					mesh.roughnessTexture = LoadTextureFromGLTF(model, texIndex);
					std::cout << "[INFO] Using metallic-roughness texture for roughness map.\n";
				}
				// Emissive texture
				if (mat.emissiveTexture.index >= 0) {
					int texIndex = mat.emissiveTexture.index;
					mesh.emissiveTexture = LoadTextureFromGLTF(model, texIndex);
					std::cout << "[INFO] Using emissive texture.\n";
				}
				// Occlusion texture (ambient occlusion, usually in R channel)
				if (mat.occlusionTexture.index >= 0) {
					int texIndex = mat.occlusionTexture.index;
					mesh.occlusionTexture = LoadTextureFromGLTF(model, texIndex);
					std::cout << "[INFO] Using occlusion texture.\n";
				}

				// EXTENSION: KHR_materials_specular - Extract specular texture and factors
				if (mat.extensions.count("KHR_materials_specular")) {
					const auto& specExt = mat.extensions.at("KHR_materials_specular");

					// Extract specular texture (scalar factor texture)
					if (specExt.Has("specularTexture") && specExt.Get("specularTexture").IsObject()) {
						const auto& specTex = specExt.Get("specularTexture");
						if (specTex.Has("index") && specTex.Get("index").IsInt()) {
							int texIndex = specTex.Get("index").Get<int>();
							if (texIndex >= 0 && texIndex < (int)model.textures.size()) {
								mesh.specularTexture = LoadTextureFromGLTF(model, texIndex);
								std::cout << "[INFO] Using KHR_materials_specular texture.\n";
							}
						}
					}

					// Extract specular color texture
					if (specExt.Has("specularColorTexture") && specExt.Get("specularColorTexture").IsObject()) {
						const auto& specColorTex = specExt.Get("specularColorTexture");
						if (specColorTex.Has("index") && specColorTex.Get("index").IsInt()) {
							int texIndex = specColorTex.Get("index").Get<int>();
							if (texIndex >= 0 && texIndex < (int)model.textures.size()) {
								mesh.specularColorTexture = LoadTextureFromGLTF(model, texIndex);
								std::cout << "[INFO] Using KHR_materials_specular color texture.\n";
							}
						}
					}

					// Extract specular factors
					if (specExt.Has("specularFactor") && specExt.Get("specularFactor").IsNumber()) {
						mesh.specularFactor.x = static_cast<float>(specExt.Get("specularFactor").Get<double>());
						mesh.specularFactor.y = mesh.specularFactor.x; // uniform scalar
						mesh.specularFactor.z = mesh.specularFactor.x;
					}

					if (specExt.Has("specularColorFactor") && specExt.Get("specularColorFactor").IsArray()) {
						const auto& colorArray = specExt.Get("specularColorFactor");
						if (colorArray.ArrayLen() >= 3) {
							mesh.specularColorFactor.x = static_cast<float>(colorArray.Get(0).Get<double>());
							mesh.specularColorFactor.y = static_cast<float>(colorArray.Get(1).Get<double>());
							mesh.specularColorFactor.z = static_cast<float>(colorArray.Get(2).Get<double>());
						}
					}
					std::cout << "[INFO] Applied KHR_materials_specular extension factors.\n";
				}

				// EXTENSION: KHR_materials_transmission - Extract transmission properties
				if (mat.extensions.count("KHR_materials_transmission")) {
					const auto& transExt = mat.extensions.at("KHR_materials_transmission");

					// Extract transmission factor
					if (transExt.Has("transmissionFactor") && transExt.Get("transmissionFactor").IsNumber()) {
						mesh.transmissionFactor = static_cast<float>(transExt.Get("transmissionFactor").Get<double>());
						// Materials with transmission should be treated as transparent
						if (mesh.transmissionFactor > 0.0f) {
							mesh.hasAlpha = true;
							if (mesh.alphaMode == MeshComponent::ALPHA_OPAQUE) {
								mesh.alphaMode = MeshComponent::ALPHA_BLEND;
							}
						}
						std::cout << "[INFO] Applied KHR_materials_transmission factor: " << mesh.transmissionFactor << "\n";
					}

					// Extract transmission texture
					if (transExt.Has("transmissionTexture") && transExt.Get("transmissionTexture").IsObject()) {
						const auto& transTex = transExt.Get("transmissionTexture");
						if (transTex.Has("index") && transTex.Get("index").IsInt()) {
							int texIndex = transTex.Get("index").Get<int>();
							if (texIndex >= 0 && texIndex < (int)model.textures.size()) {
								mesh.transmissionTexture = LoadTextureFromGLTF(model, texIndex);
								std::cout << "[INFO] Using KHR_materials_transmission texture.\n";
							}
						}
					}
				}

				// EXTENSION: KHR_materials_ior - Extract index of refraction
				if (mat.extensions.count("KHR_materials_ior")) {
					const auto& iorExt = mat.extensions.at("KHR_materials_ior");
					if (iorExt.Has("ior") && iorExt.Get("ior").IsNumber()) {
						mesh.ior = static_cast<float>(iorExt.Get("ior").Get<double>());
						std::cout << "[INFO] Applied KHR_materials_ior: " << mesh.ior << "\n";
					}
				}

				if (mat.extensions.count("KHR_materials_volume")) {
					const auto& volumeExt = mat.extensions.at("KHR_materials_volume");
					if (volumeExt.Has("thicknessFactor") && volumeExt.Get("thicknessFactor").IsNumber()) {
						mesh.thicknessFactor = static_cast<float>(volumeExt.Get("thicknessFactor").Get<double>());
					}
					if (volumeExt.Has("attenuationDistance") && volumeExt.Get("attenuationDistance").IsNumber()) {
						mesh.attenuationDistance = static_cast<float>(volumeExt.Get("attenuationDistance").Get<double>());
					}
					if (volumeExt.Has("attenuationColor") && volumeExt.Get("attenuationColor").IsArray()) {
						const auto& colorArray = volumeExt.Get("attenuationColor");
						if (colorArray.ArrayLen() >= 3) {
							mesh.attenuationColor = glm::vec3(
								static_cast<float>(colorArray.Get(0).Get<double>()),
								static_cast<float>(colorArray.Get(1).Get<double>()),
								static_cast<float>(colorArray.Get(2).Get<double>())
							);
						}
					}
				}

				// EXTENSION: KHR_materials_emissive_strength
				if (mat.extensions.count("KHR_materials_emissive_strength")) {
					const auto& emissiveExt = mat.extensions.at("KHR_materials_emissive_strength");
					if (emissiveExt.Has("emissiveStrength") && emissiveExt.Get("emissiveStrength").IsNumber()) {
						mesh.emissiveStrength = static_cast<float>(emissiveExt.Get("emissiveStrength").Get<double>());
					}
				}

				// EXTENSION: KHR_materials_clearcoat
				if (mat.extensions.count("KHR_materials_clearcoat")) {
					const auto& clearcoatExt = mat.extensions.at("KHR_materials_clearcoat");
					if (clearcoatExt.Has("clearcoatFactor") && clearcoatExt.Get("clearcoatFactor").IsNumber()) {
						mesh.clearcoatFactor = static_cast<float>(clearcoatExt.Get("clearcoatFactor").Get<double>());
					}
					if (clearcoatExt.Has("clearcoatRoughnessFactor") && clearcoatExt.Get("clearcoatRoughnessFactor").IsNumber()) {
						mesh.clearcoatRoughnessFactor = static_cast<float>(clearcoatExt.Get("clearcoatRoughnessFactor").Get<double>());
					}
				}

				// Extract occlusion strength from standard glTF material
				if (mat.occlusionTexture.index >= 0) {
					// Default occlusion strength is 1.0, but can be overridden
					mesh.occlusionStrength = 1.0f;
					// Check if there are additional properties in occlusionTexture
					if (mat.extensions.count("occlusionTexture")) {
						const auto& occExt = mat.extensions.at("occlusionTexture");
						if (occExt.Has("strength") && occExt.Get("strength").IsNumber()) {
							mesh.occlusionStrength = static_cast<float>(occExt.Get("strength").Get<double>());
						}
					}
				}

				// Retrieve material factors from the glTF material
				if (!mat.pbrMetallicRoughness.baseColorFactor.empty()) {
					baseColorFactor = glm::vec4(
						static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
						static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
						static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
						static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3])
					);
					if (baseColorFactor.a < 1.0f) {
						mesh.hasAlpha = true;
					}
				}
				// Use actual glTF material values, preserving defaults if not specified
				metallicFactor = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
				roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
				if (!mat.emissiveFactor.empty()) {
					emissiveFactor = glm::vec3(
						static_cast<float>(mat.emissiveFactor[0]),
						static_cast<float>(mat.emissiveFactor[1]),
						static_cast<float>(mat.emissiveFactor[2])
					);
				}
			}
			else {
				// No material specified: use default glTF PBR values
				baseColorFactor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // White diffuse
				metallicFactor = 0.0f;    // Non-metallic by default
				roughnessFactor = 1.0f;   // Fully rough by default
				emissiveFactor = glm::vec3(0.0f);  // No emission
			}

			// Create fallback textures for any missing material maps to ensure robust PBR shading
			if (mesh.diffuseTexture == nullptr) {
				// Create a 1x1 RGBA texture filled with baseColorFactor
				unsigned char color[4];
				color[0] = (unsigned char)glm::clamp(baseColorFactor.r * 255.0f, 0.0f, 255.0f);
				color[1] = (unsigned char)glm::clamp(baseColorFactor.g * 255.0f, 0.0f, 255.0f);
				color[2] = (unsigned char)glm::clamp(baseColorFactor.b * 255.0f, 0.0f, 255.0f);
				color[3] = (unsigned char)glm::clamp(baseColorFactor.a * 255.0f, 0.0f, 255.0f);

				mesh.diffuseTexture = Texture::Builder::Texture2D(1, 1, GL_RGBA8)
					.Format(GL_RGBA)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(color)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}
			if (mesh.normalTexture == nullptr) {
				// Default normal map: flat normal (0.5, 0.5, 1.0) in tangent space
				unsigned char normalPixel[3] = { 128, 128, 255 };

				mesh.normalTexture = Texture::Builder::Texture2D(1, 1, GL_RGB8)
					.Format(GL_RGB)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(normalPixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}
			if (mesh.roughnessTexture == nullptr) {
				// glTF metallic-roughness format: R=unused, G=roughness, B=metallic, A=unused
				unsigned char mrPixel[4];
				mrPixel[0] = 255;  // R unused
				mrPixel[1] = (unsigned char)glm::clamp(roughnessFactor * 255.0f, 0.0f, 255.0f);  // G = roughness
				mrPixel[2] = (unsigned char)glm::clamp(metallicFactor * 255.0f, 0.0f, 255.0f);   // B = metallic
				mrPixel[3] = 255;  // A unused

				mesh.roughnessTexture = Texture::Builder::Texture2D(1, 1, GL_RGBA8)
					.Format(GL_RGBA)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(mrPixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}
			if (mesh.emissiveTexture == nullptr) {
				// Create white emissive texture - when multiplied with emissiveFactor, gives correct result
				unsigned char emissivePixel[3] = { 255, 255, 255 }; // White texture - emissiveFactor * white = emissiveFactor

				mesh.emissiveTexture = Texture::Builder::Texture2D(1, 1, GL_RGB8)
					.Format(GL_RGB)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(emissivePixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}
			if (mesh.occlusionTexture == nullptr) {
				// White occlusion = no occlusion
				unsigned char whitePixel = 255;

				mesh.occlusionTexture = Texture::Builder::Texture2D(1, 1, GL_R8)
					.Format(GL_RED)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(&whitePixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}

			if (mesh.specularTexture == nullptr) {
				// Create default specular texture with specularFactor * specularColorFactor
				glm::vec3 defaultSpecular = mesh.specularFactor * mesh.specularColorFactor;
				unsigned char specPixel[3];
				specPixel[0] = (unsigned char)glm::clamp(defaultSpecular.r * 255.0f, 0.0f, 255.0f);
				specPixel[1] = (unsigned char)glm::clamp(defaultSpecular.g * 255.0f, 0.0f, 255.0f);
				specPixel[2] = (unsigned char)glm::clamp(defaultSpecular.b * 255.0f, 0.0f, 255.0f);

				mesh.specularTexture = Texture::Builder::Texture2D(1, 1, GL_RGB8)
					.Format(GL_RGB)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(specPixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}

			// Create default specular color texture if not loaded
			if (mesh.specularColorTexture == nullptr) {
				unsigned char specColorPixel[3];
				specColorPixel[0] = (unsigned char)glm::clamp(mesh.specularColorFactor.r * 255.0f, 0.0f, 255.0f);
				specColorPixel[1] = (unsigned char)glm::clamp(mesh.specularColorFactor.g * 255.0f, 0.0f, 255.0f);
				specColorPixel[2] = (unsigned char)glm::clamp(mesh.specularColorFactor.b * 255.0f, 0.0f, 255.0f);

				mesh.specularColorTexture = Texture::Builder::Texture2D(1, 1, GL_RGB8)
					.Format(GL_RGB)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(specColorPixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}

			// Create default transmission texture if not loaded
			if (mesh.transmissionTexture == nullptr) {
				unsigned char transmissionPixel = (unsigned char)glm::clamp(mesh.transmissionFactor * 255.0f, 0.0f, 255.0f);

				mesh.transmissionTexture = Texture::Builder::Texture2D(1, 1, GL_R8)
					.Format(GL_RED)
					.DataType(GL_UNSIGNED_BYTE)
					.Data(&transmissionPixel)
					.FilterMode(GL_LINEAR, GL_LINEAR)
					.WrapMode(GL_REPEAT, GL_REPEAT)
					.Build();
			}

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
			mesh.SetMaterialDesc(normalizedMaterial);

			std::cout << "[INFO] material factors: "
				<< "baseColorFactor: " << mesh.material.baseColorFactor.x << "," << mesh.material.baseColorFactor.y << "," << mesh.material.baseColorFactor.z << "," << mesh.material.baseColorFactor.w
				<< " metallicFactor: " << mesh.material.metallicFactor
				<< " roughnessFactor: " << mesh.material.roughnessFactor
				<< " emissiveFactor: " << mesh.material.emissiveFactor.x << "," << mesh.material.emissiveFactor.y << "," << mesh.material.emissiveFactor.z
				<< " emissiveStrength: " << mesh.material.emissiveStrength
				<< " specularFactor: " << mesh.material.specularFactor.x
				<< " clearcoatFactor: " << mesh.material.clearcoatFactor
				<< " clearcoatRoughnessFactor: " << mesh.material.clearcoatRoughnessFactor
				<< " occlusionStrength: " << mesh.material.occlusionStrength
				<< " transmissionFactor: " << mesh.material.transmissionFactor
				<< " thicknessFactor: " << mesh.material.thicknessFactor
				<< " attenuationDistance: " << mesh.material.attenuationDistance
				<< " ior: " << mesh.material.ior
				<< "\n";

			//Add the mesh to the Scene (use move since MeshComponent is move-only)
			meshes.emplace_back(std::move(mesh));
			}
		}
	}

	// (2) Load animations (if any) from the glTF
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
		const MaterialDesc& material = mesh.material;

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
			glUniform1f(locSpecularFactor, material.specularFactor.x);
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
	std::cout << "[INFO] Tangents computed for normal mapping.\n";
}
std::pair<glm::vec3, glm::vec3> Scene::GetBoundingBox() const {
	if (meshes.empty()) {
		return { glm::vec3(0), glm::vec3(0) };
	}
	glm::vec3 minB(std::numeric_limits<float>::max());
	glm::vec3 maxB(-std::numeric_limits<float>::max());
	bool valid = false;

	for (auto& mc : meshes) {
		GLuint vboId = mc.GetVBO();
		if (!vboId) continue;
		glBindBuffer(GL_ARRAY_BUFFER, vboId);
		GLint bufferSize = 0;
		glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bufferSize);
		if (bufferSize <= 0) {
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			continue;
		}
		void* dataPtr = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
		if (!dataPtr) {
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			continue;
		}
		const Vertex* v = (const Vertex*)dataPtr;
		size_t count = bufferSize / sizeof(Vertex);
		for (size_t i = 0; i < count; i++) {
			if (std::isfinite(v[i].position.x) &&
				std::isfinite(v[i].position.y) &&
				std::isfinite(v[i].position.z)) {
				minB = glm::min(minB, v[i].position);
				maxB = glm::max(maxB, v[i].position);
				valid = true;
			}
		}
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
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
