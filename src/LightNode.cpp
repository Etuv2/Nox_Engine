#include "LightNode.h"
#include "DirectionalLight.h"
#include "PointLight.h"
#include "SpotLight.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <iostream>

LightNode::LightNode(std::shared_ptr<BaseLight> light)
    : SceneNode(), m_light(light)
{
    if (m_light) {
        // Initialize node transform from light properties
        UpdateTransformFromLight();
        UpdateSelectionProxy();
        
        std::cout << "[LightNode] Created light node for " 
                  << static_cast<int>(m_light->GetLightType()) << " light" << std::endl;
    }
}

void LightNode::SetLight(std::shared_ptr<BaseLight> light) 
{ 
    m_light = light; 
    if (m_light) {
        UpdateTransformFromLight();
        UpdateSelectionProxy();
        m_lightDirty = false;
        m_transformDirty = true;
    }
}

void LightNode::UpdateTransform(const glm::mat4& parentTransform) 
{
    // FIXED: Use the unified hierarchy system with proper parent context
    // The parent transform is passed down during scene graph traversal
    // We need to ensure our light properties sync with the calculated world transform
    
    // Calculate our world transform using the parent context
    glm::mat4 worldTransform = GetGlobalTransform(parentTransform);
    
    // Sync light properties if dirty or if world transform changed
    if (m_light && (m_lightDirty || m_transformDirty)) {
        // Extract world position from calculated transform
        glm::vec3 worldPosition = glm::vec3(worldTransform[3]);
        m_light->SetPosition(worldPosition);
        
        // For directional and spot lights, update direction from world rotation
        if (m_light->GetLightType() == BaseLight::LightType::DIRECTIONAL ||
            m_light->GetLightType() == BaseLight::LightType::SPOT) {
            
            // Extract rotation from our local transform
            glm::vec3 scale, translation, skew;
            glm::quat rotation;
            glm::vec4 perspective;
            glm::decompose(transform, scale, rotation, translation, skew, perspective);
            
            // Calculate direction in world space
            glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
            glm::mat3 rotMat = glm::mat3_cast(rotation);
            glm::vec3 worldDirection = rotMat * forward;
            m_light->SetDirection(glm::normalize(worldDirection));
        }
        
        // Update selection proxy to reflect changes
        UpdateSelectionProxy();
    }
    
    // Clear dirty flags since we just synchronized
    m_transformDirty = false;
    m_lightDirty = false;
}

// FIXED: Override UpdateTransformSystems to sync light from world transform during traversal
void LightNode::UpdateTransformSystems(const glm::mat4& worldTransform) {
    if (!m_light) return;
    
    // Extract world position from the provided world transform
    glm::vec3 worldPosition = glm::vec3(worldTransform[3]);
    m_light->SetPosition(worldPosition);
    
    // For directional and spot lights, extract and apply rotation
    if (m_light->GetLightType() == BaseLight::LightType::DIRECTIONAL ||
        m_light->GetLightType() == BaseLight::LightType::SPOT) {
        
        // Extract rotation from world transform
        glm::vec3 scale, translation, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);
        
        // Calculate direction in world space
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::mat3 rotMat = glm::mat3_cast(rotation);
        glm::vec3 worldDirection = rotMat * forward;
        m_light->SetDirection(glm::normalize(worldDirection));
    }
    
    // Update selection proxy to reflect changes
    UpdateSelectionProxy();
    
    // Clear dirty flags
    m_transformDirty = false;
    m_lightDirty = false;
}

void LightNode::SetPosition(const glm::vec3& position) 
{
    // Update our local transform matrix
    transform[3][0] = position.x;
    transform[3][1] = position.y;
    transform[3][2] = position.z;
    
    // Mark as needing light sync
    m_lightDirty = true;
    
    if (m_light) {
        // Update light position immediately
        m_light->SetPosition(position);
        
        // Update selection proxy to reflect new position
        UpdateSelectionProxy();
        
        std::cout << "[LightNode] Position set to (" 
                  << position.x << "," << position.y << "," << position.z << ")" << std::endl;
    }
}

void LightNode::SetRotation(const glm::vec3& axis, float angle) 
{
    // Apply rotation to transform matrix
    glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), angle, axis);
    transform = transform * rotation;
    
    // Mark as needing sync
    m_lightDirty = true;
    
    // Update light immediately
    UpdateLightFromTransform(true);
    
    // Update selection proxy to reflect rotation
    UpdateSelectionProxy();
}

void LightNode::SetScale(const glm::vec3& scale) 
{
    // Apply scale to transform matrix
    glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);
    transform = transform * scaleMatrix;
    
    // Scale could affect light range for point/spot lights
    if (m_light && (m_light->GetLightType() == BaseLight::LightType::POINT ||
                   m_light->GetLightType() == BaseLight::LightType::SPOT)) {
        float avgScale = (scale.x + scale.y + scale.z) / 3.0f;
        m_light->SetRange(m_light->GetRange() * avgScale);
        UpdateSelectionProxy();
    }
}

// Helper to get position from transform matrix
glm::vec3 LightNode::GetPosition() const 
{
    return glm::vec3(transform[3]);
}

void LightNode::UpdateLightFromTransform(bool forceUpdate) 
{
    if (!m_light) return;
    
    // Check if we should update based on dirty flag or force parameter
    if (!forceUpdate && !m_lightDirty) return;
    
    // FIXED: Get world position using the unified hierarchy system
    glm::vec3 worldPosition = GetWorldPosition();
    
    // Extract rotation from our local transform for direction calculation
    glm::vec3 scale, translation, skew;
    glm::quat rotation;
    glm::vec4 perspective;
    glm::decompose(transform, scale, rotation, translation, skew, perspective);
    
    // Update light position with world position
    m_light->SetPosition(worldPosition);
    
    // For directional and spot lights, update direction based on rotation
    if (m_light->GetLightType() == BaseLight::LightType::DIRECTIONAL ||
        m_light->GetLightType() == BaseLight::LightType::SPOT) {
        
        // Default forward direction is negative Z
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::mat3 rotMat = glm::mat3_cast(rotation);
        glm::vec3 worldDirection = rotMat * forward;
        m_light->SetDirection(glm::normalize(worldDirection));
        
        std::cout << "[LightNode] Updated light direction to (" 
                  << worldDirection.x << "," << worldDirection.y << "," << worldDirection.z << ")" << std::endl;
    }
    
    m_lightDirty = false;
    
    // Debug output for position updates
    std::cout << "[LightNode] Updated light from transform: pos(" 
              << worldPosition.x << "," << worldPosition.y << "," << worldPosition.z << ")" << std::endl;
}

// FIXED: Add unified method to get world position using hierarchy system
glm::vec3 LightNode::GetWorldPosition() const
{
    // Get parent's world transform if we have a parent
    glm::mat4 parentWorld(1.0f);
    if (auto parent = parentNode.lock()) {
        parentWorld = parent->GetGlobalTransform(glm::mat4(1.0f));
    }
    
    // Get our global transform using the proper hierarchy method
    glm::mat4 worldTransform = GetGlobalTransform(parentWorld);
    
    return glm::vec3(worldTransform[3]);
}

void LightNode::UpdateTransformFromLight() 
{
    if (!m_light) return;
    
    // Update node position from light (this sets local transform)
    SetPosition(m_light->GetPosition());
    
    // For directional and spot lights, update rotation from direction
    if (m_light->GetLightType() == BaseLight::LightType::DIRECTIONAL ||
        m_light->GetLightType() == BaseLight::LightType::SPOT) {
        
        glm::vec3 lightDir = glm::normalize(m_light->GetDirection());
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
        
        // Calculate rotation from forward to light direction
        glm::vec3 axis = glm::cross(forward, lightDir);
        float angle = acos(glm::clamp(glm::dot(forward, lightDir), -1.0f, 1.0f));
        
        if (glm::length(axis) > 0.001f) {
            SetRotation(glm::normalize(axis), angle);
        }
    }
    
    m_transformDirty = false;
}

BoundingBox LightNode::GetBoundingBox() const 
{
    // Always return a valid bounding box for light selection
    if (!m_light) {
        // Return a minimal default bounding box if no light is attached
        return BoundingBox(glm::vec3(-0.5f), glm::vec3(0.5f));
    }
    
    // Ensure selection proxy is up to date
    const_cast<LightNode*>(this)->UpdateSelectionProxy();
    
    if (!m_selectionProxy.isValid) {
        // Fallback: create a basic bounding box from light position
        glm::vec3 lightPos = m_light->GetPosition();
        float defaultRadius = 1.0f;
        
        // Adjust radius based on light type
        switch (m_light->GetLightType()) {
            case BaseLight::LightType::DIRECTIONAL:
                defaultRadius = 2.0f; // Larger handle for directional lights
                break;
            case BaseLight::LightType::POINT:
                defaultRadius = glm::max(0.5f, m_light->GetRange() * 0.1f);
                break;
            case BaseLight::LightType::SPOT:
                defaultRadius = glm::max(0.8f, m_light->GetRange() * 0.15f);
                break;
            default:
                defaultRadius = 1.0f;
                break;
        }
        
        return BoundingBox(lightPos, defaultRadius);
    }
    
    return m_selectionProxy.GetBoundingBox();
}

bool LightNode::IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float& t) const 
{
    if (!m_light) return false;
    
    // Ensure selection proxy is up to date
    const_cast<LightNode*>(this)->UpdateSelectionProxy();
    
    if (!m_selectionProxy.isValid) {
        // Fallback ray-sphere intersection with light position
        glm::vec3 lightPos = m_light->GetPosition();
        float radius = 1.0f;
        
        // Adjust radius based on light type
        switch (m_light->GetLightType()) {
            case BaseLight::LightType::DIRECTIONAL:
                radius = 2.0f;
                break;
            case BaseLight::LightType::POINT:
                radius = glm::max(0.5f, m_light->GetRange() * 0.1f);
                break;
            case BaseLight::LightType::SPOT:
                radius = glm::max(0.8f, m_light->GetRange() * 0.15f);
                break;
            default:
                radius = 1.0f;
                break;
        }
        
        // Simple sphere-ray intersection
        glm::vec3 toCenter = lightPos - rayOrigin;
        float projLength = glm::dot(toCenter, rayDirection);
        
        if (projLength < 0.0f) return false; // Behind ray origin
        
        glm::vec3 closestPoint = rayOrigin + rayDirection * projLength;
        float distToCenter = glm::length(closestPoint - lightPos);
        
        if (distToCenter <= radius) {
            t = projLength - sqrt(radius * radius - distToCenter * distToCenter);
            return t >= 0.0f;
        }
        
        return false;
    }
    
    return m_selectionProxy.IntersectRay(rayOrigin, rayDirection, t);
}

void LightNode::UpdateSelectionProxy() 
{
    m_selectionProxy.UpdateFromLight(m_light);
}

void LightNode::Draw(const glm::mat4& parentTransform, const glm::mat4& view, 
                     const glm::mat4& projection, unsigned int defaultShaderProgram) 
{
    // Update transforms first
    UpdateTransform(parentTransform);
    
    // Light nodes don't render actual geometry by default
    // But we can add debug visualization if enabled
    if (m_showDebugVisualization || m_isSelected) {
        DrawDebugVisualization(view, projection);
    }
    
    // Note: Child rendering is now handled by RenderSystem
    // LightNode children with models will be rendered via the ECS pipeline
}

void LightNode::DrawDebugVisualization(const glm::mat4& view, const glm::mat4& projection) 
{
    if (!m_light || !m_selectionProxy.isValid) return;
    
    // TODO: Implement actual debug visualization rendering
    // For now, just log occasionally that we're drawing debug vis
    static int debugCounter = 0;
    if (debugCounter++ % 300 == 0) { // Log every ~5 seconds at 60fps
        std::cout << "[LightNode] Debug visualization for " 
                  << static_cast<int>(m_light->GetLightType()) << " light at (" 
                  << m_selectionProxy.center.x << "," << m_selectionProxy.center.y << "," 
                  << m_selectionProxy.center.z << ") radius=" << m_selectionProxy.radius
                  << (m_isSelected ? " [SELECTED]" : "") << std::endl;
    }
}

void LightNode::Serialize(nlohmann::json& json) const 
{
    // Manually serialize basic SceneNode properties
    glm::vec3 pos = GetPosition();
    
    json["position"] = { pos.x, pos.y, pos.z };
    
    // Override type to ensure it's marked as light
    json["type"] = "light";
    json["nodeType"] = "LIGHT";
    
    if (m_light) {
        nlohmann::json lightJson;
        
        // Manual light serialization
        lightJson["type"] = static_cast<int>(m_light->GetLightType());
        lightJson["color"] = { m_light->GetColor().x, m_light->GetColor().y, m_light->GetColor().z };
        lightJson["intensity"] = m_light->GetIntensity();
        lightJson["enabled"] = m_light->IsEnabled();
        lightJson["position"] = { m_light->GetPosition().x, m_light->GetPosition().y, m_light->GetPosition().z };
        lightJson["direction"] = { m_light->GetDirection().x, m_light->GetDirection().y, m_light->GetDirection().z };
        json["light"] = lightJson;
    }
    
    // Add visualization flags
    json["showDebugVisualization"] = m_showDebugVisualization;
    json["isSelected"] = m_isSelected;
}

void LightNode::Deserialize(const nlohmann::json& json) 
{
    // Manually deserialize basic SceneNode properties
    if (json.contains("position") && json["position"].is_array() && json["position"].size() == 3) {
        SetPosition(glm::vec3(json["position"][0], json["position"][1], json["position"][2]));
    }
    
    if (json.contains("light") && json["light"].is_object()) {
        const auto& lightJson = json["light"];
        
        // Create appropriate light type based on serialized data
        if (lightJson.contains("type")) {
            int lightTypeInt = lightJson["type"];
            BaseLight::LightType lightType = static_cast<BaseLight::LightType>(lightTypeInt);
            
            switch (lightType) {
                case BaseLight::LightType::DIRECTIONAL:
                    m_light = std::make_shared<DirectionalLight>();
                    break;
                case BaseLight::LightType::POINT:
                    m_light = std::make_shared<PointLight>();
                    break;
                case BaseLight::LightType::SPOT:
                    m_light = std::make_shared<SpotLight>();
                    break;
                default:
                    std::cerr << "[LightNode] Unknown light type: " << lightTypeInt << std::endl;
                    return;
            }
            
            if (m_light) {
                // Manual light deserialization
                if (lightJson.contains("color") && lightJson["color"].is_array() && lightJson["color"].size() == 3) {
                    m_light->SetColor(glm::vec3(lightJson["color"][0], lightJson["color"][1], lightJson["color"][2]));
                }
                if (lightJson.contains("intensity")) {
                    m_light->SetIntensity(lightJson["intensity"]);
                }
                if (lightJson.contains("enabled")) {
                    m_light->SetEnabled(lightJson["enabled"]);
                }
                if (lightJson.contains("position") && lightJson["position"].is_array() && lightJson["position"].size() == 3) {
                    m_light->SetPosition(glm::vec3(lightJson["position"][0], lightJson["position"][1], lightJson["position"][2]));
                }
                if (lightJson.contains("direction") && lightJson["direction"].is_array() && lightJson["direction"].size() == 3) {
                    m_light->SetDirection(glm::vec3(lightJson["direction"][0], lightJson["direction"][1], lightJson["direction"][2]));
                }
                
                UpdateTransformFromLight();
                UpdateSelectionProxy();
            }
        }
    }
    
    // Load visualization flags
    if (json.contains("showDebugVisualization")) {
        m_showDebugVisualization = json["showDebugVisualization"];
    }
    if (json.contains("isSelected")) {
        m_isSelected = json["isSelected"];
    }
}

void LightNode::SetTransform(const glm::mat4& newTransform) 
{
    // Override SetTransform to ensure light-node synchronization
    
    // Store the old transform for comparison
    glm::mat4 oldTransform = transform;
    
    // Update the base transform
    transform = newTransform;
    m_transformDirty = true;
    m_lightDirty = true; // Mark light as dirty to trigger sync
    
    // Force immediate light update when gizmo manipulates the node
    if (m_light) {
        // Extract position from the new transform matrix
        glm::vec3 newPosition = glm::vec3(newTransform[3]);
        
        // Update light position immediately
        m_light->SetPosition(newPosition);
        
        // For directional and spot lights, extract rotation and update direction
        if (m_light->GetLightType() == BaseLight::LightType::DIRECTIONAL ||
            m_light->GetLightType() == BaseLight::LightType::SPOT) {
            
            // Extract rotation from transform
            glm::vec3 scale, translation, skew;
            glm::quat rotation;
            glm::vec4 perspective;
            glm::decompose(newTransform, scale, rotation, translation, skew, perspective);
            
            // Calculate new direction based on rotation
            glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f); // Default forward direction
            glm::mat3 rotMat = glm::mat3_cast(rotation);
            glm::vec3 newDirection = rotMat * forward;
            m_light->SetDirection(glm::normalize(newDirection));
        }
        
        // Force an immediate UpdateLightFromTransform call
        UpdateLightFromTransform(true); // Force update
        
        // Update selection proxy to reflect new position
        UpdateSelectionProxy();
    }
    
    // Mark transform as clean since we just updated everything
    m_transformDirty = false;
    m_lightDirty = false;
}