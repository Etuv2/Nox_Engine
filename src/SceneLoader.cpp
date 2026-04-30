#include "SceneLoader.h"
#include "SceneNode.h"
#include "AudioNode.h"
#include "GuiNode.h"
#include "LightNode.h"
#include "DirectionalLight.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "ModelManager.h"
#include "ShaderLoader.h"
#include "Scene.h"
#include "HierarchySystem.h"
#include "AnimationSystem.h"
#include "RenderSystem.h"
#include "Camera.h"
#include <fstream>
#include <iostream>
#include <ctime>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <cctype>
#include <exception>
#include <filesystem>
#include <sstream>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using json = nlohmann::json;

namespace {
constexpr bool kVerboseEntityCreationLogs = false;
constexpr float kImportedTransformTolerance = 1e-3f;
constexpr float kMinimumSceneScale = 1e-5f;

namespace fs = std::filesystem;

std::string TrimPathValue(const std::string& value)
{
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	std::string trimmed = value.substr(begin, end - begin);
	if (trimmed.size() >= 2) {
		const char first = trimmed.front();
		const char last = trimmed.back();
		if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
			trimmed = trimmed.substr(1, trimmed.size() - 2);
		}
	}
	return trimmed;
}

bool ReadableFileExists(const fs::path& path)
{
	std::error_code ec;
	return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

std::string NormalizePathForLog(const fs::path& path)
{
	std::error_code ec;
	fs::path absolutePath = fs::absolute(path, ec);
	if (!ec) {
		return absolutePath.lexically_normal().string();
	}
	return path.lexically_normal().string();
}

void PushUniquePath(std::vector<fs::path>& paths, const fs::path& path)
{
	const fs::path normalized = path.lexically_normal();
	for (const fs::path& existing : paths) {
		if (existing.lexically_normal() == normalized) {
			return;
		}
	}
	paths.push_back(normalized);
}

std::vector<fs::path> BuildRelativeAssetCandidates(const std::string& rawPath, const std::string& sceneDirectory)
{
	std::vector<fs::path> candidates;
	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		return candidates;
	}

	const fs::path authoredPath(trimmed);
	PushUniquePath(candidates, authoredPath);
	if (!authoredPath.is_absolute() && !sceneDirectory.empty()) {
		const fs::path sceneDir(sceneDirectory);
		PushUniquePath(candidates, sceneDir / authoredPath);
		PushUniquePath(candidates, sceneDir / ".." / authoredPath);
	}
	return candidates;
}

std::string ResolveReadableAssetPath(
	const std::string& rawPath,
	const std::string& sceneDirectory,
	const char* assetKind,
	bool required,
	bool* resolved)
{
	if (resolved) {
		*resolved = false;
	}

	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		if (required) {
			std::cerr << "[SceneLoader] Missing required " << assetKind << " path." << std::endl;
		}
		return {};
	}

	const auto candidates = BuildRelativeAssetCandidates(trimmed, sceneDirectory);
	for (const fs::path& candidate : candidates) {
		if (ReadableFileExists(candidate)) {
			if (resolved) {
				*resolved = true;
			}
			return candidate.lexically_normal().string();
		}
	}

	std::ostringstream message;
	message << "[SceneLoader] " << (required ? "ERROR" : "Warning")
		<< ": could not resolve " << assetKind << " path '" << trimmed << "'. Tried:";
	for (const fs::path& candidate : candidates) {
		message << "\n  - " << NormalizePathForLog(candidate);
	}

	if (required) {
		std::cerr << message.str() << std::endl;
	}
	else {
		std::cout << message.str() << std::endl;
	}
	return trimmed;
}

std::string ResolveSceneJsonPath(const std::string& rawPath)
{
	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		return {};
	}

	std::vector<fs::path> candidates;
	const fs::path authoredPath(trimmed);
	PushUniquePath(candidates, authoredPath);
	if (!authoredPath.has_extension()) {
		PushUniquePath(candidates, fs::path(trimmed + ".json"));
	}
	if (!authoredPath.is_absolute()) {
		PushUniquePath(candidates, fs::path("scenes") / authoredPath);
		if (!authoredPath.has_extension()) {
			PushUniquePath(candidates, fs::path("scenes") / fs::path(trimmed + ".json"));
		}
	}

	for (const fs::path& candidate : candidates) {
		if (ReadableFileExists(candidate)) {
			return candidate.lexically_normal().string();
		}
	}

	return trimmed;
}

bool IsFiniteMatrix(const glm::mat4& m)
{
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (!std::isfinite(m[c][r])) {
				return false;
			}
		}
	}
	return true;
}

float MaxMatrixAbsDelta(const glm::mat4& a, const glm::mat4& b)
{
	float delta = 0.0f;
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			delta = std::max(delta, std::abs(a[c][r] - b[c][r]));
		}
	}
	return delta;
}

bool IsFiniteVec3(const glm::vec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool TryReadVec3Array(const json& parent, const char* key, glm::vec3& value)
{
	if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() != 3) {
		return false;
	}

	const auto& array = parent[key];
	for (size_t i = 0; i < 3; ++i) {
		if (!array[i].is_number()) {
			return false;
		}
	}

	value = glm::vec3(
		array[0].get<float>(),
		array[1].get<float>(),
		array[2].get<float>());
	return IsFiniteVec3(value);
}

glm::vec3 ReadSceneVec3(const json& parent, const char* key, const glm::vec3& fallback, const std::string& nodeName)
{
	glm::vec3 value = fallback;
	if (!parent.contains(key)) {
		return value;
	}

	if (!TryReadVec3Array(parent, key, value)) {
		std::cerr << "[SceneLoader] Warning: node '" << nodeName
			<< "' has invalid " << key << "; using fallback ("
			<< fallback.x << ", " << fallback.y << ", " << fallback.z << ")." << std::endl;
		return fallback;
	}

	return value;
}

glm::vec3 SanitizeSceneScale(const glm::vec3& scale, const std::string& nodeName)
{
	glm::vec3 sanitized = scale;
	bool changed = false;
	for (int i = 0; i < 3; ++i) {
		if (!std::isfinite(sanitized[i]) || std::abs(sanitized[i]) < kMinimumSceneScale) {
			sanitized[i] = 1.0f;
			changed = true;
		}
	}
	if (changed) {
		std::cerr << "[SceneLoader] Warning: node '" << nodeName
			<< "' had invalid or near-zero scale; sanitized to ("
			<< sanitized.x << ", " << sanitized.y << ", " << sanitized.z << ")." << std::endl;
	}
	return sanitized;
}

std::vector<glm::mat4> ComputeModelNodeWorldTransforms(const Scene& model)
{
	std::vector<glm::mat4> world(model.nodes.size(), glm::mat4(1.0f));
	std::vector<uint8_t> state(model.nodes.size(), 0u);

	std::function<glm::mat4(int)> computeWorld = [&](int nodeIndex) -> glm::mat4 {
		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
			return glm::mat4(1.0f);
		}

		if (state[nodeIndex] == 2u) {
			return world[nodeIndex];
		}

		if (state[nodeIndex] == 1u) {
			// Defensive cycle break for malformed content.
			return model.nodes[nodeIndex].localTransform;
		}

		state[nodeIndex] = 1u;
		const Scene::NodeInfo& nodeInfo = model.nodes[nodeIndex];
		glm::mat4 local = IsFiniteMatrix(nodeInfo.localTransform)
			? nodeInfo.localTransform
			: glm::mat4(1.0f);
		glm::mat4 nodeWorld = local;
		if (nodeInfo.parent >= 0) {
			nodeWorld = computeWorld(nodeInfo.parent) * local;
		}
		world[nodeIndex] = nodeWorld;
		state[nodeIndex] = 2u;
		return nodeWorld;
	};

	for (size_t i = 0; i < model.nodes.size(); ++i) {
		computeWorld(static_cast<int>(i));
	}

	return world;
}

float ComputeRenderableBoundingRadius(const Scene& model, const std::vector<uint32_t>& meshIndices, int referenceNodeIndex)
{
	float radius = 1.0f;
	bool foundMesh = false;
	for (uint32_t meshIndex : meshIndices) {
		if (meshIndex >= model.meshes.size()) {
			continue;
		}

		const MeshComponent& mesh = model.meshes[meshIndex];
		float candidate = mesh.boundingRadius;
		if (mesh.boundingVolumeValid) {
			candidate = glm::length(mesh.boundingCenter) + mesh.boundingRadius;
		}

		radius = foundMesh ? std::max(radius, candidate) : candidate;
		foundMesh = true;
	}

	return foundMesh ? radius : 1.0f;
}
}

SceneLoader::SceneLoader(std::shared_ptr<ModelManager> modelManager, std::shared_ptr<PhysicsEngine> physicsEngine, int screenW, int screenH) :
	m_modelManager(modelManager),
	m_physicsEngine(physicsEngine),
	m_screenH(screenH),
	m_screenW(screenW)
{
}

SceneLoader::~SceneLoader() {
}

std::shared_ptr<SceneGraph> SceneLoader::LoadScene(const std::string& sceneFilePath) {
	const std::string resolvedScenePath = ResolveSceneJsonPath(sceneFilePath);
	if (resolvedScenePath.empty()) {
		std::cerr << "[SceneLoader] Failed to load scene: empty scene path." << std::endl;
		GuiEventBus::GetInstance().PublishError("Failed to load scene: empty scene path");
		return nullptr;
	}

	auto sceneGraph = std::make_shared<SceneGraph>();

	// Set the current scene graph BEFORE processing nodes
	// This is required for CreateECSEntity to work properly
	m_currentSceneGraph = sceneGraph.get();
	m_currentSceneDirectory = fs::path(resolvedScenePath).parent_path().string();
	m_loadedModelCount = 0;
	m_missingCriticalAssetCount = 0;
	m_loadedSkyboxCount = 0;
	m_disabledSkyboxCount = 0;

	// Report loading started
	GuiEventBus::GetInstance().PublishLoadingStarted();
	GuiEventBus::GetInstance().PublishProgress(0.0f, "Assets");

	std::cout << "[SceneLoader] Loading scene: " << resolvedScenePath << std::endl;
	std::ifstream sceneFile(resolvedScenePath);
	if (!sceneFile.is_open()) {
		std::cerr << "[SceneLoader] Failed to open scene file: " << resolvedScenePath << std::endl;
		GuiEventBus::GetInstance().PublishError("Failed to open scene file: " + resolvedScenePath);
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();
		return nullptr;
	}

	json sceneJson;
	try {
		sceneFile >> sceneJson;
	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Failed to parse scene file '" << resolvedScenePath
			<< "': " << e.what() << std::endl;
		GuiEventBus::GetInstance().PublishError("Failed to parse scene file: " + resolvedScenePath);
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();
		return nullptr;
	}

	if (!sceneJson.is_object()) {
		std::cerr << "[SceneLoader] Invalid scene file: root JSON value must be an object: "
			<< resolvedScenePath << std::endl;
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();
		return nullptr;
	}

	// Set the scene name if provided.
	std::string sceneName = sceneJson.value("scene_name", "Unnamed Scene");
	sceneGraph->SetSceneName(sceneName);

	// Set the scenes exposure and gamma values if provided.
	sceneGraph->m_exposure = sceneJson.value("exposure", 1.0f);
	sceneGraph->m_gamma = sceneJson.value("gamma", 2.2f);

	// Look for a hierarchical "nodes" array.
	if (sceneJson.contains("nodes") && sceneJson["nodes"].is_array()) {
		// Get root entity ID for hierarchy
		EntityID rootEntityID = INVALID_ENTITY;
		auto root = sceneGraph->GetRoot();
		if (root) {
			rootEntityID = root->GetEntityID();
		}

		for (auto& nodeEntry : sceneJson["nodes"]) {
			auto node = ProcessNodeRecursive(nodeEntry, rootEntityID);
			if (node) {
				sceneGraph->GetRoot()->AddChild(node);
			}
		}

		std::cout << "[SceneLoader] Loaded " << sceneJson["nodes"].size() << " root nodes with ECS entities" << std::endl;
	}
	else {
		std::cerr << "[SceneLoader] Invalid scene file format. 'nodes' array is missing: "
			<< resolvedScenePath << std::endl;
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();
		return nullptr;
	}

	if (m_missingCriticalAssetCount > 0) {
		std::cerr << "[SceneLoader] Aborting scene load for '" << resolvedScenePath
			<< "' due to " << m_missingCriticalAssetCount
			<< " missing required asset(s)." << std::endl;
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();
		return nullptr;
	}

	// Initialize the skybox if provided.
	if (sceneJson.contains("skybox")) {
		std::string hdrPath = TrimPathValue(sceneJson.value("skybox", ""));
		if (hdrPath.empty()) {
			++m_disabledSkyboxCount;
			std::cout << "[SceneLoader] Skybox disabled for scene '" << sceneName
				<< "' because the skybox path is empty." << std::endl;
		}
		else {
			bool skyboxResolved = false;
			const std::string resolvedHdrPath = ResolveReadableAssetPath(
				hdrPath,
				m_currentSceneDirectory,
				"skybox",
				false,
				&skyboxResolved);

			if (!skyboxResolved) {
				++m_disabledSkyboxCount;
				std::cout << "[SceneLoader] Skybox disabled for scene '" << sceneName
					<< "' because the HDR asset could not be resolved: " << hdrPath << std::endl;
			}
			else {
				auto sb = std::make_shared<Skybox>();
				if (!sb->Init(resolvedHdrPath,
					"shaders/equirect2cube_vert.glsl",
					"shaders/equirect2cube_frag.glsl",
					"shaders/skybox_vert.glsl",
					"shaders/skybox_frag.glsl",
					m_screenW,
					m_screenH)) {
					++m_disabledSkyboxCount;
					std::cerr << "[SceneLoader] Failed to load skybox: " << resolvedHdrPath << "\n";
				}
				else {
					++m_loadedSkyboxCount;
					std::cout << "[SceneLoader] Loading skybox: " << resolvedHdrPath << std::endl;
					sceneGraph->SetSkybox(sb);
				}
			}
		}
	}

	// Per-scene physics flag (default true)
	bool physicsEnabled = sceneJson.value("physics_enabled", true);
	sceneGraph->SetPhysicsEnabled(physicsEnabled);

	// Publish authored scene transforms before the scene is handed back to callers.
	sceneGraph->UpdateAllTransforms();
	// Clear the scene graph reference
	m_currentSceneGraph = nullptr;
	m_currentSceneDirectory.clear();

	// Log ECS statistics
	auto* componentManager = sceneGraph->GetComponentManager();
	std::cout << "[SceneLoader] Scene load summary: scene='" << sceneName
		<< "', path='" << resolvedScenePath
		<< "', authoredRootNodes=" << sceneJson["nodes"].size()
		<< ", models=" << m_loadedModelCount
		<< ", transforms=" << componentManager->GetTransformPool().Size()
		<< ", maxTransformID=" << componentManager->GetMaxAllocatedTransformID()
		<< ", renderables=" << componentManager->GetRenderablePool().Size()
		<< ", lights=" << componentManager->GetLightPool().Size()
		<< ", cameras=" << componentManager->GetCameraPool().Size()
		<< ", skyboxesLoaded=" << m_loadedSkyboxCount
		<< ", skyboxesDisabled=" << m_disabledSkyboxCount
		<< std::endl;

	return sceneGraph;
}

// Save scene state back to JSON file
bool SceneLoader::SaveScene(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sceneFilePath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot save: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Saving scene to: " << sceneFilePath << std::endl;

	try {
		nlohmann::json sceneJson;

		// Serialize scene metadata
		sceneJson["scene_name"] = sceneGraph->GetSceneName();
		sceneJson["exposure"] = sceneGraph->m_exposure;
		sceneJson["gamma"] = sceneGraph->m_gamma;
		sceneJson["physics_enabled"] = sceneGraph->IsPhysicsEnabled();

		// Serialize skybox if present
		if (sceneGraph->GetSkybox()) {
			// Note: We'll need to store the HDR path somewhere or use a default
			// For now, use a placeholder
			sceneJson["skybox"] = "hdrs/skybox.hdr"; // TODO: Store actual path in Skybox class
		}

		// Serialize all root-level nodes and their children
		nlohmann::json nodesArray = nlohmann::json::array();
		auto root = sceneGraph->GetRoot();

		for (const auto& child : root->children) {
			if (child) {
				nlohmann::json nodeJson = SerializeNodeRecursive(child);
				if (!nodeJson.is_null()) {
					nodesArray.push_back(nodeJson);
				}
			}
		}

		sceneJson["nodes"] = nodesArray;

		// Write to file with pretty formatting
		std::ofstream outFile(sceneFilePath);
		if (!outFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open file for writing: " << sceneFilePath << std::endl;
			return false;
		}

		outFile << sceneJson.dump(2); // Indent with 2 spaces for readability
		outFile.close();

		std::cout << "[SceneLoader] Successfully saved scene with " << nodesArray.size() << " nodes" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error saving scene: " << e.what() << std::endl;
		return false;
	}
}

nlohmann::json SceneLoader::SerializeNodeRecursive(const std::shared_ptr<SceneNode>& node) {
	if (!node) {
		return nlohmann::json();
	}

	// Serialize this node
	nlohmann::json nodeJson = SerializeNode(node);

	// Serialize children recursively
	if (!node->children.empty()) {
		nlohmann::json childrenArray = nlohmann::json::array();

		for (const auto& child : node->children) {
			if (child) {
				nlohmann::json childJson = SerializeNodeRecursive(child);
				if (!childJson.is_null()) {
					childrenArray.push_back(childJson);
				}
			}
		}

		if (!childrenArray.empty()) {
			nodeJson["children"] = childrenArray;
		}
	}

	return nodeJson;
}

nlohmann::json SceneLoader::SerializeNode(const std::shared_ptr<SceneNode>& node) {
	if (!node) {
		return nlohmann::json();
	}

	nlohmann::json nodeJson;

	// Determine and set node type
	SceneNode::NODE_TYPE nodeType = node->GetNodeType();
	switch (nodeType) {
	case SceneNode::MODEL:
		nodeJson["type"] = "model";
		break;
	case SceneNode::LIGHT:
		nodeJson["type"] = "light";
		break;
	case SceneNode::AUDIO:
		nodeJson["type"] = "audio";
		break;
	case SceneNode::CAMERA:
		nodeJson["type"] = "camera";
		break;
	case SceneNode::GUI:
		nodeJson["type"] = "gui";
		break;
	case SceneNode::LPV_VOLUME:
		nodeJson["type"] = "lpv_volume";
		break;
	case SceneNode::SKELETAL:
		nodeJson["type"] = "skeletal";
		break;
	default:
		nodeJson["type"] = "node";
		break;
	}

	// Serialize transform
	glm::vec3 position = node->GetPosition();
	glm::vec3 rotation = node->GetRotation();
	glm::vec3 scale = node->GetScale();

	nodeJson["position"] = { position.x, position.y, position.z };

	// Convert rotation from radians to degrees for saving
	nodeJson["rotation"] = { glm::degrees(rotation.x), glm::degrees(rotation.y), glm::degrees(rotation.z) };

	nodeJson["scale"] = { scale.x, scale.y, scale.z };

	// Serialize node name
	std::string nodeName = node->GetName();
	if (!nodeName.empty()) {
		nodeJson["name"] = nodeName;
	}

	// Serialize type-specific properties
	switch (nodeType) {
	case SceneNode::MODEL: {
		// Serialize model path using ModelManager
		auto model = node->GetModel();
		if (model && m_modelManager) {
			std::string modelPath = m_modelManager->GetModelPath(model.get());
			if (!modelPath.empty()) {
				nodeJson["path"] = modelPath;
			}
			else {
				std::cout << "[SceneLoader] Warning: Could not find model path for node '"
					<< nodeName << "'" << std::endl;
			}
		}
		break;
	}

	case SceneNode::LIGHT: {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			nodeJson["light"] = SerializeLightProperties(lightNode);
		}
		break;
	}

	case SceneNode::AUDIO: {
		auto audioNode = std::dynamic_pointer_cast<AudioNode>(node);
		if (audioNode) {
			// Serialize audio properties
			nodeJson["pitch"] = audioNode->getPitch();
			nodeJson["volume"] = audioNode->getVolume();
			nodeJson["hearing_distance"] = audioNode->getHearingDistance();
			nodeJson["loop"] = audioNode->isPlaying(); // Note: This is approximate, would need actual loop state
			nodeJson["is3d"] = audioNode->is3D();

			// TODO: Still need sound file path - AudioNode needs to expose this
			std::cout << "[SceneLoader] Warning: Audio sound file path not saved (AudioNode doesn't expose it)" << std::endl;
		}
		break;
	}

	case SceneNode::GUI: {
		auto guiNode = std::dynamic_pointer_cast<GuiNode>(node);
		if (guiNode) {
			nodeJson["elements"] = SerializeGuiElements(guiNode);
			// TODO: Add font path and size serialization if exposed in GuiNode
		}
		break;
	}

	case SceneNode::LPV_VOLUME: {
		// Serialize LPV volume data
		const auto& lpvData = node->GetLPVVolumeData();

		nlohmann::json lpvJson;
		lpvJson["center"] = { lpvData.center.x, lpvData.center.y, lpvData.center.z };
		lpvJson["extent"] = { lpvData.extent.x, lpvData.extent.y, lpvData.extent.z };
		lpvJson["voxel_size"] = lpvData.voxelSize;
		lpvJson["grid_resolution"] = lpvData.gridResolution;

		// Serialize quaternion orientation
		lpvJson["orientation"] = {
			lpvData.orientation.w,
			lpvData.orientation.x,
			lpvData.orientation.y,
			lpvData.orientation.z
		};

		nodeJson["lpv_data"] = lpvJson;
		break;
	}

	default:
		break;
	}

	// Serialize collider if present (for any node type, not just MODEL)
	// This allows nodes without models to still have physics colliders
	if (node->GetRigidBody()) {
		nodeJson["collider"] = SerializeCollider(node);
	}

	return nodeJson;
}

nlohmann::json SceneLoader::SerializeCollider(const std::shared_ptr<SceneNode>& node) {
	nlohmann::json colliderJson;

	auto rb = node->GetRigidBody();
	if (!rb) {
		return colliderJson;
	}

	// Determine collider type
	RigidBody::ShapeType shapeType = rb->getShapeType();

	switch (shapeType) {
	case RigidBody::ShapeType::SPHERE: {
		colliderJson["type"] = "sphere";
		colliderJson["mass"] = rb->getMass();
		colliderJson["radius"] = rb->getBoundingRadius();
		colliderJson["linear_damping"] = rb->getLinearDamping();
		colliderJson["angular_damping"] = rb->getAngularDamping();
		colliderJson["friction"] = rb->getFriction();

		// Check if static body
		bool isStatic = rb->IsStatic();
		colliderJson["static"] = isStatic;

		if (!isStatic) {
			glm::vec3 velocity = rb->getVelocity();
			colliderJson["velocity_x"] = velocity.x;
			colliderJson["velocity_y"] = velocity.y;
			colliderJson["velocity_z"] = velocity.z;

			// Use gravity scale to determine if gravity is enabled
			bool hasGravity = (rb->getGravityScale() > 0.0f);
			colliderJson["gravity"] = hasGravity;
		}
		else {
			colliderJson["gravity"] = false;
		}
		break;
	}

	case RigidBody::ShapeType::BOX: {
		colliderJson["type"] = "box";
		colliderJson["mass"] = rb->getMass();

		glm::vec3 halfExtents = rb->getHalfExtents();
		colliderJson["size"] = { halfExtents.x, halfExtents.y, halfExtents.z };

		colliderJson["linear_damping"] = rb->getLinearDamping();
		colliderJson["angular_damping"] = rb->getAngularDamping();
		colliderJson["friction"] = rb->getFriction();

		// Check if static body
		bool isStatic = rb->IsStatic();
		colliderJson["static"] = isStatic;

		if (!isStatic) {
			glm::vec3 velocity = rb->getVelocity();
			colliderJson["velocity_x"] = velocity.x;
			colliderJson["velocity_y"] = velocity.y;
			colliderJson["velocity_z"] = velocity.z;

			// Use gravity scale to determine if gravity is enabled
			bool hasGravity = (rb->getGravityScale() > 0.0f);
			colliderJson["gravity"] = hasGravity;
		}
		else {
			colliderJson["gravity"] = false;
		}
		break;
	}

	case RigidBody::ShapeType::PLANE: {
		colliderJson["type"] = "plane";

		glm::vec3 normal = rb->getPlaneNormal();
		colliderJson["normal_x"] = normal.x;
		colliderJson["normal_y"] = normal.y;
		colliderJson["normal_z"] = normal.z;

		colliderJson["height"] = rb->getPlaneHeight();
		break;
	}

	default:
		std::cerr << "[SceneLoader] Unknown collider shape type" << std::endl;
		break;
	}

	return colliderJson;
}

nlohmann::json SceneLoader::SerializeGuiElements(const std::shared_ptr<GuiNode>& guiNode) {
	nlohmann::json elementsArray = nlohmann::json::array();

	if (!guiNode) {
		return elementsArray;
	}

	// TODO: GuiNode needs to expose its elements for serialization
	// This would require adding a GetElements() method or similar
	// For now, return empty array

	std::cout << "[SceneLoader] Warning: GUI element serialization requires GuiNode::GetElements() implementation" << std::endl;

	return elementsArray;
}

nlohmann::json SceneLoader::SerializeLightProperties(const std::shared_ptr<LightNode>& lightNode) {
	nlohmann::json lightJson;

	if (!lightNode || !lightNode->GetLight()) {
		return lightJson;
	}

	auto light = lightNode->GetLight();

	// Determine light type
	BaseLight::LightType lightType = light->GetLightType();
	switch (lightType) {
	case BaseLight::LightType::DIRECTIONAL:
		lightJson["type"] = "directional";
		break;
	case BaseLight::LightType::POINT:
		lightJson["type"] = "point";
		break;
	case BaseLight::LightType::SPOT:
		lightJson["type"] = "spot";
		break;
	default:
		lightJson["type"] = "unknown";
		break;
	}

	// Serialize common light properties
	glm::vec3 color = light->GetColor();
	lightJson["color"] = { color.r, color.g, color.b };

	lightJson["intensity"] = light->GetIntensity();
	lightJson["enabled"] = light->IsEnabled();
	lightJson["castsShadows"] = light->CastsShadows();

	// Serialize type-specific properties
	if (lightType == BaseLight::LightType::DIRECTIONAL) {
		auto dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
		if (dirLight) {
			// TODO: Add directional light specific properties
			// shadowSize, splitLambda, etc.
			lightJson["shadowSize"] = 2048; // Default, would need getter
			lightJson["splitLambda"] = 0.85f; // Default, would need getter
		}
	}
	else if (lightType == BaseLight::LightType::POINT || lightType == BaseLight::LightType::SPOT) {
		lightJson["range"] = light->GetRange();

		glm::vec3 attenuation = light->GetAttenuation();
		lightJson["attenuation"] = { attenuation.x, attenuation.y, attenuation.z };

		if (lightType == BaseLight::LightType::SPOT) {
			auto spotLight = std::dynamic_pointer_cast<SpotLight>(light);
			if (spotLight) {
				lightJson["cutOff"] = spotLight->GetCutOff();
				lightJson["outerCutOff"] = spotLight->GetOuterCutOff();
				lightJson["shadowResolution"] = 1024; // Default, would need getter
			}
		}
		else {
			lightJson["shadowResolution"] = 1024; // Default for point lights
		}
	}

	return lightJson;
}

void SceneLoader::ProcessCollider(const nlohmann::json& colliderJson, std::shared_ptr<SceneNode> node) {
	if (!colliderJson.is_object() || !node) return;

	std::string type = colliderJson.value("type", "sphere");
	if (type == "sphere") {
		float mass = colliderJson.value("mass", 1.0f);
		float radius = colliderJson.value("radius", 1.0f);
		bool useGravity = colliderJson.value("gravity", true);
		float linearDamping = colliderJson.value("linear_damping", 0.1f);
		float angularDamping = colliderJson.value("angular_damping", 0.1f);
		float friction = colliderJson.value("friction", 0.5f);
		bool isStatic = colliderJson.value("static", false);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::SPHERE);
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setLinearDamping(linearDamping);
		rb->setAngularDamping(angularDamping);
		rb->setFriction(friction);
		rb->setBoundingRadius(radius);

		// Determine if this should be static
		// Static if explicitly marked OR if gravity is disabled (treat as immovable obstacle)
		if (isStatic || !useGravity) {
			rb->setMass(0.0f);  // Zero mass = infinite mass = immovable
			rb->SetBodyType(RigidBody::BodyType::STATIC);
			rb->setGravityScale(0.0f);
			rb->setVelocity(glm::vec3(0.0f));
			rb->setAcceleration(glm::vec3(0.0f));
		}
		else {
			rb->setMass(mass);
			rb->setGravityScale(1.0f);

			glm::vec3 initVel{
				colliderJson.value("velocity_x", 0.0f),
				colliderJson.value("velocity_y", 0.0f),
				colliderJson.value("velocity_z", 0.0f)
			};
			rb->setVelocity(initVel);
			rb->setAcceleration(glm::vec3(0.0f));
		}

		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "plane") {
		// Plane is static, infinite, and only needs a normal + height
		glm::vec3 normal = {
			colliderJson.value("normal_x", 0.0f),
			colliderJson.value("normal_y", 1.0f),
			colliderJson.value("normal_z", 0.0f)
		};
		float height = colliderJson.value("height", 0.0f);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::PLANE);
		rb->AttachNode(node);

		// Treat plane as static: zero velocity, zero mass
		rb->setMass(0.0f);
		rb->setVelocity(glm::vec3(0.0f));
		rb->setAcceleration(glm::vec3(0.0f));
		rb->setPosition(node->GetPosition());
		rb->setPlane(normal, height);

		// Planes are always static - no gravity
		rb->setGravityScale(0.0f);
		rb->SetBodyType(RigidBody::BodyType::STATIC);

		rb->setBoundingRadius(0.0f); // not used for collision
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "box") {
		glm::vec3 size = glm::vec3(1.0f);
		if (colliderJson.contains("size") && colliderJson["size"].is_array() && colliderJson["size"].size() == 3) {
			size = glm::vec3(colliderJson["size"][0], colliderJson["size"][1], colliderJson["size"][2]);
		}

		float mass = colliderJson.value("mass", 1.0f);
		bool useGravity = colliderJson.value("gravity", true);
		float linearDamping = colliderJson.value("linear_damping", 0.0f);
		float angularDamping = colliderJson.value("angular_damping", 0.0f);
		float friction = colliderJson.value("friction", 0.5f);
		bool isStatic = colliderJson.value("static", false);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::BOX);
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setLinearDamping(linearDamping);
		rb->setAngularDamping(angularDamping);
		rb->setFriction(friction);
		rb->setBox(size);

		// Determine if this should be static
		// Static if explicitly marked OR if gravity is disabled (treat as immovable obstacle)
		if (isStatic || !useGravity) {
			rb->setMass(0.0f);  // Zero mass = infinite mass = immovable
			rb->SetBodyType(RigidBody::BodyType::STATIC);
			rb->setGravityScale(0.0f);
			rb->setVelocity(glm::vec3(0.0f));
			rb->setAcceleration(glm::vec3(0.0f));
		}
		else {
			rb->setMass(mass);
			rb->setGravityScale(1.0f);

			glm::vec3 initVel{
				colliderJson.value("velocity_x", 0.0f),
				colliderJson.value("velocity_y", 0.0f),
				colliderJson.value("velocity_z", 0.0f)
			};
			rb->setVelocity(initVel);
			rb->setAcceleration(glm::vec3(0.0f));
		}

		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
}



void SceneLoader::ProcessGuiElementProperties(std::shared_ptr<GuiNode> guiNode, int elementId, const nlohmann::json& elementJson) {
	if (!guiNode || elementId < 0) return;

	// Handle resizable properties
	if (elementJson.contains("resizable")) {
		bool resizable = elementJson["resizable"];
		float minWidth = elementJson.value("min_width", 10.0f);
		float minHeight = elementJson.value("min_height", 10.0f);
		float maxWidth = elementJson.value("max_width", -1.0f);
		float maxHeight = elementJson.value("max_height", -1.0f);

		guiNode->SetElementResizable(elementId, resizable, minWidth, minHeight, maxWidth, maxHeight);
	}

	// Handle anchoring
	if (elementJson.contains("anchor")) {
		std::string anchorStr = elementJson["anchor"];
		GuiNode::GuiElement::Anchor anchor = GuiNode::GuiElement::Anchor::TOP_LEFT;

		if (anchorStr == "top_left") {
			anchor = GuiNode::GuiElement::Anchor::TOP_LEFT;
		}
		else if (anchorStr == "top_right") {
			anchor = GuiNode::GuiElement::Anchor::TOP_RIGHT;
		}
		else if (anchorStr == "bottom_left") {
			anchor = GuiNode::GuiElement::Anchor::BOTTOM_LEFT;
		}
		else if (anchorStr == "bottom_right") {
			anchor = GuiNode::GuiElement::Anchor::BOTTOM_RIGHT;
		}
		else if (anchorStr == "center") {
			anchor = GuiNode::GuiElement::Anchor::CENTER;
		}

		guiNode->SetElementAnchor(elementId, anchor);
	}

	// Handle relative positioning
	if (elementJson.contains("relative_position") && elementJson["relative_position"].is_array() &&
		elementJson["relative_position"].size() >= 2) {
		float relX = elementJson["relative_position"][0];
		float relY = elementJson["relative_position"][1];
		guiNode->SetElementRelativePosition(elementId, relX, relY);
	}

	// Handle relative sizing
	if (elementJson.contains("relative_size") && elementJson["relative_size"].is_array() &&
		elementJson["relative_size"].size() >= 2) {
		float relWidth = elementJson["relative_size"][0];
		float relHeight = elementJson["relative_size"][1];
		guiNode->SetElementRelativeSize(elementId, relWidth, relHeight);
	}
}

std::shared_ptr<SceneNode> SceneLoader::ProcessNodeRecursive(const json& nodeJson, EntityID parentEntityID) {
	auto node = ProcessNode(nodeJson);
	if (!node) return nullptr;

	// Create ECS entity for this node
	CreateECSEntity(node, parentEntityID);

	EntityID myEntityID = node->GetEntityID();
	const size_t importedChildCount = node->children.size();
	for (size_t i = 0; i < importedChildCount; ++i) {
		CreateExistingChildEntitiesRecursive(node->children[i], myEntityID);
	}

	// If the node has children, process them recursively with this node as parent
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		for (auto& childJson : nodeJson["children"]) {
			auto childNode = ProcessNodeRecursive(childJson, myEntityID);
			if (childNode) {
				node->AddChild(childNode);
			}
		}
	}
	return node;
}

std::shared_ptr<SceneNode> SceneLoader::ProcessNode(const json& nodeJson) {
	ComponentManager* componentManager = m_currentSceneGraph ? m_currentSceneGraph->GetComponentManager() : nullptr;
	TransformSystem* transformSystem = m_currentSceneGraph ? m_currentSceneGraph->GetTransformSystem() : nullptr;

	// Determine node type.
	std::string typeStr = nodeJson.value("type", "model");
	NodeType type = GetNodeType(typeStr);

	// Create the appropriate node type.
	std::shared_ptr<SceneNode> node;
	// set node its type
	if (node == nullptr)
		switch (type) {
		case NodeType::AUDIO:
			node = std::make_shared<AudioNode>();
			node->SetECSContext(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		case NodeType::LIGHT:
			node = std::make_shared<LightNode>(nullptr); // Will be set based on light properties
			node->SetECSContext(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		case NodeType::CAMERA:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::CAMERA);
			break;
		case NodeType::LPV_VOLUME:  // Handle LPV volume nodes
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::LPV_VOLUME);
			std::cout << "[SceneLoader] Creating LPV Volume node" << std::endl;
			break;
		case NodeType::SKELETAL:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::SKELETAL);
			break;
		case NodeType::MODEL:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		default:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			break;
		}

	// Process common transform data.
	const std::string nodeDebugName = nodeJson.value("name", typeStr);
	glm::vec3 pos = ReadSceneVec3(nodeJson, "position", glm::vec3(0.0f), nodeDebugName);
	glm::vec3 rot = ReadSceneVec3(nodeJson, "rotation", glm::vec3(0.0f), nodeDebugName);
	glm::vec3 scl = SanitizeSceneScale(
		ReadSceneVec3(nodeJson, "scale", glm::vec3(1.0f), nodeDebugName),
		nodeDebugName);

	// Apply rotation correctly using combined Euler angles -> quaternion conversion
	// Previous code called SetRotation three times, but each call replaced the rotation instead of accumulating
	glm::vec3 radians = glm::radians(rot);
	glm::quat rotationQuat = glm::quat(radians); // glm::quat from Euler angles (pitch, yaw, roll)

	// Apply transform using SetLocalTRS which correctly combines translation, rotation, scale
	node->SetLocalTRS(pos, rotationQuat, scl);

	// Set node name if provided
	const std::string nodeName = nodeJson.value("name", "");
	if (!nodeName.empty()) {
		node->SetName(nodeName);
	}

	// Process type-specific properties.
	switch (type) {
	case NodeType::MODEL: { // ModelNode
		std::string modelPath = nodeJson.value("path", "");
		bool modelResolved = false;
		modelPath = ResolveReadableAssetPath(
			modelPath,
			m_currentSceneDirectory,
			"model",
			true,
			&modelResolved);
		if (modelResolved) {
			if (!m_modelManager) {
				++m_missingCriticalAssetCount;
				std::cerr << "[SceneLoader] ERROR: ModelManager unavailable while loading node '"
					<< nodeDebugName << "': " << modelPath << std::endl;
				break;
			}
			auto model = m_modelManager->LoadModel(modelPath); // Load the model using the ModelManager
			if (model) {
				++m_loadedModelCount;
				node->SetModel(model); // Set the model to the node
				if (model->hasSkin) {
					node->BuildSkeleton(*model);
				}
			}
			else {
				++m_missingCriticalAssetCount;
				std::cerr << "[SceneLoader] ERROR: Model import failed for node '"
					<< nodeDebugName << "': " << modelPath << std::endl;
			}
		}
		else {
			++m_missingCriticalAssetCount;
		}
		// Check for custom shader properties
		if (nodeJson.contains("vertex_shader") && nodeJson.contains("fragment_shader")) {
			std::string vsPath = nodeJson.value("vertex_shader", "");
			std::string fsPath = nodeJson.value("fragment_shader", "");
			if (!vsPath.empty() && !fsPath.empty()) {
				GLuint customShader = CreateShaderProgram(vsPath.c_str(), fsPath.c_str());
				if (customShader != 0)
					node->SetShader(customShader);
				else
					std::cerr << "[ERROR] Custom shader compilation failed for node ("
					<< node->GetName() << "). Falling back to default shader." << std::endl;
			}
		}
		if (auto model = node->GetModel(); model && !model->hasSkin) {
			BuildImportedMeshNodeChildren(node, model);
		}
		break;
	}
	case NodeType::AUDIO: { // AudioNode
		AudioNode* audioNode = dynamic_cast<AudioNode*>(node.get()); // Ensure the node is of type AudioNode
		if (audioNode) {
			std::string soundFile = nodeJson.value("sound", "");
			float pitch = nodeJson.value("pitch", 1.0f);
			float volume = nodeJson.value("volume", 1.0f);
			float hearingDistance = nodeJson.value("hearing_distance", 10.0f);
			bool loop = nodeJson.value("loop", false);
			bool is3d = nodeJson.value("is3d", true);
			audioNode->setPitch(pitch);
			audioNode->setVolume(volume);
			audioNode->setHearingDistance(hearingDistance);
			if (!soundFile.empty()) {
				if (!audioNode->initAudio(soundFile))
					std::cerr << "[ERROR] Failed to load sound file: " << soundFile << std::endl;
			}
			audioNode->play(loop, is3d);
		}
		break;
	}
	case NodeType::LIGHT: {
		LightNode* lightNode = dynamic_cast<LightNode*>(node.get());
		if (lightNode && nodeJson.contains("light")) {
			const auto& lightJson = nodeJson["light"];

			// Create appropriate light type
			std::string lightType = lightJson.value("type", "point");
			std::shared_ptr<BaseLight> light;

			if (lightType == "directional") {
				auto dirLight = std::make_shared<DirectionalLight>();
				if (lightJson.contains("shadowSize")) {
					dirLight->InitializeCascades(lightJson["shadowSize"], lightJson.value("splitLambda", 0.8f));
				}
				light = dirLight;
			}
			else if (lightType == "point") {
				auto pointLight = std::make_shared<PointLight>();
				if (lightJson.contains("shadowResolution")) {
					pointLight->InitializeShadowMap(lightJson["shadowResolution"]);
				}
				light = pointLight;
			}
			else if (lightType == "spot") {
				auto spotLight = std::make_shared<SpotLight>();
				if (lightJson.contains("shadowResolution")) {
					spotLight->InitializeShadowMap(lightJson["shadowResolution"]);
				}
				if (lightJson.contains("cutOff")) {
					spotLight->SetCutOff(lightJson["cutOff"]);
				}
				if (lightJson.contains("outerCutOff")) {
					spotLight->SetOuterCutOff(lightJson["outerCutOff"]);
				}
				light = spotLight;
			}

			if (light) {
				// Set common light properties
				if (lightJson.contains("color") && lightJson["color"].is_array() && lightJson["color"].size() == 3) {
					light->SetColor(glm::vec3(lightJson["color"][0], lightJson["color"][1], lightJson["color"][2]));
				}
				if (lightJson.contains("intensity")) {
					light->SetIntensity(lightJson["intensity"]);
				}
				if (lightJson.contains("enabled")) {
					light->SetEnabled(lightJson["enabled"]);
				}
				else {
					// Enable lights by default when loaded from scene files
					light->SetEnabled(true);
				}
				if (lightJson.contains("castsShadows")) {
					light->SetCastsShadows(lightJson["castsShadows"]);
				}
				if (lightJson.contains("range")) {
					light->SetRange(lightJson["range"]);
				}
				if (lightJson.contains("attenuation") && lightJson["attenuation"].is_array() && lightJson["attenuation"].size() == 3) {
					light->SetAttenuation(lightJson["attenuation"][0], lightJson["attenuation"][1], lightJson["attenuation"][2]);
				}

				// Set the light and force transform update
				lightNode->SetLight(light);

				// Update light properties from node transform
				lightNode->UpdateLightFromTransform();

				// Enable debug visualization if specified
				if (lightJson.contains("showDebugVisualization")) {
					lightNode->SetDebugVisualization(lightJson["showDebugVisualization"]);
				}

				// Store node name for light registration (don't call SetName if it doesn't exist)
				std::string lightName = nodeJson.value("name", "Light_" + lightType);
				// node->SetName(lightName); // Comment out if SetName doesn't exist

				std::cout << "[SceneLoader] Successfully loaded " << lightType << " light node '"
					<< lightName << "' at (" << pos.x << "," << pos.y << "," << pos.z
					<< ") with intensity " << light->GetIntensity()
					<< ", enabled: " << light->IsEnabled() << std::endl;
			}
			else {
				std::cerr << "[SceneLoader] Failed to create light of type: " << lightType << std::endl;
			}
		}
		else {
			std::cerr << "[SceneLoader] Light node missing light properties" << std::endl;
		}
		break;
	}
	case NodeType::GUI: {
		auto guiNode = std::make_shared<GuiNode>(m_screenW, m_screenH); // Create a GUI node,this acts as a container for GUI elements in the scene
		guiNode->SetECSContext(componentManager, transformSystem);

		// Load font if specified
		std::string fontPath = nodeJson.value("font_path", "fonts\\arial.ttf");
		int fontSize = nodeJson.value("font_size", 24);
		guiNode->LoadFont(fontPath, fontSize);

		if (nodeJson.contains("elements")) { // Check for GUI elements
			for (const auto& el : nodeJson["elements"]) { // Loop through each element
				std::string kind = el.value("kind", "text");

				if (kind == "text") {
					SDL_Color textColor = { 255, 255, 255, 255 }; // Default white
					if (el.contains("color") && el["color"].is_array() && el["color"].size() >= 3) {
						textColor.r = static_cast<Uint8>(el["color"][0]);
						textColor.g = static_cast<Uint8>(el["color"][1]);
						textColor.b = static_cast<Uint8>(el["color"][2]);
						textColor.a = el["color"].size() > 3 ? static_cast<Uint8>(el["color"][3]) : 255;
					}
					guiNode->AddText(el.value("text", ""), el.value("x", 0.0f), el.value("y", 0.0f), textColor);
				}
				else if (kind == "image") {
					guiNode->AddImage(el.value("path", ""), el.value("x", 0.0f), el.value("y", 0.0f),
						el.value("w", 128.0f), el.value("h", 64.0f));
				}
				else if (kind == "solid_rect") {
					SDL_Color rectColor = { 128, 128, 128, 255 }; // Default gray
					if (el.contains("color") && el["color"].is_array() && el["color"].size() >= 3) {
						rectColor.r = static_cast<Uint8>(el["color"][0]);
						rectColor.g = static_cast<Uint8>(el["color"][1]);
						rectColor.b = static_cast<Uint8>(el["color"][2]);
						rectColor.a = el["color"].size() > 3 ? static_cast<Uint8>(el["color"][3]) : 255;
					}

					int elementId = guiNode->AddSolidRect(
						el.value("x", 0.0f),
						el.value("y", 0.0f),
						el.value("w", 100.0f),
						el.value("h", 100.0f),
						rectColor
					);

					// Apply dynamic properties if specified
					ProcessGuiElementProperties(guiNode, elementId, el);
				}
				else if (kind == "gradient_rect") {
					GuiNode::GradientStyle gradient;

					// Set gradient type
					std::string gradientType = el.value("gradient_type", "vertical");
					if (gradientType == "horizontal") {
						gradient.type = GuiNode::GradientType::LINEAR_HORIZONTAL;
					}
					else if (gradientType == "vertical") {
						gradient.type = GuiNode::GradientType::LINEAR_VERTICAL;
					}
					else if (gradientType == "radial") {
						gradient.type = GuiNode::GradientType::RADIAL;
					}

					// Set start color
					gradient.startColor = { 255, 255, 255, 255 }; // Default white
					if (el.contains("start_color") && el["start_color"].is_array() && el["start_color"].size() >= 3) {
						gradient.startColor.r = static_cast<Uint8>(el["start_color"][0]);
						gradient.startColor.g = static_cast<Uint8>(el["start_color"][1]);
						gradient.startColor.b = static_cast<Uint8>(el["start_color"][2]);
						gradient.startColor.a = el["start_color"].size() > 3 ? static_cast<Uint8>(el["start_color"][3]) : 255;
					}

					// Set end color
					gradient.endColor = { 0, 0, 0, 255 }; // Default black
					if (el.contains("end_color") && el["end_color"].is_array() && el["end_color"].size() >= 3) {
						gradient.endColor.r = static_cast<Uint8>(el["end_color"][0]);
						gradient.endColor.g = static_cast<Uint8>(el["end_color"][1]);
						gradient.endColor.b = static_cast<Uint8>(el["end_color"][2]);
						gradient.endColor.a = el["end_color"].size() > 3 ? static_cast<Uint8>(el["end_color"][3]) : 255;
					}

					// Set radial gradient properties
					if (gradient.type == GuiNode::GradientType::RADIAL) {
						if (el.contains("center") && el["center"].is_array() && el["center"].size() >= 2) {
							gradient.center = glm::vec2(el["center"][0], el["center"][1]);
						}
						gradient.radius = el.value("radius", 1.0f);
					}

					int elementId = guiNode->AddGradientRect(
						el.value("x", 0.0f),
						el.value("y", 0.0f),
						el.value("w", 100.0f),
						el.value("h", 100.0f),
						gradient
					);

					// Apply dynamic properties if specified
					ProcessGuiElementProperties(guiNode, elementId, el);
				}
				else if (kind == "bevel_rect") {
					SDL_Color baseColor = { 128, 128, 128, 255 }; // Default gray
					if (el.contains("color") && el["color"].is_array() && el["color"].size() >= 3) {
						baseColor.r = static_cast<Uint8>(el["color"][0]);
						baseColor.g = static_cast<Uint8>(el["color"][1]);
						baseColor.b = static_cast<Uint8>(el["color"][2]);
						baseColor.a = el["color"].size() > 3 ? static_cast<Uint8>(el["color"][3]) : 255;
					}

					GuiNode::BevelStyle bevel;
					bevel.radius = el.value("corner_radius", 0.0f);
					bevel.bevelSize = el.value("bevel_size", 2.0f);

					// Set highlight color
					if (el.contains("highlight_color") && el["highlight_color"].is_array() && el["highlight_color"].size() >= 3) {
						bevel.highlightColor.r = static_cast<Uint8>(el["highlight_color"][0]);
						bevel.highlightColor.g = static_cast<Uint8>(el["highlight_color"][1]);
						bevel.highlightColor.b = static_cast<Uint8>(el["highlight_color"][2]);
						bevel.highlightColor.a = el["highlight_color"].size() > 3 ? static_cast<Uint8>(el["highlight_color"][3]) : 128;
					}

					// Set shadow color
					if (el.contains("shadow_color") && el["shadow_color"].is_array() && el["shadow_color"].size() >= 3) {
						bevel.shadowColor.r = static_cast<Uint8>(el["shadow_color"][0]);
						bevel.shadowColor.g = static_cast<Uint8>(el["shadow_color"][1]);
						bevel.shadowColor.b = static_cast<Uint8>(el["shadow_color"][2]);
						bevel.shadowColor.a = el["shadow_color"].size() > 3 ? static_cast<Uint8>(el["shadow_color"][3]) : 128;
					}

					int elementId = guiNode->AddBevelRect(
						el.value("x", 0.0f),
						el.value("y", 0.0f),
						el.value("w", 100.0f),
						el.value("h", 100.0f),
						baseColor,
						bevel
					);

					// Apply dynamic properties if specified
					ProcessGuiElementProperties(guiNode, elementId, el);
				}
				else {
					std::cerr << "[SceneLoader] Unknown GUI element kind: " << kind << std::endl;
				}
			}
		}
		node = guiNode;
		node->SetNodeType(SceneNode::GUI); // Set the node type to GUI
		node->SetLocalTRS(pos, rotationQuat, scl);
		if (!nodeName.empty()) {
			node->SetName(nodeName);
		}
		break;
	}

	default:
		std::cerr << "Unknown node type: " << typeStr << std::endl;
		break;
	}

	// Process collider for any node type (not just MODEL nodes)
	// This allows nodes without models to still have physics colliders
	if (nodeJson.contains("collider")) {
		ProcessCollider(nodeJson["collider"], node);
	}

	return node;
}

// ============== ECS ENTITY AND COMPONENT CREATION ==============

void SceneLoader::CreateECSEntity(std::shared_ptr<SceneNode> node, EntityID parentID) {
	if (!node || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	TransformSystem* transformSystem = m_currentSceneGraph->GetTransformSystem();
	if (!componentManager) return;

	const glm::mat4 authoredLocalTransform = node->GetTransform();
	const glm::mat4 authoredAnimatedTransform = node->GetAnimatedTransform();

	// Skip if entity already exists
	if (node->GetEntityID() != INVALID_ENTITY) {
		// Keep ECS adjacency authoritative whenever an existing node is reattached.
		componentManager->SetParent(node->GetEntityID(), parentID);
		if (TransformComponent* transform = node->GetTransformComponent()) {
			transform->isDirty = true;
		}
		return;
	}

	// Determine ECS node type
	::NodeType ecsType = ::NodeType::NODE;
	switch (node->GetNodeType()) {
	case SceneNode::NODE:       ecsType = ::NodeType::NODE; break;
	case SceneNode::MODEL:      ecsType = ::NodeType::MODEL; break;
	case SceneNode::LIGHT:      ecsType = ::NodeType::LIGHT; break;
	case SceneNode::CAMERA:     ecsType = ::NodeType::CAMERA; break;
	case SceneNode::AUDIO:      ecsType = ::NodeType::AUDIO; break;
	case SceneNode::GUI:        ecsType = ::NodeType::GUI; break;
	case SceneNode::LPV_VOLUME: ecsType = ::NodeType::LPV_VOLUME; break;
	case SceneNode::SKELETAL:   ecsType = ::NodeType::SKELETAL; break;
	}

	// Create entity
	std::string nodeName = node->GetName();
	EntityID entityID = componentManager->CreateEntity(nodeName, ecsType);
	node->SetEntityID(entityID);

	// CreateEntity already allocates a transform component. Populate it in place so the
	// loader authors the final local transform instead of layering a second AddTransform call.
	TransformComponent* transformComp = componentManager->GetTransform(entityID);
	if (!transformComp) {
		return;
	}
	transformComp->localTransform = authoredLocalTransform;
	transformComp->animatedTransform = authoredAnimatedTransform;
	transformComp->worldTransform = glm::mat4(1.0f);
	transformComp->prevWorldTransform = glm::mat4(1.0f);
	transformComp->hasAnimation = (authoredAnimatedTransform != glm::mat4(1.0f));
	transformComp->isDirty = true;
	transformComp->parentID = INVALID_ENTITY;

	componentManager->SetParent(entityID, parentID);
	componentManager->QueueTransformUpdate(entityID);
	if (transformSystem) {
		transformSystem->SetLocalTransform(entityID, authoredLocalTransform);
		if (authoredAnimatedTransform != glm::mat4(1.0f)) {
			transformSystem->SetAnimatedTransform(entityID, authoredAnimatedTransform);
		}
		else {
			transformSystem->ClearAnimatedTransform(entityID);
		}
	}
	// Create RenderableComponent if node has a model
	if (node->GetModel() && (node->renderWholeModel || !node->renderMeshIndices.empty())) {
		CreateRenderableComponent(node, node->GetModel());
	}

	if (node->GetNodeType() == SceneNode::LIGHT) {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			LightComponent lightComp;
			lightComp.light = lightNode->GetLight();
			lightComp.enabled = lightNode->GetLight()->IsEnabled();
			lightComp.castsShadows = lightNode->GetLight()->CastsShadows();
			componentManager->AddLight(entityID, lightComp);
		}
	}
	else if (node->GetNodeType() == SceneNode::CAMERA) {
		CameraComponent cameraComp;
		if (auto cameraNode = std::dynamic_pointer_cast<Camera>(node)) {
			cameraComp.up = cameraNode->GetCameraUpVector();
			cameraComp.front = cameraNode->GetCameraFrontVector();
			cameraComp.right = cameraNode->GetCameraRightVector();
			cameraComp.yaw = cameraNode->GetCameraFacingAngle();
			cameraComp.pitch = 0.0f;
			cameraComp.fov = cameraNode->GetCameraFov();
			cameraComp.aspectRatio = (m_screenH != 0) ? static_cast<float>(m_screenW) / static_cast<float>(m_screenH) : cameraComp.aspectRatio;
			cameraComp.nearPlane = cameraNode->GetCameraNearPlane();
			cameraComp.farPlane = cameraNode->GetCameraFarPlane();
			cameraComp.movementSpeed = cameraNode->GetCameraMovementSpeed();
			cameraComp.mouseSensitivity = cameraNode->GetCameraMouseSensitivity();
		}
		else {
			const glm::mat4 cameraTransform = node->GetTransform();
			cameraComp.right = glm::normalize(glm::vec3(cameraTransform[0]));
			cameraComp.up = glm::normalize(glm::vec3(cameraTransform[1]));
			cameraComp.front = glm::normalize(-glm::vec3(cameraTransform[2]));
			cameraComp.aspectRatio = (m_screenH != 0) ? static_cast<float>(m_screenW) / static_cast<float>(m_screenH) : cameraComp.aspectRatio;
		}
		componentManager->AddCamera(entityID, cameraComp);
	}

	// Create AnimationComponent if model has animations
	if (node->GetModel() && !node->GetModel()->animations.empty()) {
		CreateAnimationComponent(node, node->GetModel());
	}

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created ECS entity " << entityID << " for node: " << nodeName
			<< " (parent=" << parentID << ")" << std::endl;
	}
}

void SceneLoader::CreateRenderableComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model) {
	if (!node || !model || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	EntityID entityID = node->GetEntityID();
	if (!componentManager || entityID == INVALID_ENTITY) return;

	RenderableComponent renderComp;
	renderComp.model = model;
	renderComp.shaderID = node->GetShader();
	renderComp.boundingRadius = node->boundingRadius;
	renderComp.isSkinned = node->isSkinned;
	renderComp.hasAlpha = false;
	renderComp.renderWholeModel = node->renderWholeModel;
	renderComp.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(node->GetCullingOverride()));
	renderComp.meshIndices = node->renderMeshIndices;
	renderComp.boneNodes = node->boneNodes;
	renderComp.boneInverseBindMatrices = node->boneInverseBindMatrices;
	renderComp.nodeIndex = node->nodeIndex;

	auto markAlpha = [&](const MeshComponent& mesh) {
		if (mesh.RequiresAlphaBlending()) {
			renderComp.hasAlpha = true;
		}
	};

	if (renderComp.renderWholeModel) {
		for (const auto& mesh : model->meshes) {
			markAlpha(mesh);
		}
	}
	else {
		for (uint32_t meshIndex : renderComp.meshIndices) {
			if (meshIndex < model->meshes.size()) {
				markAlpha(model->meshes[meshIndex]);
			}
		}
	}

	componentManager->AddRenderable(entityID, renderComp);

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created RenderableComponent for entity " << entityID
			<< " with " << model->meshes.size() << " meshes" << std::endl;
	}
}

void SceneLoader::CreateExistingChildEntitiesRecursive(const std::shared_ptr<SceneNode>& node, EntityID parentID)
{
	if (!node) {
		return;
	}

	CreateECSEntity(node, parentID);
	const EntityID entityID = node->GetEntityID();
	const size_t childCount = node->children.size();
	for (size_t i = 0; i < childCount; ++i) {
		CreateExistingChildEntitiesRecursive(node->children[i], entityID);
	}
}

void SceneLoader::BuildImportedMeshNodeChildren(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& model)
{
	if (!node || !model || model->nodes.empty()) {
		return;
	}

	const std::vector<glm::mat4>& cachedSourceWorldTransforms = model->GetNodeWorldTransforms();
	const bool hasCachedSourceWorldTransforms = (cachedSourceWorldTransforms.size() == model->nodes.size());
	std::vector<glm::mat4> fallbackSourceWorldTransforms;
	if (!hasCachedSourceWorldTransforms) {
		fallbackSourceWorldTransforms = ComputeModelNodeWorldTransforms(*model);
	}
	const std::vector<glm::mat4>& sourceWorldTransforms = hasCachedSourceWorldTransforms
		? cachedSourceWorldTransforms
		: fallbackSourceWorldTransforms;

	std::vector<uint32_t> rootMeshIndices;
	std::vector<bool> meshAttached(model->meshes.size(), false);
	for (const Scene::NodeInfo& nodeInfo : model->nodes) {
		for (uint32_t meshIndex : nodeInfo.meshIndices) {
			if (meshIndex < meshAttached.size()) {
				meshAttached[meshIndex] = true;
			}
		}
	}
	for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(meshAttached.size()); ++meshIndex) {
		if (!meshAttached[meshIndex]) {
			rootMeshIndices.push_back(meshIndex);
		}
	}

	node->renderWholeModel = false;
	node->renderMeshIndices = rootMeshIndices;
	node->boundingRadius = ComputeRenderableBoundingRadius(*model, rootMeshIndices, -1);

	ComponentManager* componentManager = m_currentSceneGraph ? m_currentSceneGraph->GetComponentManager() : nullptr;
	TransformSystem* transformSystem = m_currentSceneGraph ? m_currentSceneGraph->GetTransformSystem() : nullptr;

	size_t invalidLocalTransformCount = 0;
	size_t reparentedToRootCount = 0;
	size_t correctedLocalTransformCount = 0;

	std::vector<std::shared_ptr<SceneNode>> importedNodes(model->nodes.size());
	for (size_t nodeIndex = 0; nodeIndex < model->nodes.size(); ++nodeIndex) {
		const Scene::NodeInfo& nodeInfo = model->nodes[nodeIndex];
		const bool hasRenderableMeshes = !nodeInfo.meshIndices.empty();
		glm::mat4 localTransform = nodeInfo.localTransform;
		if (!IsFiniteMatrix(localTransform)) {
			localTransform = glm::mat4(1.0f);
			++invalidLocalTransformCount;
		}

		auto importedNode = std::make_shared<SceneNode>(componentManager, transformSystem);
		importedNode->SetNodeType(hasRenderableMeshes ? SceneNode::MODEL : SceneNode::NODE);
		importedNode->SetShader(node->GetShader());
		importedNode->SetCullingOverride(node->GetCullingOverride());
		importedNode->SetTransform(localTransform);
		importedNode->SetName(!nodeInfo.name.empty()
			? nodeInfo.name
			: node->GetName() + "_node_" + std::to_string(nodeIndex));
		importedNode->nodeIndex = static_cast<int>(nodeIndex);

		if (hasRenderableMeshes) {
			importedNode->SetModel(model);
			importedNode->renderWholeModel = false;
			importedNode->renderMeshIndices = nodeInfo.meshIndices;
			importedNode->boundingRadius = ComputeRenderableBoundingRadius(*model, nodeInfo.meshIndices, static_cast<int>(nodeIndex));
		}

		importedNodes[nodeIndex] = importedNode;
	}

	for (size_t nodeIndex = 0; nodeIndex < model->nodes.size(); ++nodeIndex) {
		auto& importedNode = importedNodes[nodeIndex];
		if (!importedNode) {
			continue;
		}

		const int parentIndex = model->nodes[nodeIndex].parent;
		if (parentIndex >= 0 && parentIndex < static_cast<int>(importedNodes.size()) && importedNodes[parentIndex]) {
			importedNodes[parentIndex]->AddChild(importedNode);
		}
		else {
			if (parentIndex >= 0 && nodeIndex < sourceWorldTransforms.size()) {
				// Preserve authored world placement if malformed parent links force a root fallback.
				importedNode->SetTransform(sourceWorldTransforms[nodeIndex]);
				++reparentedToRootCount;
			}
			node->AddChild(importedNode);
		}
	}

	std::unordered_map<int, glm::mat4> importedWorldCache;
	std::function<glm::mat4(int)> computeImportedWorld = [&](int nodeIndex) -> glm::mat4 {
		auto cached = importedWorldCache.find(nodeIndex);
		if (cached != importedWorldCache.end()) {
			return cached->second;
		}

		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model->nodes.size()) || !importedNodes[nodeIndex]) {
			return glm::mat4(1.0f);
		}

		glm::mat4 localTransform = importedNodes[nodeIndex]->GetTransform();
		if (!IsFiniteMatrix(localTransform)) {
			localTransform = glm::mat4(1.0f);
		}

		const int parentIndex = model->nodes[nodeIndex].parent;
		glm::mat4 worldTransform = (parentIndex >= 0)
			? computeImportedWorld(parentIndex) * localTransform
			: localTransform;
		importedWorldCache[nodeIndex] = worldTransform;
		return worldTransform;
	};

	for (size_t nodeIndex = 0; nodeIndex < importedNodes.size(); ++nodeIndex) {
		if (!importedNodes[nodeIndex] || nodeIndex >= sourceWorldTransforms.size()) {
			continue;
		}

		glm::mat4 importedWorld = computeImportedWorld(static_cast<int>(nodeIndex));
		const glm::mat4& sourceWorld = sourceWorldTransforms[nodeIndex];
		if (!IsFiniteMatrix(importedWorld) || !IsFiniteMatrix(sourceWorld)) {
			continue;
		}

		if (MaxMatrixAbsDelta(importedWorld, sourceWorld) <= kImportedTransformTolerance) {
			continue;
		}

		glm::mat4 correctedLocal = sourceWorld;
		const int parentIndex = model->nodes[nodeIndex].parent;
		if (parentIndex >= 0 && parentIndex < static_cast<int>(sourceWorldTransforms.size())) {
			glm::mat4 parentInverse = glm::inverse(sourceWorldTransforms[parentIndex]);
			if (IsFiniteMatrix(parentInverse)) {
				correctedLocal = parentInverse * sourceWorld;
			}
		}

		if (IsFiniteMatrix(correctedLocal)) {
			importedNodes[nodeIndex]->SetTransform(correctedLocal);
			importedWorldCache.clear();
			++correctedLocalTransformCount;
		}
	}

	if (invalidLocalTransformCount > 0 || reparentedToRootCount > 0 || correctedLocalTransformCount > 0) {
		std::cout << "[SceneLoader] Imported transform diagnostics for model node '" << node->GetName()
			<< "': invalidLocal=" << invalidLocalTransformCount
			<< ", reparentedToRoot=" << reparentedToRootCount
			<< ", correctedLocal=" << correctedLocalTransformCount
			<< std::endl;
	}
}

void SceneLoader::CreateAnimationComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model) {
	if (!node || !model || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	EntityID entityID = node->GetEntityID();
	if (!componentManager || entityID == INVALID_ENTITY) return;

	// Only create if model has animations
	if (model->animations.empty()) return;

	AnimationComponent animComp;
	animComp.isPlaying = false;
	animComp.isPaused = false;
	animComp.animationTime = 0.0f;
	animComp.currentAnimationIndex = -1;
	// Controller will be created when animation is played

	componentManager->AddAnimation(entityID, animComp);

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created AnimationComponent for entity " << entityID
			<< " with " << model->animations.size() << " animations available" << std::endl;
	}
}

void SceneLoader::ReportProgress(float progress, const std::string& stage) {
	progress = std::min(std::max(progress, 0.0f), 1.0f);

	// Publish via event bus for GUI systems
	GuiEventBus::GetInstance().PublishProgress(progress, stage);

	// Also call callback if registered
	if (m_progressCallback) {
		m_progressCallback(progress, stage);
	}
}

void SceneLoader::ReportAssetLoadingProgress(float t, int current, int total) {
	if (total <= 0) return;

	float assetProgress = (float)current / (float)total;
	float overallProgress = assetProgress * 0.3f;  // Assets are first 30% of load
	ReportProgress(overallProgress, "Loading Assets (" + std::to_string(current) + "/" + std::to_string(total) + ")");
}

void SceneLoader::ReportECSCreationProgress(float t, int current, int total) {
	if (total <= 0) return;

	float ecsProgress = (float)current / (float)total;
	float overallProgress = 0.3f + ecsProgress * 0.35f;  // ECS creation is 35% of load
	ReportProgress(overallProgress, "Creating ECS Entities (" + std::to_string(current) + "/" + std::to_string(total) + ")");
}


// Runtime State Management


bool SceneLoader::SaveRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot save runtime state: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Saving runtime state to: " << filepath << std::endl;

	try {
		nlohmann::json stateJson;

		// Version for forward/backward compatibility
		stateJson["version"] = "1.0";
		stateJson["format"] = "runtime_state";
		stateJson["timestamp"] = std::time(nullptr);

		// Serialize camera state
		if (camera) {
			nlohmann::json cameraJson;

			glm::vec3 pos = camera->GetCameraPosition();
			cameraJson["position"] = { pos.x, pos.y, pos.z };

			glm::vec3 front = camera->GetCameraFrontVector();
			cameraJson["front"] = { front.x, front.y, front.z };

			glm::vec3 up = camera->GetCameraUpVector();
			cameraJson["up"] = { up.x, up.y, up.z };

			cameraJson["fov"] = camera->GetCameraFov();
			cameraJson["near_plane"] = camera->GetCameraNearPlane();
			cameraJson["far_plane"] = camera->GetCameraFarPlane();
			cameraJson["movement_speed"] = camera->GetCameraMovementSpeed();
			cameraJson["mouse_sensitivity"] = camera->GetCameraMouseSensitivity();

			stateJson["camera"] = cameraJson;
		}

		// Serialize scene metadata
		stateJson["scene_name"] = sceneGraph->GetSceneName();
		stateJson["exposure"] = sceneGraph->m_exposure;
		stateJson["gamma"] = sceneGraph->m_gamma;
		stateJson["physics_enabled"] = sceneGraph->IsPhysicsEnabled();

		// Serialize all nodes with runtime state
		nlohmann::json nodesArray = nlohmann::json::array();
		auto root = sceneGraph->GetRoot();

		for (const auto& child : root->children) {
			if (child) {
				nlohmann::json nodeJson = SerializeNodeRecursive(child);
				if (!nodeJson.is_null()) {
					// Add runtime-specific data
					SerializeRuntimeNodeState(child, nodeJson, sceneGraph);
					nodesArray.push_back(nodeJson);
				}
			}
		}

		stateJson["nodes"] = nodesArray;

		// Write to file with pretty formatting
		std::ofstream outFile(filepath);
		if (!outFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open file for writing: " << filepath << std::endl;
			return false;
		}

		outFile << stateJson.dump(2);
		outFile.close();

		std::cout << "[SceneLoader] Successfully saved runtime state with "
			<< nodesArray.size() << " nodes" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error saving runtime state: " << e.what() << std::endl;
		return false;
	}
}

bool SceneLoader::LoadRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot load runtime state: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Loading runtime state from: " << filepath << std::endl;

	try {
		std::ifstream inFile(filepath);
		if (!inFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open runtime state file: " << filepath << std::endl;
			return false;
		}

		nlohmann::json stateJson;
		inFile >> stateJson;
		inFile.close();

		// Version check
		std::string version = stateJson.value("version", "unknown");
		std::string format = stateJson.value("format", "unknown");

		if (format != "runtime_state") {
			std::cerr << "[SceneLoader] Invalid runtime state format: " << format << std::endl;
			return false;
		}

		std::cout << "[SceneLoader] Loading runtime state version: " << version << std::endl;

		// Restore camera state
		if (camera && stateJson.contains("camera")) {
			const auto& cameraJson = stateJson["camera"];

			if (cameraJson.contains("position") && cameraJson["position"].is_array()) {
				glm::vec3 pos(
					cameraJson["position"][0],
					cameraJson["position"][1],
					cameraJson["position"][2]
				);
				camera->SetPosition(pos);
			}

			// Note: Camera doesn't expose setters for front/up vectors directly
			// They are derived from yaw/pitch which we'd need to calculate
			// For now, position restore is most critical
		}

		// Restore scene metadata
		if (stateJson.contains("exposure")) {
			sceneGraph->m_exposure = stateJson["exposure"];
		}
		if (stateJson.contains("gamma")) {
			sceneGraph->m_gamma = stateJson["gamma"];
		}

		// Restore node states
		if (stateJson.contains("nodes") && stateJson["nodes"].is_array()) {
			auto root = sceneGraph->GetRoot();
			RestoreNodeStates(stateJson["nodes"], root->children, sceneGraph);
		}

		std::cout << "[SceneLoader] Successfully loaded runtime state" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error loading runtime state: " << e.what() << std::endl;
		return false;
	}
}

void SceneLoader::SerializeRuntimeNodeState(const std::shared_ptr<SceneNode>& node,
	nlohmann::json& nodeJson,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node || !sceneGraph) return;

	EntityID entityID = node->GetEntityID();
	if (entityID == INVALID_ENTITY) return;

	ComponentManager* cm = sceneGraph->GetComponentManager();
	if (!cm) return;

	// Add runtime section
	nlohmann::json runtimeJson;

	// Animation state
	if (cm->HasAnimation(entityID)) {
		auto* animComp = cm->GetAnimation(entityID);
		if (animComp) {
			nlohmann::json animJson;
			animJson["current_animation_index"] = animComp->currentAnimationIndex;
			animJson["animation_time"] = animComp->animationTime;
			animJson["is_playing"] = animComp->isPlaying;
			animJson["is_paused"] = animComp->isPaused;

			if (!animComp->morphWeights.empty()) {
				animJson["morph_weights"] = animComp->morphWeights;
			}

			runtimeJson["animation"] = animJson;
		}
	}

	// Physics state (velocity, angular velocity, etc.)
	auto rb = node->GetRigidBody();
	if (rb) {
		nlohmann::json physicsJson;

		glm::vec3 velocity = rb->getVelocity();
		physicsJson["velocity"] = { velocity.x, velocity.y, velocity.z };

		glm::vec3 angularVel = rb->getAngularVelocity();
		physicsJson["angular_velocity"] = { angularVel.x, angularVel.y, angularVel.z };

		glm::vec3 acceleration = rb->getAcceleration();
		physicsJson["acceleration"] = { acceleration.x, acceleration.y, acceleration.z };

		runtimeJson["physics"] = physicsJson;
	}

	// Light state (if it's a light node with runtime modifications)
	if (node->GetNodeType() == SceneNode::LIGHT) {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			// Light properties already serialized in SerializeLightProperties
			// But add runtime-specific state here if needed
		}
	}

	// Store runtime data if any was added
	if (!runtimeJson.empty()) {
		nodeJson["runtime_state"] = runtimeJson;
	}

	// Recursively serialize children's runtime state
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		size_t childIndex = 0;
		for (const auto& child : node->children) {
			if (child && childIndex < nodeJson["children"].size()) {
				SerializeRuntimeNodeState(child, nodeJson["children"][childIndex], sceneGraph);
				childIndex++;
			}
		}
	}
}

void SceneLoader::RestoreNodeStates(const nlohmann::json& nodesArray,
	const std::vector<std::shared_ptr<SceneNode>>& nodes,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!nodesArray.is_array() || nodes.empty()) return;

	// The saved state preserves the exact order of nodes, so index matching is most accurate
	size_t nodeCount = std::min(nodesArray.size(), nodes.size());

	for (size_t i = 0; i < nodeCount; ++i) {
		const auto& nodeJson = nodesArray[i];
		const auto& node = nodes[i];

		if (!node) continue;

		// Verify type matches as a sanity check
		std::string savedType = nodeJson.value("type", "node");
		std::string actualType;
		switch (node->GetNodeType()) {
		case SceneNode::MODEL:     actualType = "model"; break;
		case SceneNode::LIGHT:     actualType = "light"; break;
		case SceneNode::AUDIO:     actualType = "audio"; break;
		case SceneNode::CAMERA:    actualType = "camera"; break;
		case SceneNode::GUI:       actualType = "gui"; break;
		case SceneNode::LPV_VOLUME: actualType = "lpv_volume"; break;
		case SceneNode::SKELETAL:  actualType = "skeletal"; break;
		default:                   actualType = "node"; break;
		}

		// Log if there's a type mismatch (shouldn't happen if scene hasn't changed)
		if (savedType != actualType) {
			std::cerr << "[SceneLoader] Warning: Type mismatch at index " << i
				<< " - saved: " << savedType << ", actual: " << actualType << std::endl;
		}

		// Restore the node state
		RestoreNodeState(nodeJson, node, sceneGraph);
	}

	// Log if counts don't match
	if (nodesArray.size() != nodes.size()) {
		std::cerr << "[SceneLoader] Warning: Node count mismatch - saved: "
			<< nodesArray.size() << ", actual: " << nodes.size() << std::endl;
	}
}

void SceneLoader::RestoreNodeState(const nlohmann::json& nodeJson,
	const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node || !sceneGraph) return;

	// Restore transform - use SetLocalTRS for proper rotation handling
	glm::vec3 pos = node->GetPosition();
	glm::vec3 scale = node->GetScale();
	glm::quat rotation = node->GetOrientation();

	// Extract position if present
	if (nodeJson.contains("position") && nodeJson["position"].is_array() && nodeJson["position"].size() == 3) {
		pos = glm::vec3(
			nodeJson["position"][0],
			nodeJson["position"][1],
			nodeJson["position"][2]
		);
	}

	// Extract rotation if present - stored in degrees, convert to quaternion
	if (nodeJson.contains("rotation") && nodeJson["rotation"].is_array() && nodeJson["rotation"].size() == 3) {
		// Rotation is stored in degrees, convert to radians then to quaternion
		glm::vec3 rotDegrees(
			nodeJson["rotation"][0],
			nodeJson["rotation"][1],
			nodeJson["rotation"][2]
		);
		glm::vec3 rotRadians = glm::radians(rotDegrees);
		// Use glm::quat from Euler angles - this correctly combines all three rotations
		rotation = glm::quat(rotRadians);
	}

	// Extract scale if present
	if (nodeJson.contains("scale") && nodeJson["scale"].is_array() && nodeJson["scale"].size() == 3) {
		scale = glm::vec3(
			nodeJson["scale"][0],
			nodeJson["scale"][1],
			nodeJson["scale"][2]
		);
	}

	// Apply transform using SetLocalTRS which correctly combines TRS
	node->SetLocalTRS(pos, rotation, scale);

	// Sync transform to ECS if entity exists
	if (node->GetEntityID() != INVALID_ENTITY) {
		node->SyncToECS();
	}

	// Always sync physics body to match restored transform (even without runtime_state)
	auto rb = node->GetRigidBody();
	if (rb) {
		rb->setPosition(node->GetWorldPosition());
		rb->setOrientation(node->GetOrientation());
		rb->computeAABB();
		rb->storePreviousState();
	}

	// Restore runtime state if present
	if (nodeJson.contains("runtime_state")) {
		const auto& runtimeJson = nodeJson["runtime_state"];

		EntityID entityID = node->GetEntityID();
		ComponentManager* cm = sceneGraph->GetComponentManager();

		if (entityID != INVALID_ENTITY && cm) {
			// Restore animation state
			if (runtimeJson.contains("animation") && cm->HasAnimation(entityID)) {
				const auto& animJson = runtimeJson["animation"];
				auto* animComp = cm->GetAnimation(entityID);

				if (animComp) {
					animComp->currentAnimationIndex = animJson.value("current_animation_index", -1);
					animComp->animationTime = animJson.value("animation_time", 0.0f);
					animComp->isPlaying = animJson.value("is_playing", false);
					animComp->isPaused = animJson.value("is_paused", false);

					if (animJson.contains("morph_weights") && animJson["morph_weights"].is_array()) {
						animComp->morphWeights.clear();
						for (const auto& weight : animJson["morph_weights"]) {
							animComp->morphWeights.push_back(weight);
						}
					}
				}
			}

			// Restore physics runtime state (velocity, angular velocity, etc.)
			if (runtimeJson.contains("physics") && rb) {
				const auto& physicsJson = runtimeJson["physics"];

				if (physicsJson.contains("velocity") && physicsJson["velocity"].is_array()) {
					glm::vec3 velocity(
						physicsJson["velocity"][0],
						physicsJson["velocity"][1],
						physicsJson["velocity"][2]
					);
					rb->setVelocity(velocity);
				}

				if (physicsJson.contains("angular_velocity") && physicsJson["angular_velocity"].is_array()) {
					glm::vec3 angularVel(
						physicsJson["angular_velocity"][0],
						physicsJson["angular_velocity"][1],
						physicsJson["angular_velocity"][2]
					);
					rb->setAngularVelocity(angularVel);
				}

				if (physicsJson.contains("acceleration") && physicsJson["acceleration"].is_array()) {
					glm::vec3 acceleration(
						physicsJson["acceleration"][0],
						physicsJson["acceleration"][1],
						physicsJson["acceleration"][2]
					);
					rb->setAcceleration(acceleration);
				}
			}
		}
	}

	// Recursively restore children
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		RestoreNodeStates(nodeJson["children"], node->children, sceneGraph);
	}
}
