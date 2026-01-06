#include "RuntimeStateManager.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "Camera.h"
#include "Skybox.h"
#include "LightNode.h"
#include "AudioNode.h"
#include "ComponentManager.h"
#include "ComponentTypes.h"
#include "RigidBody.h"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <ctime>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using json = nlohmann::json;

RuntimeStateManager::RuntimeStateManager()
	: m_cachedNodeCount(0)
	, m_nodeCountValid(false)
{
}

RuntimeStateManager::~RuntimeStateManager() = default;

bool RuntimeStateManager::SaveState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[RuntimeStateManager] Cannot save: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[RuntimeStateManager] Saving complete state to: " << filepath << std::endl;

	try {
		json stateJson;

		// Header/metadata
		stateJson["format"] = FORMAT_TYPE;
		stateJson["version"] = FORMAT_VERSION;
		stateJson["timestamp"] = std::time(nullptr);
		stateJson["scene_name"] = sceneGraph->GetSceneName();

		// Store base scene file path for proper state loading
		// This allows us to load the correct scene before applying state
		if (!m_currentSceneFilePath.empty()) {
			stateJson["base_scene_file"] = m_currentSceneFilePath;
			std::cout << "[RuntimeStateManager] Saving state for scene: " << m_currentSceneFilePath << std::endl;
		}
		else {
			std::cerr << "[RuntimeStateManager] WARNING: No base scene file path set!" << std::endl;
		}

		// Add scene validation hash (for detecting scene modifications)
		stateJson["scene_node_count"] = CountSceneNodes(sceneGraph);

		// Camera state
		if (camera) {
			stateJson["camera"] = SerializeCamera(camera);
		}

		// Skybox configuration
		stateJson["skybox"] = SerializeSkybox(sceneGraph);

		// Environment settings
		stateJson["environment"] = SerializeEnvironment(sceneGraph);

		// All scene nodes with complete state
		json nodesArray = json::array();
		auto root = sceneGraph->GetRoot();
		for (const auto& child : root->children) {
			if (child) {
				json nodeJson = SerializeNodeRecursive(child, sceneGraph);
				if (!nodeJson.is_null()) {
					nodesArray.push_back(nodeJson);
				}
			}
		}
		stateJson["nodes"] = nodesArray;

		// Ensure directory exists
		std::filesystem::path filePath(filepath);
		if (filePath.has_parent_path()) {
			std::filesystem::create_directories(filePath.parent_path());
		}

		// Write to file
		std::ofstream outFile(filepath);
		if (!outFile.is_open()) {
			std::cerr << "[RuntimeStateManager] Failed to open file: " << filepath << std::endl;
			return false;
		}

		outFile << stateJson.dump(2);
		outFile.close();

		std::cout << "[RuntimeStateManager] Saved " << nodesArray.size() << " nodes successfully" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Save error: " << e.what() << std::endl;
		return false;
	}
}

bool RuntimeStateManager::LoadState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[RuntimeStateManager] Cannot load: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[RuntimeStateManager] Loading state from: " << filepath << std::endl;

	try {
		std::ifstream inFile(filepath);
		if (!inFile.is_open()) {
			std::cerr << "[RuntimeStateManager] Failed to open file: " << filepath << std::endl;
			return false;
		}

		json stateJson;
		inFile >> stateJson;
		inFile.close();

		// Version check
		std::string format = stateJson.value("format", "unknown");
		std::string version = stateJson.value("version", "unknown");

		if (format != FORMAT_TYPE && format != "runtime_state") {
			std::cerr << "[RuntimeStateManager] Invalid format: " << format << std::endl;
			return false;
		}

		std::cout << "[RuntimeStateManager] Loading version " << version << " state file" << std::endl;

		// Restore in deterministic order:
		// 1. Environment settings (exposure, gamma, physics)
		if (stateJson.contains("environment")) {
			RestoreEnvironment(stateJson["environment"], sceneGraph);
		}
		else {
			// Legacy format support
			if (stateJson.contains("exposure")) {
				sceneGraph->m_exposure = stateJson["exposure"];
			}
			if (stateJson.contains("gamma")) {
				sceneGraph->m_gamma = stateJson["gamma"];
			}
		}

		// 2. Camera (before nodes, as some node logic might reference camera)
		if (camera && stateJson.contains("camera")) {
			RestoreCamera(stateJson["camera"], camera);
		}

		// 3. Skybox (can be slow, do after quick state)
		if (stateJson.contains("skybox")) {
			RestoreSkybox(stateJson["skybox"], sceneGraph);
		}

		// 4. All nodes with their state
		if (stateJson.contains("nodes") && stateJson["nodes"].is_array()) {
			auto root = sceneGraph->GetRoot();
			RestoreNodesRecursive(stateJson["nodes"], root->children, sceneGraph);
		}

		std::cout << "[RuntimeStateManager] State loaded successfully" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Load error: " << e.what() << std::endl;
		return false;
	}
}

bool RuntimeStateManager::QuickSave(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera) {
	return SaveState(sceneGraph, camera, m_quickSavePath);
}

bool RuntimeStateManager::QuickLoad(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera) {
	return LoadState(sceneGraph, camera, m_quickSavePath);
}

// Load state with automatic base scene loading and validation
std::shared_ptr<SceneGraph> RuntimeStateManager::LoadStateWithSceneValidation(
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath,
	std::function<std::shared_ptr<SceneGraph>(const std::string&)> sceneLoaderCallback) {

	std::cout << "[RuntimeStateManager] Loading state with scene validation from: " << filepath << std::endl;

	try {
		// Step 1: Read and validate state file
		std::ifstream inFile(filepath);
		if (!inFile.is_open()) {
			std::cerr << "[RuntimeStateManager] ERROR: Failed to open state file: " << filepath << std::endl;
			return nullptr;
		}

		json stateJson;
		inFile >> stateJson;
		inFile.close();

		// Step 2: Validate format
		std::string format = stateJson.value("format", "unknown");
		std::string version = stateJson.value("version", "unknown");

		if (format != FORMAT_TYPE && format != "runtime_state") {
			std::cerr << "[RuntimeStateManager] ERROR: Invalid format '" << format
				<< "', expected '" << FORMAT_TYPE << "'" << std::endl;
			return nullptr;
		}

		std::cout << "[RuntimeStateManager] State file format: " << format << " version: " << version << std::endl;

		// Step 3: Extract base scene file path
		if (!stateJson.contains("base_scene_file")) {
			std::cerr << "[RuntimeStateManager] ERROR: State file missing 'base_scene_file' field." << std::endl;
			std::cerr << "[RuntimeStateManager] This state was saved with an older version and cannot be loaded safely." << std::endl;
			return nullptr;
		}

		std::string baseSceneFile = stateJson["base_scene_file"];

		// Validate base scene file path is not empty
		if (baseSceneFile.empty()) {
			std::cerr << "[RuntimeStateManager] ERROR: Base scene file path is empty!" << std::endl;
			return nullptr;
		}

		// Check if scene file exists
		if (!std::filesystem::exists(baseSceneFile)) {
			std::cerr << "[RuntimeStateManager] ERROR: Base scene file does not exist: " << baseSceneFile << std::endl;
			std::cerr << "[RuntimeStateManager] Please ensure the scene file is in the correct location." << std::endl;
			return nullptr;
		}

		std::cout << "[RuntimeStateManager] State requires base scene: " << baseSceneFile << std::endl;

		// Step 4: Load the base scene using callback
		if (!sceneLoaderCallback) {
			std::cerr << "[RuntimeStateManager] ERROR: No scene loader callback provided!" << std::endl;
			return nullptr;
		}

		std::cout << "[RuntimeStateManager] Loading base scene: " << baseSceneFile << std::endl;
		auto sceneGraph = sceneLoaderCallback(baseSceneFile);

		if (!sceneGraph || !sceneGraph->GetRoot()) {
			std::cerr << "[RuntimeStateManager] ERROR: Failed to load base scene: " << baseSceneFile << std::endl;
			return nullptr;
		}

		std::cout << "[RuntimeStateManager] Base scene loaded successfully: " << sceneGraph->GetSceneName() << std::endl;

		// Step 5: Validate scene matches state expectations
		int actualNodeCount = CountSceneNodes(sceneGraph);
		int expectedNodeCount = stateJson.value("scene_node_count", -1);

		if (expectedNodeCount >= 0 && actualNodeCount != expectedNodeCount) {
			std::cerr << "[RuntimeStateManager] WARNING: Scene node count mismatch!" << std::endl;
			std::cerr << "[RuntimeStateManager]   Expected: " << expectedNodeCount << " nodes" << std::endl;
			std::cerr << "[RuntimeStateManager]   Actual:   " << actualNodeCount << " nodes" << std::endl;
			std::cerr << "[RuntimeStateManager] The scene may have been modified since this state was saved." << std::endl;
			std::cerr << "[RuntimeStateManager] Proceeding with caution..." << std::endl;
		}
		else {
			std::cout << "[RuntimeStateManager] Scene validation passed: " << actualNodeCount << " nodes" << std::endl;
		}

		// Step 6: Apply state to the loaded scene
		std::cout << "[RuntimeStateManager] Applying saved state to loaded scene..." << std::endl;

		// Restore in deterministic order (same as LoadState):

		// 1. Environment settings
		if (stateJson.contains("environment")) {
			RestoreEnvironment(stateJson["environment"], sceneGraph);
		}
		else {
			// Legacy format support
			if (stateJson.contains("exposure")) {
				sceneGraph->m_exposure = stateJson["exposure"];
			}
			if (stateJson.contains("gamma")) {
				sceneGraph->m_gamma = stateJson["gamma"];
			}
		}

		// 2. Camera
		if (camera && stateJson.contains("camera")) {
			RestoreCamera(stateJson["camera"], camera);
		}

		// 3. Skybox
		if (stateJson.contains("skybox")) {
			RestoreSkybox(stateJson["skybox"], sceneGraph);
		}

		// 4. All nodes with their state
		if (stateJson.contains("nodes") && stateJson["nodes"].is_array()) {
			auto root = sceneGraph->GetRoot();
			RestoreNodesRecursive(stateJson["nodes"], root->children, sceneGraph);
		}

		std::cout << "[RuntimeStateManager] State applied successfully to scene: " << baseSceneFile << std::endl;

		// Update current scene file path for future saves
		m_currentSceneFilePath = baseSceneFile;
		m_nodeCountValid = false; // Invalidate cache after loading new state

		return sceneGraph;

	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] ERROR during state load: " << e.what() << std::endl;
		return nullptr;
	}
}

// ============================================================================
// Serialization Helpers
// ============================================================================

int RuntimeStateManager::CountSceneNodes(const std::shared_ptr<SceneGraph>& sceneGraph) const {
	if (!sceneGraph || !sceneGraph->GetRoot()) return 0;

	// Use cached value if valid
	if (m_nodeCountValid) {
		return m_cachedNodeCount;
	}

	int count = 0;
	std::function<void(const std::shared_ptr<SceneNode>&)> countRecursive =
		[&](const std::shared_ptr<SceneNode>& node) {
		if (!node) return;
		count++;
		for (const auto& child : node->children) {
			countRecursive(child);
		}
		};
	for (const auto& child : sceneGraph->GetRoot()->children) {
		countRecursive(child);
	}

	// Cache the result
	m_cachedNodeCount = count;
	m_nodeCountValid = true;

	return count;
}

json RuntimeStateManager::SerializeCamera(const std::shared_ptr<Camera>& camera) {
	json cameraJson;

	glm::vec3 pos = camera->GetCameraPosition();
	cameraJson["position"] = { pos.x, pos.y, pos.z };

	glm::vec3 front = camera->GetCameraFrontVector();
	cameraJson["front"] = { front.x, front.y, front.z };

	glm::vec3 up = camera->GetCameraUpVector();
	cameraJson["up"] = { up.x, up.y, up.z };

	glm::vec3 right = camera->GetCameraRightVector();
	cameraJson["right"] = { right.x, right.y, right.z };

	cameraJson["fov"] = camera->GetCameraFov();
	cameraJson["near_plane"] = camera->GetCameraNearPlane();
	cameraJson["far_plane"] = camera->GetCameraFarPlane();
	cameraJson["movement_speed"] = camera->GetCameraMovementSpeed();
	cameraJson["mouse_sensitivity"] = camera->GetCameraMouseSensitivity();

	return cameraJson;
}

json RuntimeStateManager::SerializeSkybox(const std::shared_ptr<SceneGraph>& sceneGraph) {
	json skyboxJson;

	auto skybox = sceneGraph->GetSkybox();
	if (skybox) {
		skyboxJson["enabled"] = true;
		skyboxJson["hdr_path"] = skybox->GetHDRPath();
		skyboxJson["ibl_intensity"] = skybox->GetIBLIntensity();
		skyboxJson["skybox_exposure"] = skybox->GetSkyboxExposure();
		skyboxJson["diffuse_scale"] = skybox->GetDiffuseIBLScale();
		skyboxJson["specular_scale"] = skybox->GetSpecularIBLScale();
	}
	else {
		skyboxJson["enabled"] = false;
	}

	return skyboxJson;
}

json RuntimeStateManager::SerializeEnvironment(const std::shared_ptr<SceneGraph>& sceneGraph) {
	json envJson;

	envJson["exposure"] = sceneGraph->m_exposure;
	envJson["gamma"] = sceneGraph->m_gamma;
	envJson["physics_enabled"] = sceneGraph->IsPhysicsEnabled();

	return envJson;
}

json RuntimeStateManager::SerializeNodeRecursive(const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node) return json();

	json nodeJson;

	// Node identification
	nodeJson["name"] = node->GetName();

	// Node type
	SceneNode::NODE_TYPE nodeType = node->GetNodeType();
	switch (nodeType) {
	case SceneNode::MODEL:     nodeJson["type"] = "model"; break;
	case SceneNode::LIGHT:     nodeJson["type"] = "light"; break;
	case SceneNode::AUDIO:     nodeJson["type"] = "audio"; break;
	case SceneNode::CAMERA:    nodeJson["type"] = "camera"; break;
	case SceneNode::GUI:       nodeJson["type"] = "gui"; break;
	case SceneNode::LPV_VOLUME: nodeJson["type"] = "lpv_volume"; break;
	default:                   nodeJson["type"] = "node"; break;
	}

	// Transform (always serialize)
	nodeJson["transform"] = SerializeNodeTransform(node);

	// Runtime state (animations, physics, etc.)
	json runtimeJson = SerializeNodeRuntime(node, sceneGraph);
	if (!runtimeJson.empty()) {
		nodeJson["runtime"] = runtimeJson;
	}

	// Type-specific properties
	switch (nodeType) {
	case SceneNode::LIGHT: {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			auto light = lightNode->GetLight();
			json lightJson;

			glm::vec3 color = light->GetColor();
			lightJson["color"] = { color.r, color.g, color.b };
			lightJson["intensity"] = light->GetIntensity();
			lightJson["enabled"] = light->IsEnabled();
			lightJson["casts_shadows"] = light->CastsShadows();
			lightJson["range"] = light->GetRange();

			nodeJson["light_properties"] = lightJson;
		}
		break;
	}
	case SceneNode::AUDIO: {
		auto audioNode = std::dynamic_pointer_cast<AudioNode>(node);
		if (audioNode) {
			json audioJson;
			audioJson["pitch"] = audioNode->getPitch();
			audioJson["volume"] = audioNode->getVolume();
			audioJson["hearing_distance"] = audioNode->getHearingDistance();
			audioJson["is_playing"] = audioNode->isPlaying();
			audioJson["is_3d"] = audioNode->is3D();
			nodeJson["audio_properties"] = audioJson;
		}
		break;
	}
	case SceneNode::LPV_VOLUME: {
		const auto& lpvData = node->GetLPVVolumeData();
		json lpvJson;
		lpvJson["center"] = { lpvData.center.x, lpvData.center.y, lpvData.center.z };
		lpvJson["extent"] = { lpvData.extent.x, lpvData.extent.y, lpvData.extent.z };
		lpvJson["voxel_size"] = lpvData.voxelSize;
		lpvJson["grid_resolution"] = lpvData.gridResolution;
		nodeJson["lpv_properties"] = lpvJson;
		break;
	}
	default:
		break;
	}

	// Children (recursive)
	if (!node->children.empty()) {
		json childrenArray = json::array();
		for (const auto& child : node->children) {
			if (child) {
				json childJson = SerializeNodeRecursive(child, sceneGraph);
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

json RuntimeStateManager::SerializeNodeTransform(const std::shared_ptr<SceneNode>& node) {
	json transformJson;

	glm::vec3 pos = node->GetPosition();
	transformJson["position"] = { pos.x, pos.y, pos.z };

	glm::vec3 rot = node->GetRotation();
	// Store in degrees for human readability
	transformJson["rotation"] = { glm::degrees(rot.x), glm::degrees(rot.y), glm::degrees(rot.z) };

	glm::vec3 scale = node->GetScale();
	transformJson["scale"] = { scale.x, scale.y, scale.z };

	// Also store quaternion for precision
	glm::quat orientation = node->GetOrientation();
	transformJson["orientation"] = { orientation.w, orientation.x, orientation.y, orientation.z };

	return transformJson;
}

json RuntimeStateManager::SerializeNodeRuntime(const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	json runtimeJson;

	EntityID entityID = node->GetEntityID();
	if (entityID == INVALID_ENTITY || !sceneGraph) return runtimeJson;

	ComponentManager* cm = sceneGraph->GetComponentManager();
	if (!cm) return runtimeJson;

	// Animation state
	if (cm->HasAnimation(entityID)) {
		auto* animComp = cm->GetAnimation(entityID);
		if (animComp) {
			json animJson;
			animJson["current_index"] = animComp->currentAnimationIndex;
			animJson["time"] = animComp->animationTime;
			animJson["playing"] = animComp->isPlaying;
			animJson["paused"] = animComp->isPaused;
			if (!animComp->morphWeights.empty()) {
				animJson["morph_weights"] = animComp->morphWeights;
			}
			runtimeJson["animation"] = animJson;
		}
	}

	// Physics state
	auto rb = node->GetRigidBody();
	if (rb) {
		json physicsJson;

		glm::vec3 velocity = rb->getVelocity();
		physicsJson["velocity"] = { velocity.x, velocity.y, velocity.z };

		glm::vec3 angularVel = rb->getAngularVelocity();
		physicsJson["angular_velocity"] = { angularVel.x, angularVel.y, angularVel.z };

		glm::vec3 acceleration = rb->getAcceleration();
		physicsJson["acceleration"] = { acceleration.x, acceleration.y, acceleration.z };

		runtimeJson["physics"] = physicsJson;
	}

	return runtimeJson;
}

// ============================================================================
// Deserialization Helpers
// ============================================================================

bool RuntimeStateManager::RestoreCamera(const nlohmann::json& cameraJson,
	const std::shared_ptr<Camera>& camera) {
	if (!camera) return false;

	try {
		if (cameraJson.contains("position") && cameraJson["position"].is_array()) {
			glm::vec3 pos(
				cameraJson["position"][0],
				cameraJson["position"][1],
				cameraJson["position"][2]
			);
			camera->SetPosition(pos);
		}

		// TODO: Restore yaw/pitch from front vector if Camera exposes setters
		// For now, position is the most critical

		return true;
	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Camera restore error: " << e.what() << std::endl;
		return false;
	}
}

bool RuntimeStateManager::RestoreSkybox(const nlohmann::json& skyboxJson,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!skyboxJson.value("enabled", false)) {
		return true; // Skybox disabled, nothing to restore
	}

	auto skybox = sceneGraph->GetSkybox();
	if (!skybox) {
		std::cout << "[RuntimeStateManager] No skybox to restore settings to" << std::endl;
		return true;
	}

	try {
		if (skyboxJson.contains("ibl_intensity")) {
			skybox->SetIBLIntensity(skyboxJson["ibl_intensity"]);
		}
		if (skyboxJson.contains("skybox_exposure")) {
			skybox->SetSkyboxExposure(skyboxJson["skybox_exposure"]);
		}
		if (skyboxJson.contains("diffuse_scale")) {
			skybox->SetDiffuseIBLScale(skyboxJson["diffuse_scale"]);
		}
		if (skyboxJson.contains("specular_scale")) {
			skybox->SetSpecularIBLScale(skyboxJson["specular_scale"]);
		}

		return true;
	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Skybox restore error: " << e.what() << std::endl;
		return false;
	}
}

bool RuntimeStateManager::RestoreEnvironment(const nlohmann::json& envJson,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	try {
		if (envJson.contains("exposure")) {
			sceneGraph->m_exposure = envJson["exposure"];
		}
		if (envJson.contains("gamma")) {
			sceneGraph->m_gamma = envJson["gamma"];
		}
		// Note: physics_enabled should be handled by PhysicsEngine, not just scene graph flag

		return true;
	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Environment restore error: " << e.what() << std::endl;
		return false;
	}
}

void RuntimeStateManager::RestoreNodesRecursive(const nlohmann::json& nodesJson,
	const std::vector<std::shared_ptr<SceneNode>>& nodes,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!nodesJson.is_array() || nodes.empty()) return;

	// Primary strategy: Match by index position (most reliable for scene state)
	// The saved state preserves the exact order of nodes, so index matching is most accurate
	size_t nodeCount = std::min(nodesJson.size(), nodes.size());

	for (size_t i = 0; i < nodeCount; ++i) {
		const auto& nodeJson = nodesJson[i];
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
			std::cerr << "[RuntimeStateManager] Warning: Type mismatch at index " << i
				<< " - saved: " << savedType << ", actual: " << actualType << std::endl;
		}

		// Restore the node state
		RestoreNodeState(nodeJson, node, sceneGraph);
	}

	// Log if counts don't match
	if (nodesJson.size() != nodes.size()) {
		std::cerr << "[RuntimeStateManager] Warning: Node count mismatch - saved: "
			<< nodesJson.size() << ", actual: " << nodes.size() << std::endl;
	}
}

void RuntimeStateManager::RestoreNodeState(const nlohmann::json& nodeJson,
	const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node || !sceneGraph) return;

	try {
		// Restore transform
		if (nodeJson.contains("transform")) {
			const auto& transformJson = nodeJson["transform"];

			// Prefer quaternion if available for precision
			if (transformJson.contains("orientation") && transformJson["orientation"].is_array()) {
				glm::quat orientation(
					transformJson["orientation"][0],  // w
					transformJson["orientation"][1],  // x
					transformJson["orientation"][2],  // y
					transformJson["orientation"][3]   // z
				);

				glm::vec3 pos(0.0f);
				if (transformJson.contains("position") && transformJson["position"].is_array()) {
					pos = glm::vec3(
						transformJson["position"][0],
						transformJson["position"][1],
						transformJson["position"][2]
					);
				}

				glm::vec3 scale(1.0f);
				if (transformJson.contains("scale") && transformJson["scale"].is_array()) {
					scale = glm::vec3(
						transformJson["scale"][0],
						transformJson["scale"][1],
						transformJson["scale"][2]
					);
				}

				node->SetLocalTRS(pos, orientation, scale);
			}
			else {
				// Fallback to Euler angles -> quaternion conversion
				glm::vec3 pos = node->GetPosition();
				glm::vec3 scale = node->GetScale();
				glm::quat rotation = node->GetOrientation();

				if (transformJson.contains("position") && transformJson["position"].is_array()) {
					pos = glm::vec3(
						transformJson["position"][0],
						transformJson["position"][1],
						transformJson["position"][2]
					);
				}

				if (transformJson.contains("rotation") && transformJson["rotation"].is_array()) {
					// Rotation stored in degrees, convert to quaternion
					glm::vec3 rotDegrees(
						transformJson["rotation"][0],
						transformJson["rotation"][1],
						transformJson["rotation"][2]
					);
					glm::vec3 rotRadians = glm::radians(rotDegrees);
					rotation = glm::quat(rotRadians);
				}

				if (transformJson.contains("scale") && transformJson["scale"].is_array()) {
					scale = glm::vec3(
						transformJson["scale"][0],
						transformJson["scale"][1],
						transformJson["scale"][2]
					);
				}

				node->SetLocalTRS(pos, rotation, scale);
			}
		}
		// Legacy format support (position/rotation/scale at root level)
		else if (nodeJson.contains("position")) {
			glm::vec3 pos = node->GetPosition();
			glm::vec3 scale = node->GetScale();
			glm::quat rotation = node->GetOrientation();

			if (nodeJson["position"].is_array()) {
				pos = glm::vec3(
					nodeJson["position"][0],
					nodeJson["position"][1],
					nodeJson["position"][2]
				);
			}

			if (nodeJson.contains("rotation") && nodeJson["rotation"].is_array()) {
				glm::vec3 rotDegrees(
					nodeJson["rotation"][0],
					nodeJson["rotation"][1],
					nodeJson["rotation"][2]
				);
				glm::vec3 rotRadians = glm::radians(rotDegrees);
				rotation = glm::quat(rotRadians);
			}

			if (nodeJson.contains("scale") && nodeJson["scale"].is_array()) {
				scale = glm::vec3(
					nodeJson["scale"][0],
					nodeJson["scale"][1],
					nodeJson["scale"][2]
				);
			}

			node->SetLocalTRS(pos, rotation, scale);
		}

		// Sync transform to ECS after restoring
		if (node->GetEntityID() != INVALID_ENTITY) {
			node->SyncToECS();
		}

		// Sync physics body to match restored transform
		auto rb = node->GetRigidBody();
		if (rb) {
			rb->setPosition(node->GetWorldPosition());
			rb->setOrientation(node->GetOrientation());
			rb->computeAABB();
			rb->storePreviousState();
		}

		// Restore runtime state
		if (nodeJson.contains("runtime")) {
			const auto& runtimeJson = nodeJson["runtime"];

			EntityID entityID = node->GetEntityID();
			ComponentManager* cm = sceneGraph->GetComponentManager();

			if (entityID != INVALID_ENTITY && cm) {
				// Animation state
				if (runtimeJson.contains("animation") && cm->HasAnimation(entityID)) {
					const auto& animJson = runtimeJson["animation"];
					auto* animComp = cm->GetAnimation(entityID);

					if (animComp) {
						animComp->currentAnimationIndex = animJson.value("current_index", -1);
						animComp->animationTime = animJson.value("time", 0.0f);
						animComp->isPlaying = animJson.value("playing", false);
						animComp->isPaused = animJson.value("paused", false);

						if (animJson.contains("morph_weights") && animJson["morph_weights"].is_array()) {
							animComp->morphWeights.clear();
							for (const auto& w : animJson["morph_weights"]) {
								animComp->morphWeights.push_back(w);
							}
						}
					}
				}

				// Physics state (velocity, angular velocity, etc.)
				if (runtimeJson.contains("physics") && rb) {
					const auto& physicsJson = runtimeJson["physics"];

					if (physicsJson.contains("velocity") && physicsJson["velocity"].is_array()) {
						rb->setVelocity(glm::vec3(
							physicsJson["velocity"][0],
							physicsJson["velocity"][1],
							physicsJson["velocity"][2]
						));
					}
					if (physicsJson.contains("angular_velocity") && physicsJson["angular_velocity"].is_array()) {
						rb->setAngularVelocity(glm::vec3(
							physicsJson["angular_velocity"][0],
							physicsJson["angular_velocity"][1],
							physicsJson["angular_velocity"][2]
						));
					}
					if (physicsJson.contains("acceleration") && physicsJson["acceleration"].is_array()) {
						rb->setAcceleration(glm::vec3(
							physicsJson["acceleration"][0],
							physicsJson["acceleration"][1],
							physicsJson["acceleration"][2]
						));
					}
				}
			}
		}
		// Legacy runtime_state support
		else if (nodeJson.contains("runtime_state")) {
			const auto& runtimeJson = nodeJson["runtime_state"];
			EntityID entityID = node->GetEntityID();
			ComponentManager* cm = sceneGraph->GetComponentManager();

			if (entityID != INVALID_ENTITY && cm) {
				if (runtimeJson.contains("animation") && cm->HasAnimation(entityID)) {
					const auto& animJson = runtimeJson["animation"];
					auto* animComp = cm->GetAnimation(entityID);
					if (animComp) {
						animComp->currentAnimationIndex = animJson.value("current_animation_index", -1);
						animComp->animationTime = animJson.value("animation_time", 0.0f);
						animComp->isPlaying = animJson.value("is_playing", false);
						animComp->isPaused = animJson.value("is_paused", false);
					}
				}

				if (runtimeJson.contains("physics") && rb) {
					const auto& physicsJson = runtimeJson["physics"];
					if (physicsJson.contains("velocity") && physicsJson["velocity"].is_array()) {
						rb->setVelocity(glm::vec3(
							physicsJson["velocity"][0],
							physicsJson["velocity"][1],
							physicsJson["velocity"][2]
						));
					}
				}
			}
		}

		// Type-specific properties
		if (node->GetNodeType() == SceneNode::LIGHT && nodeJson.contains("light_properties")) {
			auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
			if (lightNode && lightNode->GetLight()) {
				const auto& lightJson = nodeJson["light_properties"];
				auto light = lightNode->GetLight();

				if (lightJson.contains("color") && lightJson["color"].is_array()) {
					light->SetColor(glm::vec3(
						lightJson["color"][0],
						lightJson["color"][1],
						lightJson["color"][2]
					));
				}
				if (lightJson.contains("intensity")) {
					light->SetIntensity(lightJson["intensity"]);
				}
				if (lightJson.contains("enabled")) {
					light->SetEnabled(lightJson["enabled"]);
				}
			}
		}

		if (node->GetNodeType() == SceneNode::AUDIO && nodeJson.contains("audio_properties")) {
			auto audioNode = std::dynamic_pointer_cast<AudioNode>(node);
			if (audioNode) {
				const auto& audioJson = nodeJson["audio_properties"];
				if (audioJson.contains("pitch")) audioNode->setPitch(audioJson["pitch"]);
				if (audioJson.contains("volume")) audioNode->setVolume(audioJson["volume"]);
				if (audioJson.contains("hearing_distance")) {
					audioNode->setHearingDistance(audioJson["hearing_distance"]);
				}
			}
		}

		// Recursively restore children
		if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
			RestoreNodesRecursive(nodeJson["children"], node->children, sceneGraph);
		}

	}
	catch (const std::exception& e) {
		std::cerr << "[RuntimeStateManager] Node restore error for '" << node->GetName()
			<< "': " << e.what() << std::endl;
	}
}
