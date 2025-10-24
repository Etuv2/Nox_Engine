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
    // Recursively process a node and its children.
    std::shared_ptr<class SceneNode> ProcessNodeRecursive(const nlohmann::json& nodeJson);

    // Processes a single node (without processing children).
    std::shared_ptr<class SceneNode> ProcessNode(const nlohmann::json& nodeJson);
    // Processes a collider and attaches it to the node.
    void ProcessCollider(const nlohmann::json& colliderJson, std::shared_ptr<SceneNode> node);
    // Processes GUI element properties like resizing, anchoring, and relative positioning
    void ProcessGuiElementProperties(std::shared_ptr<GuiNode> guiNode, int elementId, const nlohmann::json& elementJson);
};
