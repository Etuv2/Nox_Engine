#pragma once
#include "SceneNode.h"
#include "BaseLight.h"
#include "json.hpp"
#include <memory>
#include <glm/glm.hpp>

// Enhanced BoundingBox for light selection
struct BoundingBox {
    glm::vec3 min = glm::vec3(-0.5f);
    glm::vec3 max = glm::vec3(0.5f);
    
    BoundingBox() = default;
    BoundingBox(const glm::vec3& minPoint, const glm::vec3& maxPoint) : min(minPoint), max(maxPoint) {}
    BoundingBox(const glm::vec3& center, float radius) {
        min = center - glm::vec3(radius);
        max = center + glm::vec3(radius);
    }
    
    bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    glm::vec3 GetCenter() const { return (min + max) * 0.5f; }
    glm::vec3 GetSize() const { return max - min; }
    float GetRadius() const { return glm::length(GetSize()) * 0.5f; }
    
    // Ray-box intersection for selection
    bool IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& tNear, float& tFar) const {
        glm::vec3 invRayDir = 1.0f / rayDir;
        glm::vec3 t1 = (min - rayOrigin) * invRayDir;
        glm::vec3 t2 = (max - rayOrigin) * invRayDir;
        
        glm::vec3 tMin = glm::min(t1, t2);
        glm::vec3 tMax = glm::max(t1, t2);
        
        tNear = glm::max(glm::max(tMin.x, tMin.y), tMin.z);
        tFar = glm::min(glm::min(tMax.x, tMax.y), tMax.z);
        
        return tNear <= tFar && tFar >= 0.0f;
    }
};

/**
 * Enhanced LightNode allows lights to be positioned in the scene graph hierarchy.
 * This enables lights to be transformed, parented, and managed like other scene objects.
 * Now includes proper selection support and transform synchronization.
 */
class LightNode : public SceneNode
{
public:
    LightNode(std::shared_ptr<BaseLight> light);
    virtual ~LightNode() = default;

    // Light management
    void SetLight(std::shared_ptr<BaseLight> light);
    std::shared_ptr<BaseLight> GetLight() const { return m_light; }

    // Enhanced transform system with automatic light synchronization
    void UpdateTransform(const glm::mat4& parentTransform);
    void SetPosition(const glm::vec3& position);
    void SetRotation(const glm::vec3& axis, float angle);
    void SetScale(const glm::vec3& scale);
    void SetTransform(const glm::mat4& transform); // CRITICAL: Sync with light when transform changes
    glm::vec3 GetPosition() const;

    // Scene graph integration
    void Draw(const glm::mat4& parentTransform, const glm::mat4& view, 
              const glm::mat4& projection, unsigned int defaultShaderProgram);

    // Update light properties based on node transform
    void UpdateLightFromTransform(bool forceUpdate = false);
    void UpdateTransformFromLight();

    // Node type identification (if base class supports it)
    SceneNode::NODE_TYPE GetNodeType() const { return SceneNode::NODE_TYPE::LIGHT; }

    // Enhanced selection support for editor integration
    BoundingBox GetBoundingBox() const;
    bool IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float& t) const;
    
    // Light-specific selection proxy that updates automatically
    struct LightSelectionProxy {
        glm::vec3 center = glm::vec3(0.0f);
        float radius = 1.0f;
        glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f);
        BaseLight::LightType type = BaseLight::LightType::POINT;
        bool isValid = false;
        
        // Update proxy based on light properties
        void UpdateFromLight(std::shared_ptr<BaseLight> light) {
            if (!light) {
                isValid = false;
                return;
            }
            
            center = light->GetPosition();
            direction = light->GetDirection();
            type = light->GetLightType();
            isValid = true;
            
            // Set proxy size based on light type
            switch (type) {
                case BaseLight::LightType::DIRECTIONAL:
                    radius = 2.0f; // Fixed size for directional - visualize as larger sphere
                    break;
                case BaseLight::LightType::POINT:
                    radius = glm::min(3.0f, light->GetRange() * 0.15f); // Scale with range but cap it
                    radius = glm::max(0.5f, radius); // Minimum size
                    break;
                case BaseLight::LightType::SPOT:
                    radius = glm::min(4.0f, light->GetRange() * 0.2f); // Slightly larger for spots
                    radius = glm::max(0.8f, radius); // Minimum size
                    break;
                default:
                    radius = 1.0f;
                    break;
            }
        }
        
        // Enhanced ray intersection that considers light type
        bool IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& distance) const {
            if (!isValid) return false;
            
            // For all light types, use sphere intersection as primary selection method
            glm::vec3 toCenter = center - rayOrigin;
            float projLength = glm::dot(toCenter, rayDir);
            
            if (projLength < 0.0f) return false; // Behind ray origin
            
            glm::vec3 closestPoint = rayOrigin + rayDir * projLength;
            float distToCenter = glm::length(closestPoint - center);
            
            if (distToCenter <= radius) {
                distance = projLength - sqrt(radius * radius - distToCenter * distToCenter);
                return distance >= 0.0f;
            }
            return false;
        }
        
        // Get bounding box for this proxy
        BoundingBox GetBoundingBox() const {
            if (!isValid) {
                return BoundingBox(); // Invalid bounding box
            }
            return BoundingBox(center, radius);
        }
    };
    
    // Access to selection proxy
    const LightSelectionProxy& GetSelectionProxy() const { return m_selectionProxy; }
    void UpdateSelectionProxy();
    
    // ENHANCED: Unified world position calculation using hierarchy system
    glm::vec3 GetWorldPosition() const;

    // Serialization support
    void Serialize(nlohmann::json& json) const;
    void Deserialize(const nlohmann::json& json);

    // Debug visualization
    void DrawDebugVisualization(const glm::mat4& view, const glm::mat4& projection);
    void SetDebugVisualization(bool enabled) { m_showDebugVisualization = enabled; }
    bool IsDebugVisualizationEnabled() const { return m_showDebugVisualization; }

    // Selection state for editor
    void SetSelected(bool selected) { m_isSelected = selected; }
    bool IsSelected() const { return m_isSelected; }

protected:
    std::shared_ptr<BaseLight> m_light;
    
    // Transform synchronization
    glm::mat4 m_lastWorldTransform = glm::mat4(1.0f);
    bool m_transformDirty = true;
    bool m_lightDirty = true;
    
    // Selection and visualization
    LightSelectionProxy m_selectionProxy;
    bool m_showDebugVisualization = false;
    bool m_isSelected = false;
    
    // Internal sync helpers
    void SyncLightTransform();
    void SyncNodeTransform();
};