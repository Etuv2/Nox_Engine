#pragma once
#include <string>
#include <memory>
#include <functional>
#include <optional>
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

	// Loads the scene from a JSON file and returns a shared pointer to a SceneGraph. Blocking: BeginLoad, then
	// StepLoad until done, then TakeLoadedScene. Returns nullptr on failure.
	std::shared_ptr<SceneGraph> LoadScene(const std::string& sceneFilePath);

	// ---- Asynchronous loading (docs/scenes.md, "Loading") ----
	// A load is a job advanced by StepLoad on the calling (main) thread in small steps, so the caller can keep
	// pumping events and drawing a loading screen. File reads, JSON parsing and glTF decoding run on worker
	// threads. The live scene is not touched until the caller takes the result with TakeLoadedScene: the
	// previous scene stays intact while the load runs and when it fails.
	enum class LoadStatus { Idle, Running, Ready, Failed };
	// Starts a load. Returns false while another load is running (the caller waits or retries).
	bool BeginLoad(const std::string& sceneFilePath);
	// Runs steps until budgetMs has passed, or until the job waits on a worker. budgetMs < 0 runs to completion.
	LoadStatus StepLoad(double budgetMs);
	LoadStatus GetLoadStatus() const;
	// Ready: returns the new graph and removes the physics bodies of the scene it replaces. Otherwise nullptr.
	std::shared_ptr<SceneGraph> TakeLoadedScene();
	// Cancels a running or finished-but-untaken load: removes its physics bodies and restores the physics config.
	void CancelLoad();
	// Message of the last failed load (also published on GuiEventBus as an error).
	const std::string& GetLoadError() const { return m_loadError; }
	struct LoadProgress {
		float fraction = 0.0f;          // 0..1 over the whole load
		std::string stage;              // what the load is doing now, e.g. "Decoding models (12/120)"
		std::string sceneName;          // "scene_name" once the file is parsed
		std::string sceneFile;          // logical path as requested
	};
	LoadProgress GetLoadProgress() const;

	// Saves the current scene state back to a JSON file
	bool SaveScene(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sceneFilePath);

	// Editor helpers: serialize one node subtree in the scene-file schema, and instantiate such a
	// subtree into an existing graph under 'parent' (ECS entities, models and lights included).
	// A prefab instance is written as a reference (see src/scene/Prefab.h) unless it is the root.
	nlohmann::json SerializeNodeForEditor(const std::shared_ptr<SceneNode>& node);
	// Serializes a subtree. references: prefab instances below the root are written as references;
	// isRoot: the node itself is never written as a reference (flat content, used for prefab files and diffs).
	nlohmann::json SerializeNodeTree(const std::shared_ptr<SceneNode>& node, bool references, bool isRoot);
	// The node's own keys only (no children), as SerializeNodeTree writes them. Play-world snapshots use it.
	nlohmann::json SerializeNodeOwn(const std::shared_ptr<SceneNode>& node);
	// Writes the loaded graph to a physical file. sourceLogicalPath supplies the top-level keys the runtime does
	// not model (as SaveScene does). Used by tooling and the NOX_SAVE_ROUNDTRIP hook; never touches the source tree.
	bool SaveSceneCopy(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sourceLogicalPath,
	                   const std::string& physicalOutPath);
	std::shared_ptr<SceneNode> InstantiateNode(SceneGraph& graph, const nlohmann::json& nodeJson,
		const std::shared_ptr<SceneNode>& parent, const std::string& sceneFilePath);

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
		SKELETAL,
		FOLDER
	};

	static NodeType GetNodeType(const std::string& typeStr) {
		if (typeStr == "model")  return NodeType::MODEL;
		if (typeStr == "light")  return NodeType::LIGHT;
		if (typeStr == "audio")  return NodeType::AUDIO;
		if (typeStr == "camera") return NodeType::CAMERA;
		if (typeStr == "gui")    return NodeType::GUI;
		if (typeStr == "lpv_volume" || typeStr == "lpvvolume") return NodeType::LPV_VOLUME;
		if (typeStr == "skeletal") return NodeType::SKELETAL;
		if (typeStr == "folder") return NodeType::FOLDER;
		return NodeType::NODE;
	}

private:
	std::shared_ptr<ModelManager> m_modelManager;
	std::shared_ptr<PhysicsEngine> m_physicsEngine;
	int m_screenW, m_screenH;
	ProgressCallback m_progressCallback;

	// Asynchronous load state (see BeginLoad). The job's definition lives in SceneLoader.cpp.
	struct LoadJob;
	std::unique_ptr<LoadJob> m_job;
	LoadStatus m_loadStatus = LoadStatus::Idle;
	std::string m_loadError;
	// Physics bodies registered before the running load began: the replaced scene's bodies.
	std::vector<std::shared_ptr<RigidBody>> m_bodiesBeforeLoad;
	bool StepLoadOnce(LoadJob& job);          // one unit of work; false when it waits on a worker
	void FailLoad(const std::string& message);
	void RetirePhysicsBodies(bool removeReplaced);  // true: bodies of the replaced scene; false: the load's own bodies
	std::string m_currentSceneDirectory;
	size_t m_loadedModelCount = 0;
	size_t m_missingCriticalAssetCount = 0;
	// Child nodes emitted by a procedural generator while processing the current node; consumed
	// by ProcessNodeRecursive and attached as generated (non-serialized) children.
	nlohmann::json m_pendingGeneratedChildren;
	// Root "game" object of the scene being loaded (road_network for procedural districts).
	nlohmann::json m_sceneGameJson = nlohmann::json::object();
	// True while a prefab baseline is built for saving: no autoplay, no physics in the live world.
	bool m_scratchInstance = false;
	// Instantiates JSON whose prefab references are already expanded (ExpandReferences ran once).
	std::shared_ptr<SceneNode> InstantiateExpanded(SceneGraph& graph, const nlohmann::json& expanded,
		const std::shared_ptr<SceneNode>& parent, const std::string& sceneFilePath);
	void AddReflectionProbes(const nlohmann::json& probes);
	size_t m_loadedSkyboxCount = 0;
	size_t m_disabledSkyboxCount = 0;
	
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
	void CreateExistingChildEntitiesRecursive(const std::shared_ptr<SceneNode>& node, EntityID parentID);
	void BuildImportedMeshNodeChildren(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& model);

	// ECS entity and component creation
	void CreateECSEntity(std::shared_ptr<SceneNode> node, EntityID parentID = INVALID_ENTITY);
	// Records the node's authored keys and its loaded state for save (SceneNode::SetAuthoredBaseline).
	void StoreAuthoredState(const std::shared_ptr<SceneNode>& node, const nlohmann::json& nodeJson);
	void CreateRenderableComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model);
	void CreateAnimationComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model);

	// Saving methods
	nlohmann::json BuildSceneJson(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sourceLogicalPath);
	nlohmann::json SerializePrefabReference(const std::shared_ptr<SceneNode>& node);
	// Flattened subtree of a prefab as it instantiates with no instance edits (nested references expanded).
	std::optional<nlohmann::json> PrefabBaseline(const std::string& logicalPath);
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
