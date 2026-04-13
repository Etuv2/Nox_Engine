#pragma once
#include <string>
#include <memory>
#include <functional>
#include "SceneGraph.h"
#include "ModelManager.h"
#include "PhysicsEngine.h"
#include "GuiEventBus.h"
#include "json.hpp"
#include "ComponentTypes.h"

class GuiNode;
class LightNode;
class HierarchySystem;
class AnimationSystem;
class RenderSystem;
class Camera;

/**
 * SceneLoader - Loads and saves scenes from/to JSON files
 *
 * ECS Integration:
 * - Creates ECS entities for each node during loading
 * - Populates TransformComponent, RenderableComponent, AnimationComponent
 * - Establishes hierarchy relationships via TransformComponent::parentID
 * - Physics components linked to ECS entities
 * 
 * Progress Tracking:
 * - Publishes progress events via GuiEventBus during loading
 * - Supports multi-stage loading (Assets, ECS, Physics, etc.)
 * - Non-blocking design allows GUI animations during load
 */
class SceneLoader {
public:
	SceneLoader(std::shared_ptr<ModelManager> modelManager, std::shared_ptr<PhysicsEngine> physicsEngine, int screenW, int screenH);
	~SceneLoader();

	// Loads the scene from a JSON file and returns a shared pointer to a SceneGraph.
	std::shared_ptr<SceneGraph> LoadScene(const std::string& sceneFilePath);

	// Saves the current scene state back to a JSON file
	bool SaveScene(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sceneFilePath);

	// Runtime state management - captures full runtime state including camera and all node states
	bool SaveRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph, 
	                      const std::shared_ptr<class Camera>& camera,
	                      const std::string& filepath);
	bool LoadRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph,
	                      const std::shared_ptr<class Camera>& camera,
	                      const std::string& filepath);

	// Progress callback for loading stages
	using ProgressCallback = std::function<void(float progress, const std::string& stage)>;
	void SetProgressCallback(ProgressCallback callback) { m_progressCallback = callback; }

	// Node type enumeration
	enum class NodeType {
		NODE,
		AUDIO,
		MODEL,
		LIGHT,
		CAMERA,
		GUI,
		LPV_VOLUME,
		SKELETAL
	};

	static NodeType GetNodeType(const std::string& typeStr) {
		if (typeStr == "model")  return NodeType::MODEL;
		if (typeStr == "light")  return NodeType::LIGHT;
		if (typeStr == "audio")  return NodeType::AUDIO;
		if (typeStr == "camera") return NodeType::CAMERA;
		if (typeStr == "gui")    return NodeType::GUI;
		if (typeStr == "lpv_volume" || typeStr == "lpvvolume") return NodeType::LPV_VOLUME;
		if (typeStr == "skeletal") return NodeType::SKELETAL;
		return NodeType::NODE;
	}

private:
	std::shared_ptr<ModelManager> m_modelManager;
	std::shared_ptr<PhysicsEngine> m_physicsEngine;
	int m_screenW, m_screenH;
	ProgressCallback m_progressCallback;
	
	// Internal helper methods for progress reporting
	void ReportProgress(float progress, const std::string& stage);
	void ReportAssetLoadingProgress(float t, int current, int total);
	void ReportECSCreationProgress(float t, int current, int total);

	// Current scene graph being loaded (for ECS access)
	SceneGraph* m_currentSceneGraph = nullptr;

	// Loading methods
	std::shared_ptr<SceneNode> ProcessNodeRecursive(const nlohmann::json& nodeJson, EntityID parentEntityID = INVALID_ENTITY);
	std::shared_ptr<SceneNode> ProcessNode(const nlohmann::json& nodeJson);
	void ProcessCollider(const nlohmann::json& colliderJson, std::shared_ptr<SceneNode> node);
	void ProcessGuiElementProperties(std::shared_ptr<GuiNode> guiNode, int elementId, const nlohmann::json& elementJson);
	void CreateExistingChildEntitiesRecursive(const std::shared_ptr<SceneNode>& node, EntityID parentID);
	void BuildImportedMeshNodeChildren(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& model);

	// ECS entity and component creation
	void CreateECSEntity(std::shared_ptr<SceneNode> node, EntityID parentID = INVALID_ENTITY);
	void CreateRenderableComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model);
	void CreateAnimationComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model);

	// Saving methods
	nlohmann::json SerializeNodeRecursive(const std::shared_ptr<SceneNode>& node);
	nlohmann::json SerializeNode(const std::shared_ptr<SceneNode>& node);
	nlohmann::json SerializeCollider(const std::shared_ptr<SceneNode>& node);
	nlohmann::json SerializeGuiElements(const std::shared_ptr<GuiNode>& guiNode);
	nlohmann::json SerializeLightProperties(const std::shared_ptr<LightNode>& lightNode);
	
	// Runtime state helpers
	void SerializeRuntimeNodeState(const std::shared_ptr<SceneNode>& node, nlohmann::json& nodeJson, 
	                                const std::shared_ptr<SceneGraph>& sceneGraph);
	void RestoreNodeStates(const nlohmann::json& nodesArray, 
	                       const std::vector<std::shared_ptr<SceneNode>>& nodes,
	                       const std::shared_ptr<SceneGraph>& sceneGraph);
	void RestoreNodeState(const nlohmann::json& nodeJson, const std::shared_ptr<SceneNode>& node,
	                      const std::shared_ptr<SceneGraph>& sceneGraph);
};
