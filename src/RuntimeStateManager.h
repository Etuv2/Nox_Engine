#pragma once

#include <memory>
#include <string>
#include <functional>
#include "json.hpp"

// Forward declarations
class SceneGraph;
class SceneNode;
class Camera;
class SceneLoader;
class ModelManager;

/**
 * @brief Unified runtime state management for complete scene serialization
 * 
 * This class provides a single entry point for saving and loading the complete
 * runtime state of a scene, including:
 * - Camera position, orientation, and settings
 * - Skybox/environment configuration
 * - All scene nodes with transforms and type-specific properties
 * - Animation states
 * - Physics states (velocities, etc.)
 * - Lighting parameters
 * 
 * The format is versioned for forward/backward compatibility.
 */
class RuntimeStateManager {
public:
    static constexpr const char* FORMAT_VERSION = "2.0";
    static constexpr const char* FORMAT_TYPE = "nox_scene_state";

    RuntimeStateManager();
    ~RuntimeStateManager();

    /**
     * @brief Set the current scene file path for state saving
     * @param sceneFilePath Path to the currently loaded scene file
     */
    void SetCurrentSceneFilePath(const std::string& sceneFilePath) {
        m_currentSceneFilePath = sceneFilePath;
        m_nodeCountValid = false; // Invalidate cache when scene changes
    }
    
    /**
     * @brief Get the current scene file path
     * @return Path to the currently tracked scene file
     */
    std::string GetCurrentSceneFilePath() const {
        return m_currentSceneFilePath;
    }

    /**
     * @brief Save complete scene state to file
     * @param sceneGraph The scene graph to save
     * @param camera The main camera
     * @param filepath Output file path
     * @return true if save succeeded
     */
    bool SaveState(const std::shared_ptr<SceneGraph>& sceneGraph,
                   const std::shared_ptr<Camera>& camera,
                   const std::string& filepath);

    /**
     * @brief Load complete scene state from file (onto current scene)
     * @param sceneGraph The scene graph to restore into
     * @param camera The main camera to restore
     * @param filepath Input file path
     * @return true if load succeeded
     */
    bool LoadState(const std::shared_ptr<SceneGraph>& sceneGraph,
                   const std::shared_ptr<Camera>& camera,
                   const std::string& filepath);
    
    /**
     * @brief Load state with automatic scene loading and validation
     * 
     * This method ensures the correct base scene is loaded before applying state.
     * It will:
     * 1. Read the state file and extract the base scene path
     * 2. Call the scene loader callback to load the base scene if needed
     * 3. Validate the loaded scene matches the state expectations
     * 4. Apply the saved state onto the loaded scene
     * 
     * @param camera The main camera
     * @param filepath Path to the state file
     * @param sceneLoaderCallback Callback to load a scene by file path
     * @return Loaded scene graph with state applied, or nullptr on failure
     */
    std::shared_ptr<SceneGraph> LoadStateWithSceneValidation(
        const std::shared_ptr<Camera>& camera,
        const std::string& filepath,
        std::function<std::shared_ptr<SceneGraph>(const std::string&)> sceneLoaderCallback);

    /**
     * @brief Quick save to default location
     */
    bool QuickSave(const std::shared_ptr<SceneGraph>& sceneGraph,
                   const std::shared_ptr<Camera>& camera);

    /**
     * @brief Quick load from default location
     */
    bool QuickLoad(const std::shared_ptr<SceneGraph>& sceneGraph,
                   const std::shared_ptr<Camera>& camera);

    // Configuration
    void SetQuickSavePath(const std::string& path) { m_quickSavePath = path; }
    const std::string& GetQuickSavePath() const { return m_quickSavePath; }

private:
    // Serialization helpers
    nlohmann::json SerializeCamera(const std::shared_ptr<Camera>& camera);
    nlohmann::json SerializeSkybox(const std::shared_ptr<SceneGraph>& sceneGraph);
    nlohmann::json SerializeEnvironment(const std::shared_ptr<SceneGraph>& sceneGraph);
    nlohmann::json SerializeNodeRecursive(const std::shared_ptr<SceneNode>& node,
                                          const std::shared_ptr<SceneGraph>& sceneGraph);
    nlohmann::json SerializeNodeTransform(const std::shared_ptr<SceneNode>& node);
    nlohmann::json SerializeNodeRuntime(const std::shared_ptr<SceneNode>& node,
                                        const std::shared_ptr<SceneGraph>& sceneGraph);

    // Deserialization helpers
    bool RestoreCamera(const nlohmann::json& cameraJson,
                       const std::shared_ptr<Camera>& camera);
    bool RestoreSkybox(const nlohmann::json& skyboxJson,
                       const std::shared_ptr<SceneGraph>& sceneGraph);
    bool RestoreEnvironment(const nlohmann::json& envJson,
                            const std::shared_ptr<SceneGraph>& sceneGraph);
    void RestoreNodesRecursive(const nlohmann::json& nodesJson,
                               const std::vector<std::shared_ptr<SceneNode>>& nodes,
                               const std::shared_ptr<SceneGraph>& sceneGraph);
    void RestoreNodeState(const nlohmann::json& nodeJson,
                          const std::shared_ptr<SceneNode>& node,
                          const std::shared_ptr<SceneGraph>& sceneGraph);

    // Post-restoration synchronization helpers
    void SyncAllNodesFromECS(const std::shared_ptr<SceneGraph>& sceneGraph);
    void SyncPhysicsBodiesFromNodes(const std::shared_ptr<SceneGraph>& sceneGraph);

    std::string m_quickSavePath = "snapshots/quicksave.json";
    std::string m_currentSceneFilePath;
    
    // Cache for node count to avoid repeated traversals
    mutable int m_cachedNodeCount;
    mutable bool m_nodeCountValid;
    
    // Helper to count nodes for validation
    int CountSceneNodes(const std::shared_ptr<SceneGraph>& sceneGraph) const;
};
