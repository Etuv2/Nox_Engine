#include "GizmoRayCast.h"
#include "SceneNode.h"
#include "LightNode.h"  // CRITICAL: Add missing include
#include "AudioNode.h"
#include "Camera.h"
#include <iostream>
#include <algorithm>

GizmoRayCast::GizmoHitResult GizmoRayCast::QueryForGizmoSelection(
    const RayCast::Ray& ray,
    std::shared_ptr<Camera> camera,
    BVH::SceneNodeBVH* sceneBVH,
    std::shared_ptr<SceneNode> currentSelection,
    const GizmoConfig& config) {
    
    if (!camera) {
        std::cout << "[GizmoRayCast] Error: Camera is null" << std::endl;
        return GizmoHitResult();
    }
    
    std::vector<std::shared_ptr<SceneNode>> candidates;
    
    if (config.enableBVHAcceleration && sceneBVH && !sceneBVH->Empty()) {
        // ENHANCED: Use multiple BVH queries with different tolerances for precision
        candidates = sceneBVH->QueryRay(ray, GizmoNodeFilter);
        std::cout << "[GizmoRayCast] BVH returned " << candidates.size() << " gizmo-compatible candidates" << std::endl;
        
        // If too few candidates found, try with expanded ray (for very small objects)
        if (candidates.size() < 3) {
            // Create slightly thicker ray for better small object detection
            const float rayExpansion = 0.1f;
            std::vector<RayCast::Ray> expandedRays;
            
            // Generate multiple parallel rays around the main ray
            glm::vec3 perpendicular1 = glm::normalize(glm::cross(ray.direction, glm::vec3(0, 1, 0)));
            glm::vec3 perpendicular2 = glm::normalize(glm::cross(ray.direction, perpendicular1));
            
            expandedRays.push_back(RayCast::Ray(ray.origin + perpendicular1 * rayExpansion, ray.direction));
            expandedRays.push_back(RayCast::Ray(ray.origin - perpendicular1 * rayExpansion, ray.direction));
            expandedRays.push_back(RayCast::Ray(ray.origin + perpendicular2 * rayExpansion, ray.direction));
            expandedRays.push_back(RayCast::Ray(ray.origin - perpendicular2 * rayExpansion, ray.direction));
            
            for (const auto& expandedRay : expandedRays) {
                auto additionalCandidates = sceneBVH->QueryRay(expandedRay, GizmoNodeFilter);
                candidates.insert(candidates.end(), additionalCandidates.begin(), additionalCandidates.end());
            }
            
            // Remove duplicates
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            
            std::cout << "[GizmoRayCast] Expanded ray search found " << candidates.size() << " total candidates" << std::endl;
        }
    } else {
        // Fallback: collect all nodes and filter
        std::cout << "[GizmoRayCast] Using fallback traversal for gizmo selection" << std::endl;
        return GizmoHitResult();
    }
    
    if (candidates.empty()) {
        return GizmoHitResult();
    }
    
    // Separate light nodes and mesh nodes for priority processing
    std::vector<std::shared_ptr<SceneNode>> lightNodes;
    std::vector<std::shared_ptr<SceneNode>> audioNodes; // NEW
    std::vector<std::shared_ptr<SceneNode>> meshNodes;
    
    for (auto& node : candidates) {
        if (node->GetNodeType() == SceneNode::LIGHT) {
            lightNodes.push_back(node);
        } else if (node->GetNodeType() == SceneNode::AUDIO) {
            audioNodes.push_back(node);
        } else {
            meshNodes.push_back(node);
        }
    }
    std::cout << "[GizmoRayCast] Found " << lightNodes.size() << " light nodes, "
              << audioNodes.size() << " audio nodes and " << meshNodes.size() << " mesh nodes" << std::endl;
    
    // ENHANCED: Multi-pass selection for maximum precision
    struct CandidateResult {
        std::shared_ptr<SceneNode> node;
        RayCast::HitResult hit;
        float finalPriority;
        float distanceToRayAxis; // For small object bias
        bool isLightNode;
    };
    
    std::vector<CandidateResult> validCandidates;
    
    // Process light nodes first with intersection testing
    for (auto& node : lightNodes) {
        if (!IsNodeGizmoCompatible(node)) continue;
        
        // For light nodes, use their selection proxy for intersection testing
        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
            auto proxy = lightNode->GetSelectionProxy();
            float distance;
            
            if (proxy.IntersectRay(ray.origin, ray.direction, distance) && distance <= config.maxSelectionDistance) {
                RayCast::HitResult hit;
                hit.hit = true;
                hit.distance = distance;
                hit.point = ray.origin + ray.direction * distance;
                hit.node = node;
                
                CandidateResult result;
                result.node = node;
                result.hit = hit;
                result.finalPriority = CalculateSelectionPriority(node, hit, currentSelection, config);
                result.distanceToRayAxis = 0.0f; // Light nodes are spherical, so axis distance is not critical
                result.isLightNode = true;
                
                validCandidates.push_back(result);
                
                std::cout << "[GizmoRayCast] Light node intersection: " << node->GetName() 
                          << " distance=" << distance << " priority=" << result.finalPriority << std::endl;
            }
        }
    }
    
    // NEW: Process audio nodes (sphere intersection around node position)
    for (auto& node : audioNodes) {
        if (!IsNodeGizmoCompatible(node)) continue;
        auto aNode = std::dynamic_pointer_cast<AudioNode>(node);
        if (!aNode) continue;
        
        // Validate the global transform before using it
        glm::mat4 globalTransform = aNode->GetGlobalTransform(glm::mat4(1.0f));
        
        // Check if transform is valid
        bool validTransform = true;
        for (int i = 0; i < 4 && validTransform; ++i) {
            for (int j = 0; j < 4 && validTransform; ++j) {
                if (!std::isfinite(globalTransform[i][j])) {
                    validTransform = false;
                }
            }
        }
        
        if (!validTransform) {
            std::cout << "[GizmoRayCast] Skipping audio node with invalid transform: " << node->GetName() << std::endl;
            continue;
        }
        
        glm::vec3 center = glm::vec3(globalTransform[3]);
        
        // Validate center position
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) {
            std::cout << "[GizmoRayCast] Skipping audio node with invalid center position: " << node->GetName() << std::endl;
            continue;
        }
        
        // Clamp position to reasonable bounds
        const float MAX_COORD = 100000.0f;
        center = glm::clamp(center, glm::vec3(-MAX_COORD), glm::vec3(MAX_COORD));
        
        float radius = aNode->GetSelectionRadius();
        if (!std::isfinite(radius) || radius <= 0.0f) {
            radius = 0.6f; // Default fallback radius
        }
        
        glm::vec3 L = center - ray.origin;
        
        // Validate ray calculations
        if (!std::isfinite(L.x) || !std::isfinite(L.y) || !std::isfinite(L.z)) {
            std::cout << "[GizmoRayCast] Skipping audio node due to invalid ray calculation: " << node->GetName() << std::endl;
            continue;
        }
        
        float tca = glm::dot(L, ray.direction);
        if (tca < 0.0f) continue;
        float d2 = glm::dot(L, L) - tca * tca;
        if (d2 > radius * radius) continue;
        float thc = sqrtf(glm::max(radius * radius - d2, 0.0f));
        float t = tca - thc;
        if (t < 0) t = tca + thc;
        if (t < 0 || t > config.maxSelectionDistance || !std::isfinite(t)) continue;
        
        RayCast::HitResult hit; hit.hit = true; hit.distance = t; hit.point = ray.origin + ray.direction * t; hit.node = node;
        CandidateResult result; result.node = node; result.hit = hit; result.distanceToRayAxis = 0.0f; result.isLightNode = false;
        result.finalPriority = CalculateSelectionPriority(node, hit, currentSelection, config) + 5.0f; // audio boost
        validCandidates.push_back(result);
        std::cout << "[GizmoRayCast] Audio node intersection: " << node->GetName() << " distance=" << t << " priority=" << result.finalPriority << std::endl;
    }
    
    // Then process mesh nodes with regular intersection testing
    for (auto& node : meshNodes) {
        if (!IsNodeGizmoCompatible(node)) continue;
        
        // ENHANCED: More precise intersection testing for mesh nodes
        glm::mat4 worldTransform = node->GetTransform();
        RayCast::HitResult hit = RayCast::RayIntersectNode(ray, node, worldTransform);
        
        if (hit.hit && hit.distance <= config.maxSelectionDistance) {
            CandidateResult result;
            result.node = node;
            result.hit = hit;
            result.finalPriority = CalculateSelectionPriority(node, hit, currentSelection, config);
            result.isLightNode = false;
            
            // Calculate distance to ray axis for small object preference
            glm::vec3 rayToHit = hit.point - ray.origin;
            glm::vec3 projectedPoint = ray.origin + glm::dot(rayToHit, ray.direction) * ray.direction;
            result.distanceToRayAxis = glm::distance(hit.point, projectedPoint);
            
            validCandidates.push_back(result);
        }
    }
    
    if (validCandidates.empty()) {
        return GizmoHitResult();
    }
    
    // ENHANCED: Sort by priority, with light nodes getting natural priority boost
    std::sort(validCandidates.begin(), validCandidates.end(), 
        [](const CandidateResult& a, const CandidateResult& b) {
            // Light nodes always get priority over mesh nodes
            if (a.isLightNode && !b.isLightNode) return true;
            if (!a.isLightNode && b.isLightNode) return false;
            
            // If both are the same type, sort by priority
            if (std::abs(a.finalPriority - b.finalPriority) < 0.1f) {
                return a.distanceToRayAxis < b.distanceToRayAxis;
            }
            return a.finalPriority > b.finalPriority;
        });
    
    // Select the best candidate
    const CandidateResult& best = validCandidates[0];
    
    GizmoHitResult result(best.node, best.hit.point, best.hit.distance);
    result.gizmoCenter = CalculateGizmoPosition(best.node, camera);
    
    std::cout << "[GizmoRayCast] Selected " << (best.isLightNode ? "LIGHT" : "MESH") 
              << " node: " << best.node->GetName() 
              << " (Priority: " << best.finalPriority 
              << ", Distance: " << best.hit.distance 
              << ", Ray Axis Distance: " << best.distanceToRayAxis << ")" << std::endl;
    
    return result;
}

glm::vec3 GizmoRayCast::CalculateGizmoPosition(
    std::shared_ptr<SceneNode> node,
    std::shared_ptr<Camera> camera,
    GizmoMode mode) {
    
    if (!node) {
        return glm::vec3(0.0f);
    }
    
    // ENHANCED: Special handling for light nodes
    if (node->GetNodeType() == SceneNode::LIGHT) {
        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
            if (lightNode->GetLight()) {
                // For lights, always use the actual light position
                return lightNode->GetLight()->GetPosition();
            }
        }
    }
    
    // NEW: Special handling for audio nodes to ensure stable positioning
    if (node->GetNodeType() == SceneNode::AUDIO) {
        glm::mat4 transform = node->GetTransform();
        
        // Validate transform matrix
        bool validTransform = true;
        for (int i = 0; i < 4 && validTransform; ++i) {
            for (int j = 0; j < 4 && validTransform; ++j) {
                if (!std::isfinite(transform[i][j])) {
                    validTransform = false;
                }
            }
        }
        
        if (validTransform) {
            glm::vec3 position = glm::vec3(transform[3]);
            
            // Validate position
            if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
                // Clamp to reasonable bounds
                const float MAX_COORD = 100000.0f;
                position = glm::clamp(position, glm::vec3(-MAX_COORD), glm::vec3(MAX_COORD));
                return position;
            }
        }
        
        // Fallback to origin for audio nodes with invalid transforms
        std::cout << "[GizmoRayCast] Warning: AudioNode has invalid transform, using origin for gizmo position" << std::endl;
        return glm::vec3(0.0f);
    }
    
    // Get node's world position
    glm::mat4 transform = node->GetTransform();
    glm::vec3 position = glm::vec3(transform[3]);
    
    // ENHANCED: More sophisticated positioning based on node characteristics
    if (node->GetModel()) {
        auto [minBounds, maxBounds] = node->GetBoundingBox();
        glm::vec3 size = maxBounds - minBounds;
        glm::vec3 center = (minBounds + maxBounds) * 0.5f;
        
        // For very small objects, prefer the actual center for precision
        if (glm::length(size) < 2.0f) {
            position = center;
        }
        
        switch (mode) {
            case GizmoMode::TRANSLATE:
                // For translation, use the geometric center for small objects, origin for large ones
                if (glm::length(size) < 5.0f) {
                    return center;
                } else {
                    return position; // Use transform origin for large objects
                }
                
            case GizmoMode::ROTATE:
                // For rotation, always prefer the geometric center for intuitive rotation
                return center;
                
            case GizmoMode::SCALE:
                // For scaling, use geometric center for uniform scaling feel
                return center;
                
            case GizmoMode::UNIVERSAL:
            default:
                // For universal mode, use a weighted average based on object size
                float sizeWeight = glm::clamp(glm::length(size) / 10.0f, 0.0f, 1.0f);
                return glm::mix(center, position, sizeWeight);
        }
    }
    
    // Fallback to transform position
    return position;
}

float GizmoRayCast::CalculateGizmoScale(
    const glm::vec3& gizmoPosition,
    std::shared_ptr<Camera> camera,
    float baseScale) {
    
    if (!camera) {
        return baseScale;
    }
    
    // Calculate distance-based scaling to maintain consistent screen size
    glm::vec3 cameraPos = camera->GetCameraPosition();
    float distance = glm::distance(cameraPos, gizmoPosition);
    
    // Scale gizmo based on distance (adjust multiplier as needed)
    float scaleFactor = distance * 0.1f; // Adjust this multiplier based on your needs
    return baseScale * glm::max(0.1f, scaleFactor);
}

bool GizmoRayCast::IsNodeGizmoCompatible(std::shared_ptr<SceneNode> node) {
    if (!node) return false;
    
    // PRIORITY FIX: Check if this is a LightNode first
    // Light nodes are ALWAYS gizmo-compatible regardless of having a model
    if (node->GetNodeType() == SceneNode::LIGHT) {
        // Cast to LightNode to verify it has a valid light and selection proxy
        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
            if (lightNode->GetLight()) {
                // Ensure the light node has a valid selection proxy
                lightNode->UpdateSelectionProxy();
                return true; // Lights are always manipulable
            }
        }
    }
    
    // NEW: Audio nodes always manipulable (allow positioning)
    if (node->GetNodeType() == SceneNode::AUDIO) {
        return true;
    }
    
    // For non-light nodes, only allow gizmo manipulation on nodes with models
    if (!node->GetModel()) return false;
    
    // Enhanced filtering: Check for valid bounding box
    auto [minBounds, maxBounds] = node->GetBoundingBox();
    glm::vec3 size = maxBounds - minBounds;
    
    // Reject nodes with invalid or extremely large bounding boxes
    if (glm::length(size) > 1000.0f || glm::length(size) < 0.001f) {
        return false;
    }
    
    // Could add additional filters here:
    // - Check if node is locked
    // - Check node type restrictions
    // - Check if node has physics body that shouldn't be moved manually
    
    return true;
}

glm::vec3 GizmoRayCast::SnapPosition(const glm::vec3& position, float snapIncrement) {
    if (snapIncrement <= 0.0f) {
        return position;
    }
    
    return glm::round(position / snapIncrement) * snapIncrement;
}

glm::vec3 GizmoRayCast::SnapRotation(const glm::vec3& rotation, float snapIncrement) {
    if (snapIncrement <= 0.0f) {
        return rotation;
    }
    
    float snapRad = glm::radians(snapIncrement);
    return glm::round(rotation / snapRad) * snapRad;
}

std::pair<glm::vec3, glm::vec3> GizmoRayCast::CalculateMultiSelectionBounds(
    const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    
    if (nodes.empty()) {
        return {glm::vec3(0.0f), glm::vec3(0.0f)};
    }
    
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
    
    for (auto& node : nodes) {
        if (!node || !node->GetModel()) continue;
        
        auto [nodeMin, nodeMax] = node->GetBoundingBox();
        minBounds = glm::min(minBounds, nodeMin);
        maxBounds = glm::max(maxBounds, nodeMax);
    }
    
    return {minBounds, maxBounds};
}

bool GizmoRayCast::IsPointInGizmoArea(
    const glm::vec2& point,
    const glm::vec2& gizmoCenter,
    float gizmoSize,
    float tolerance) {
    
    float distance = glm::distance(point, gizmoCenter);
    return distance <= (gizmoSize * 0.5f + tolerance);
}

bool GizmoRayCast::GizmoNodeFilter(std::shared_ptr<SceneNode> node) {
    return IsNodeGizmoCompatible(node);
}

float GizmoRayCast::CalculateSelectionPriority(
    std::shared_ptr<SceneNode> node,
    const RayCast::HitResult& hit,
    std::shared_ptr<SceneNode> currentSelection,
    const GizmoConfig& config) {
    
    float priority = 1.0f;
    
    // ASSIVE PRIORITY BOOST FOR LIGHT NODES
    // Light nodes should almost always take precedence over mesh nodes for editor workflow
    if (node->GetNodeType() == SceneNode::LIGHT) {
        priority += 10.0f; // Major priority boost for lights
        
        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
            float lightPriority = CalculateLightNodePriority(lightNode, hit);
            priority += lightPriority; // Additional light-specific priority
        }
        
        std::cout << "[GizmoRayCast] Light node gets major priority boost: " << priority << std::endl;
    }
    
    // Distance-based priority (closer = higher priority)
    float normalizedDistance = hit.distance / config.maxSelectionDistance;
    priority *= (1.0f - normalizedDistance);
    
    // SIZE BIAS: Smaller objects get higher priority when overlapping
    float sizeBonus = CalculateNodeSizeBonus(node);
    priority += sizeBonus;
    
    // PRECISION BIAS: More precise intersection gets higher priority
    float precisionBonus = CalculateIntersectionPrecision(node, hit);
    priority += precisionBonus;
    
    // VISIBILITY BIAS: Fully visible nodes get priority over partially occluded ones
    float visibilityBonus = CalculateVisibilityBonus(node, hit);
    priority += visibilityBonus;
    
    // Bias toward current selection (but not as strong as light node priority)
    if (config.prioritizeSelectedNode && node == currentSelection) {
        priority += config.selectionBias * 0.5f; // Reduced to allow light override
    }
    
    return priority;
}

float GizmoRayCast::CalculateNodeSizeBonus(std::shared_ptr<SceneNode> node) {
    if (!node) return 0.0f;
    
    // For light nodes, use light-specific sizing
    if (node->GetNodeType() == SceneNode::LIGHT) {
        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
            auto proxy = lightNode->GetSelectionProxy();
            if (proxy.isValid) {
                // Smaller light selection areas get higher priority
                float radius = proxy.radius;
                return glm::clamp(2.0f / (radius + 0.5f), 0.0f, 1.5f);
            }
        }
        return 0.5f; // Default bonus for lights
    }
    
    // For audio nodes, boost selection modestly
    if (node->GetNodeType() == SceneNode::AUDIO) {
        return 1.0f; // modest constant to ease selection
    }
    
    // For regular nodes, calculate based on bounding box
    if (!node->GetModel()) return 0.0f;
    
    auto [minBounds, maxBounds] = node->GetBoundingBox();
    glm::vec3 size = maxBounds - minBounds;
    float volume = size.x * size.y * size.z;
    
    // Logarithmic scaling: smaller objects get exponentially higher bonus
    float normalizedVolume = glm::clamp(volume / 100.0f, 0.001f, 100.0f);
    float sizeBonus = 1.0f - glm::log(normalizedVolume + 1.0f) / glm::log(101.0f);
    
    // Scale bonus to reasonable range
    return sizeBonus * 2.0f; // Up to 2.0 bonus for very small objects
}

float GizmoRayCast::CalculateIntersectionPrecision(std::shared_ptr<SceneNode> node, const RayCast::HitResult& hit) {
    if (!node || !hit.hit) return 0.0f;
    
    // Calculate how close the hit point is to the object's center
    auto [minBounds, maxBounds] = node->GetBoundingBox();
    glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    glm::vec3 size = maxBounds - minBounds;
    
    float distanceToCenter = glm::distance(hit.point, center);
    float maxDistanceToCenter = glm::length(size) * 0.5f;
    
    if (maxDistanceToCenter < 0.001f) return 0.5f;
    
    // Hits closer to center get higher precision bonus
    float normalizedDistance = distanceToCenter / maxDistanceToCenter;
    return (1.0f - normalizedDistance) * 0.5f; // Up to 0.5 bonus
}

float GizmoRayCast::CalculateVisibilityBonus(std::shared_ptr<SceneNode> node, const RayCast::HitResult& hit) {
    // For now, return a small bonus for all visible hits
    // This could be enhanced with actual visibility testing
    return hit.hit ? 0.1f : 0.0f;
}

float GizmoRayCast::CalculateLightNodePriority(std::shared_ptr<LightNode> lightNode, const RayCast::HitResult& hit) {
    if (!lightNode || !lightNode->GetLight()) return 0.0f;
    
    auto light = lightNode->GetLight();
    glm::vec3 lightPos = light->GetPosition();
    
    // Distance from hit point to light center
    float distanceToLight = glm::distance(hit.point, lightPos);
    
    // Different priority based on light type
    float basePriority = 0.0f;
    switch (light->GetLightType()) {
        case BaseLight::LightType::DIRECTIONAL:
            basePriority = 1.0f; // Directional lights are important
            break;
        case BaseLight::LightType::POINT:
            basePriority = 0.8f;
            break;
        case BaseLight::LightType::SPOT:
            basePriority = 0.9f; // Spot lights are quite important
            break;
        default:
            basePriority = 0.5f;
            break;
    }
    
    // Bonus for hits very close to light center (within 0.5 units)
    if (distanceToLight < 0.5f) {
        basePriority += 1.0f;
    }
    
    return basePriority;
}