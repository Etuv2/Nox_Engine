#pragma once
#include <string>
#include <memory>
#include "SceneGraph.h"
#include "ModelManager.h"
#include "PhysicsEngine.h"
#include "json.hpp"

class GuiNode; // Forward declaration

class SceneLoader {
public:
    SceneLoader(std::shared_ptr<ModelManager> modelManager, std::shared_ptr<PhysicsEngine> physicsEngine,int screenW, int screenH);
    ~SceneLoader();

    // Loads the scene from a JSON file and returns a shared pointer to a SceneGraph.
    std::shared_ptr<SceneGraph> LoadScene(const std::string& sceneFilePath);
    
    // NEW: Saves the current scene state back to a JSON file
    bool SaveScene(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sceneFilePath);


    //supporting functions & variables
    enum class NodeType {
        NODE,
        AUDIO,
        MODEL,
        LIGHT,
        CAMERA,
        GUI,
        LPV_VOLUME  // NEW: Light Propagation Volume
    };

    static NodeType GetNodeType(const std::string& typeStr) {
        if (typeStr == "model")  return NodeType::MODEL;
        if (typeStr == "light")  return NodeType::LIGHT;
        if (typeStr == "audio")  return NodeType::AUDIO;
        if (typeStr == "camera") return NodeType::CAMERA;
        if (typeStr == "gui") return NodeType::GUI;
        if (typeStr == "lpv_volume" || typeStr == "lpvvolume") return NodeType::LPV_VOLUME;  // NEW

        return NodeType::NODE;
    }

private:
    std::shared_ptr<ModelManager> m_modelManager;
    std::shared_ptr<PhysicsEngine> m_physicsEngine;
    int m_screenW, m_screenH;
    
    // Loading methods
    std::shared_ptr<class SceneNode> ProcessNodeRecursive(const nlohmann::json& nodeJson);
    std::shared_ptr<class SceneNode> ProcessNode(const nlohmann::json& nodeJson);
    void ProcessCollider(const nlohmann::json& colliderJson, std::shared_ptr<SceneNode> node);
    void ProcessGuiElementProperties(std::shared_ptr<GuiNode> guiNode, int elementId, const nlohmann::json& elementJson);
    
    // NEW: Saving methods
    nlohmann::json SerializeNodeRecursive(const std::shared_ptr<SceneNode>& node);
    nlohmann::json SerializeNode(const std::shared_ptr<SceneNode>& node);
    nlohmann::json SerializeCollider(const std::shared_ptr<SceneNode>& node);
    nlohmann::json SerializeGuiElements(const std::shared_ptr<GuiNode>& guiNode);
    nlohmann::json SerializeLightProperties(const std::shared_ptr<class LightNode>& lightNode);
};
