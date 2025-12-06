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
#include <fstream>
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using json = nlohmann::json;

SceneLoader::SceneLoader(std::shared_ptr<ModelManager> modelManager, std::shared_ptr<PhysicsEngine> physicsEngine, int screenW, int screenH):
    m_modelManager(modelManager),
    m_physicsEngine(physicsEngine),
    m_screenH(screenH),
    m_screenW(screenW)
{
}

SceneLoader::~SceneLoader() {
}

std::shared_ptr<SceneGraph> SceneLoader::LoadScene(const std::string& sceneFilePath) {
    auto sceneGraph = std::make_shared<SceneGraph>();
    
    // CRITICAL: Set the current scene graph BEFORE processing nodes
    // This is required for CreateECSEntity to work properly
    m_currentSceneGraph = sceneGraph.get();
    
    // Report loading started
    GuiEventBus::GetInstance().PublishLoadingStarted();
    GuiEventBus::GetInstance().PublishProgress(0.0f, "Assets");

    std::ifstream sceneFile(sceneFilePath);
    if (!sceneFile.is_open()) {
        std::cerr << "Failed to open scene file: " << sceneFilePath << std::endl;
        GuiEventBus::GetInstance().PublishError("Failed to open scene file: " + sceneFilePath);
        m_currentSceneGraph = nullptr;
        return sceneGraph;
    }

    json sceneJson;
    sceneFile >> sceneJson;

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
            // Create ECS entity for root if needed
            if (root->GetEntityID() == INVALID_ENTITY) {
                root->CreateECSEntity("Root");
            }
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
        std::cerr << "Invalid scene file format. 'nodes' array is missing." << std::endl;
    }

    // Initialize the skybox if provided.
    if (sceneJson.contains("skybox")) {
        std::string hdrPath = sceneJson.value("skybox", "hdrs//skybox.hdr");

        auto sb = std::make_shared<Skybox>();
        if (!sb->Init(hdrPath,
            "shaders/equirect2cube_vert.glsl",
            "shaders/equirect2cube_frag.glsl",
            "shaders/skybox_vert.glsl",
            "shaders/skybox_frag.glsl",
            m_screenW,
            m_screenH)) {
            std::cerr << "[SceneLoader] Failed to load skybox: " << hdrPath << "\n";
        }
        else {
            std::cout << "[SceneLoader] Loading skybox: " << hdrPath << std::endl;
            sceneGraph->SetSkybox(sb);
        }
    }

    // Per-scene physics flag (default true)
    bool physicsEnabled = sceneJson.value("physics_enabled", true);
    sceneGraph->SetPhysicsEnabled(physicsEnabled);

    // Clear the scene graph reference
    m_currentSceneGraph = nullptr;
    
    // Log ECS statistics
    std::cout << "[SceneLoader] Scene loaded successfully. ECS entities created: " 
              << sceneGraph->GetComponentManager()->GetTransformPool().Size() << std::endl;

    return sceneGraph;
}

// NEW: Save scene state back to JSON file
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
        
    } catch (const std::exception& e) {
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
        default:
            nodeJson["type"] = "node";
            break;
    }
    
    // Serialize transform
    glm::vec3 position = node->GetPosition();
    glm::vec3 rotation = node->GetRotation();
    glm::vec3 scale = node->GetScale();
    
    nodeJson["position"] = { position.x, position.y, position.z };
    
    // FIXED: Convert rotation from radians to degrees for saving
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
            // FIXED: Serialize model path using ModelManager
            auto model = node->GetModel();
            if (model && m_modelManager) {
                std::string modelPath = m_modelManager->GetModelPath(model.get());
                if (!modelPath.empty()) {
                    nodeJson["path"] = modelPath;
                } else {
                    std::cout << "[SceneLoader] Warning: Could not find model path for node '" 
                              << nodeName << "'" << std::endl;
                }
            }
            
            // Serialize collider if present
            if (node->GetRigidBody()) {
                nodeJson["collider"] = SerializeCollider(node);
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
                // FIXED: Serialize audio properties
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
            
            glm::vec3 velocity = rb->getVelocity();
            colliderJson["velocity_x"] = velocity.x;
            colliderJson["velocity_y"] = velocity.y;
            colliderJson["velocity_z"] = velocity.z;
            
            glm::vec3 acceleration = rb->getAcceleration();
            bool hasGravity = (glm::length(acceleration) > 0.001f);
            colliderJson["gravity"] = hasGravity;
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
            
            glm::vec3 velocity = rb->getVelocity();
            colliderJson["velocity_x"] = velocity.x;
            colliderJson["velocity_y"] = velocity.y;
            colliderJson["velocity_z"] = velocity.z;
            
            glm::vec3 acceleration = rb->getAcceleration();
            bool hasGravity = (glm::length(acceleration) > 0.001f);
            colliderJson["gravity"] = hasGravity;
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
        bool  useGravity = colliderJson.value("gravity", true);
        float linearDamping = colliderJson.value("linear_damping", 0.1f);
        float angularDamping = colliderJson.value("angular_damping", 0.1f);
        float friction = colliderJson.value("friction", 0.5f);

        auto rb = std::make_shared<RigidBody>();
        rb->setShape(RigidBody::ShapeType::SPHERE);
        rb->AttachNode(node);
        rb->setPosition(node->GetPosition());
        rb->setMass(mass);
        rb->setLinearDamping(linearDamping);
        rb->setAngularDamping(angularDamping);
        rb->setFriction(friction);
        rb->setBoundingRadius(radius);

        glm::vec3 initVel{
            colliderJson.value("velocity_x", 0.0f),
            colliderJson.value("velocity_y", 0.0f),
            colliderJson.value("velocity_z", 0.0f)
        };
        rb->setVelocity(initVel);

        // Gravity will be applied per-frame by PhysicsEngine
        // Initialize acceleration to zero (gravity applied in step())
          rb->setAcceleration(glm::vec3(0.0f));

        rb->computeInertiaTensor();
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
        rb->setPosition(node->GetPosition()); // Set position above the plane
        rb->setPlane(normal, height);


   
        rb->setBoundingRadius(0.0f); // not used for collision
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

        auto rb = std::make_shared<RigidBody>();
        rb->setShape(RigidBody::ShapeType::BOX);
        rb->AttachNode(node);
        rb->setPosition(node->GetPosition());
        rb->setOrientation(node->GetOrientation());
        rb->setMass(mass);
        rb->setLinearDamping(linearDamping);
        rb->setAngularDamping(angularDamping);
        rb->setFriction(friction);
        rb->setBox(size);

        glm::vec3 initVel{
            colliderJson.value("velocity_x", 0.0f),
            colliderJson.value("velocity_y", 0.0f),
            colliderJson.value("velocity_z", 0.0f)
        };
        rb->setVelocity(initVel);

        // Gravity will be applied per-frame by PhysicsEngine
        // Initialize acceleration to zero (gravity applied in step())
          rb->setAcceleration(glm::vec3(0.0f));

        rb->computeInertiaTensor();
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
        } else if (anchorStr == "top_right") {
            anchor = GuiNode::GuiElement::Anchor::TOP_RIGHT;
        } else if (anchorStr == "bottom_left") {
            anchor = GuiNode::GuiElement::Anchor::BOTTOM_LEFT;
        } else if (anchorStr == "bottom_right") {
            anchor = GuiNode::GuiElement::Anchor::BOTTOM_RIGHT;
        } else if (anchorStr == "center") {
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
    
    // If the node has children, process them recursively with this node as parent
    if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
        EntityID myEntityID = node->GetEntityID();
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
            node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
            break;
        case NodeType::LIGHT:
            node = std::make_shared<LightNode>(nullptr); // Will be set based on light properties
            node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
            break;
        case NodeType::LPV_VOLUME:  // NEW: Handle LPV volume nodes
            node = std::make_shared<SceneNode>();
            node->SetNodeType(SceneNode::LPV_VOLUME);
            std::cout << "[SceneLoader] Creating LPV Volume node" << std::endl;
            break;
        case NodeType::MODEL:
            node = std::make_shared<SceneNode>();
            node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
            break;
        default:
            node = std::make_shared<SceneNode>();
            break;
        }

    // Process common transform data.
    glm::vec3 pos(0.0f), rot(0.0f), scl(1.0f);
    if (nodeJson.contains("position") && nodeJson["position"].is_array() && nodeJson["position"].size() == 3)
        pos = glm::vec3(nodeJson["position"][0], nodeJson["position"][1], nodeJson["position"][2]);
    if (nodeJson.contains("rotation") && nodeJson["rotation"].is_array() && nodeJson["rotation"].size() == 3)
        rot = glm::vec3(nodeJson["rotation"][0], nodeJson["rotation"][1], nodeJson["rotation"][2]);
    if (nodeJson.contains("scale") && nodeJson["scale"].is_array() && nodeJson["scale"].size() == 3)
        scl = glm::vec3(nodeJson["scale"][0], nodeJson["scale"][1], nodeJson["scale"][2]);
    
    // FIXED: Apply rotation correctly using combined Euler angles -> quaternion conversion
    // Previous code called SetRotation three times, but each call replaced the rotation instead of accumulating
    glm::vec3 radians = glm::radians(rot);
    glm::quat rotationQuat = glm::quat(radians); // glm::quat from Euler angles (pitch, yaw, roll)
    
    // Apply transform using SetLocalTRS which correctly combines translation, rotation, scale
    node->SetLocalTRS(pos, rotationQuat, scl);

    // Set node name if provided
    if (nodeJson.contains("name")) {
        // Note: SceneNode doesn't have a SetName method, but we can store it for LightManager
        // The name will be used when registering with LightManager
    }

    // Process type-specific properties.
    switch (type) {
    case NodeType::MODEL: { // ModelNode
        std::string modelPath = nodeJson.value("path", "");
        if (!modelPath.empty()) {
            auto model = m_modelManager->LoadModel(modelPath); // Load the model using the ModelManager
            if (model) {
                node->SetModel(model); // Set the model to the node
                if (model->hasSkin)
                    node->BuildSkeleton(*model); 
            }
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
        if (nodeJson.contains("collider")) {
            ProcessCollider(nodeJson["collider"], node); // Process the collider for physics
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
                } else {
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
            } else {
                std::cerr << "[SceneLoader] Failed to create light of type: " << lightType << std::endl;
            }
        } else {
            std::cerr << "[SceneLoader] Light node missing light properties" << std::endl;
        }
        break;
    }
    case NodeType::GUI: {
        auto guiNode = std::make_shared<GuiNode>(m_screenW, m_screenH); // Create a GUI node,this acts as a container for GUI elements in the scene
        
        // Load font if specified
        std::string fontPath = nodeJson.value("font_path", "fonts\\arial.ttf");
        int fontSize = nodeJson.value("font_size", 24);
        guiNode->LoadFont(fontPath, fontSize);
        
        if (nodeJson.contains("elements")) { // Check for GUI elements
            for (const auto& el : nodeJson["elements"]) { // Loop through each element
                std::string kind = el.value("kind", "text");
                
                if (kind == "text") {
                    SDL_Color textColor = {255, 255, 255, 255}; // Default white
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
                    SDL_Color rectColor = {128, 128, 128, 255}; // Default gray
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
                    } else if (gradientType == "vertical") {
                        gradient.type = GuiNode::GradientType::LINEAR_VERTICAL;
                    } else if (gradientType == "radial") {
                        gradient.type = GuiNode::GradientType::RADIAL;
                    }
                    
                    // Set start color
                    gradient.startColor = {255, 255, 255, 255}; // Default white
                    if (el.contains("start_color") && el["start_color"].is_array() && el["start_color"].size() >= 3) {
                        gradient.startColor.r = static_cast<Uint8>(el["start_color"][0]);
                        gradient.startColor.g = static_cast<Uint8>(el["start_color"][1]);
                        gradient.startColor.b = static_cast<Uint8>(el["start_color"][2]);
                        gradient.startColor.a = el["start_color"].size() > 3 ? static_cast<Uint8>(el["start_color"][3]) : 255;
                    }
                    
                    // Set end color
                    gradient.endColor = {0, 0, 0, 255}; // Default black
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
                    SDL_Color baseColor = {128, 128, 128, 255}; // Default gray
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
        break;
    }

    default:
        std::cerr << "Unknown node type: " << typeStr << std::endl;
        break;
    }


    return node;
}

// ============== ECS ENTITY AND COMPONENT CREATION ==============

void SceneLoader::CreateECSEntity(std::shared_ptr<SceneNode> node, EntityID parentID) {
    if (!node || !m_currentSceneGraph) return;
    
    ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
    if (!componentManager) return;
    
    // Skip if entity already exists
    if (node->GetEntityID() != INVALID_ENTITY) {
        // Just update parent relationship
        TransformComponent* transform = node->GetTransformComponent();
        if (transform && parentID != INVALID_ENTITY) {
            transform->parentID = parentID;
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
    }
    
    // Create entity
    std::string nodeName = node->GetName();
    EntityID entityID = componentManager->CreateEntity(nodeName, ecsType);
    node->SetEntityID(entityID);
    
    // Create TransformComponent
    TransformComponent transformComp;
    transformComp.localTransform = node->GetTransform();
    transformComp.animatedTransform = node->GetAnimatedTransform();
    transformComp.worldTransform = glm::mat4(1.0f);
    transformComp.isDirty = true;
    transformComp.parentID = parentID;
    
    componentManager->AddTransform(entityID, transformComp);
    
    // Create RenderableComponent if node has a model
    if (node->GetModel()) {
        CreateRenderableComponent(node, node->GetModel());
    }
    
    // Create AnimationComponent if model has animations
    if (node->GetModel() && !node->GetModel()->animations.empty()) {
        CreateAnimationComponent(node, node->GetModel());
    }
    
    std::cout << "[SceneLoader] Created ECS entity " << entityID << " for node: " << nodeName 
              << " (parent=" << parentID << ")" << std::endl;
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
    renderComp.hasAlpha = false;  // Will be determined by mesh materials
    renderComp.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(node->GetCullingOverride()));
    renderComp.boneNodes = node->boneNodes;
    renderComp.boneInverseBindMatrices = node->boneInverseBindMatrices;
    renderComp.nodeIndex = node->nodeIndex;
    
    componentManager->AddRenderable(entityID, renderComp);
    
    std::cout << "[SceneLoader] Created RenderableComponent for entity " << entityID 
              << " with " << model->meshes.size() << " meshes" << std::endl;
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
    
    std::cout << "[SceneLoader] Created AnimationComponent for entity " << entityID 
              << " with " << model->animations.size() << " animations available" << std::endl;
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