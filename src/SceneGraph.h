#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <map>
#include <GL/glew.h>
#include "SceneNode.h"
#include "Skybox.h"
#include "MDIBatch.h"

// Forward declarations
class LightManager;

/**
 * SceneGraph holds the root node of the entire scene.
 */
class SceneGraph {
public:
    SceneGraph();
    virtual ~SceneGraph();

    std::shared_ptr<SceneNode> GetRoot();
    // const variant for read-only traversals
    std::shared_ptr<SceneNode> GetRoot() const { return m_root; }

    // Returns the scene's hierarchy as a map
    std::map<std::string, std::shared_ptr<SceneNode>> GetSceneHierarchy();

    // Forward pass draw
    void Draw(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram);

    // Shadow pass draw
    void DrawCascade(const glm::mat4& lightSpace, GLuint shadowShader);

    // Deferred geometry pass
    void DrawGeometry(GLuint geometryShader);

    // Motion vector pass for TAA
    void DrawVelocity(GLuint velocityShader);

    // MDI collection
    void CollectRenderableObjects(MDIBatch& batch);

    // Find all nodes by type
    std::vector<std::shared_ptr<SceneNode>> FindNodesByType(SceneNode::NODE_TYPE type);
    
    // NEW: Find first LPV volume node in scene
    std::shared_ptr<SceneNode> FindLPVVolumeNode();
    
    // NEW: Save scene to file
    bool SaveToFile(const std::string& filePath);

    void SetSkybox(const std::shared_ptr<Skybox>& skybox);

    std::shared_ptr<Skybox> GetSkybox();

    // Multi-light system integration
    void SetLightManager(std::shared_ptr<LightManager> lightManager);
    std::shared_ptr<LightManager> GetLightManager() const;
    void UpdateLightManager(); // Collect lights from scene and update LightManager

    // Node searching
    std::shared_ptr<SceneNode> FindNodeByModelName(const std::string& modelName);

    void SetSceneName(const std::string& sceneName);
    std::string GetSceneName() const { return m_sceneName; }
    void PrintMembers();

    void Shutdown();

    bool IsActive() const { return m_active; }
    bool IsSwappingScenes() const { return m_swapping_scenes; }
    void SetSwappingScenes(bool swapping) { m_swapping_scenes = swapping; }

    // Post-process overrides
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Per-scene physics toggle
    void SetPhysicsEnabled(bool enabled) { m_physicsEnabled = enabled; }
    bool IsPhysicsEnabled() const { return m_physicsEnabled; }

private:
    std::shared_ptr<SceneNode> FindNodeByModelNameRecursive(
        const std::shared_ptr<SceneNode>& node,
        const std::string& modelName
    );
    std::shared_ptr<Skybox> m_skybox;
    std::shared_ptr<LightManager> m_lightManager; // Multi-light system

    std::shared_ptr<SceneNode> m_root;
    std::string m_sceneName;
    bool m_active = false;
    bool m_swapping_scenes = false;
    bool m_physicsEnabled = true; // new flag
};
