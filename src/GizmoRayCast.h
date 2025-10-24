#pragma once

#include <glm/glm.hpp>
#include <memory>
#include "RayCast.h"
#include "BVH.h"

class SceneNode;
class Camera;

/**
 * @brief Utility class for gizmo-related ray casting and spatial queries
 * 
 * This class provides specialized ray casting utilities for gizmo interactions,
 * including hover detection, selection validation, and gizmo placement calculations.
 */
class GizmoRayCast {
public:
    /**
     * @brief Gizmo interaction modes
     */
    enum class GizmoMode {
        TRANSLATE,
        ROTATE,
        SCALE,
        UNIVERSAL
    };
    
    /**
     * @brief Result of gizmo-specific ray queries
     */
    struct GizmoHitResult {
        bool hit = false;
        std::shared_ptr<SceneNode> node = nullptr;
        glm::vec3 worldPosition = glm::vec3(0.0f);
        glm::vec3 gizmoCenter = glm::vec3(0.0f);
        float distanceToCamera = 0.0f;
        bool isValidForGizmo = false;
        
        GizmoHitResult() = default;
        GizmoHitResult(std::shared_ptr<SceneNode> n, const glm::vec3& pos, float dist)
            : hit(true), node(n), worldPosition(pos), distanceToCamera(dist), isValidForGizmo(true) {
            gizmoCenter = pos; // Default to hit position
        }
    };
    
    /**
     * @brief Configuration for gizmo ray casting
     */
    struct GizmoConfig {
        float maxSelectionDistance = 1000.0f;
        float gizmoScale = 1.0f;
        bool enableBVHAcceleration = true;
        bool prioritizeSelectedNode = true;
        float selectionBias = 0.1f; // Bias toward keeping current selection
        
        // ENHANCED: Precision and small object preferences
        bool preferSmallerObjects = true;        // Bias toward smaller objects when overlapping
        float smallObjectBonus = 2.0f;          // Bonus multiplier for small objects
        float precisionTolerance = 0.1f;        // Tolerance for considering priorities "equal"
        bool enableExpandedRaySearch = true;    // Use expanded ray search for small objects
        float rayExpansionRadius = 0.1f;        // Radius for expanded ray search
        bool enableMultiPassSelection = true;   // Use multi-pass selection algorithm
    };
    
    /**
     * @brief Perform ray casting specifically for gizmo selection
     * @param ray Ray to cast
     * @param camera Camera for distance calculations
     * @param sceneBVH Optional BVH for acceleration
     * @param currentSelection Currently selected node (for bias)
     * @param config Configuration parameters
     * @return Gizmo-specific hit result
     */
    static GizmoHitResult QueryForGizmoSelection(
        const RayCast::Ray& ray,
        std::shared_ptr<Camera> camera,
        BVH::SceneNodeBVH* sceneBVH = nullptr,
        std::shared_ptr<SceneNode> currentSelection = nullptr,
        const GizmoConfig& config = GizmoConfig()
    );
    
    /**
     * @brief Calculate optimal gizmo position for a node
     * @param node Node to calculate gizmo position for
     * @param camera Camera for view-dependent calculations
     * @param mode Gizmo mode for position adjustment
     * @return Optimal gizmo center position in world space
     */
    static glm::vec3 CalculateGizmoPosition(
        std::shared_ptr<SceneNode> node,
        std::shared_ptr<Camera> camera,
        GizmoMode mode = GizmoMode::TRANSLATE
    );
    
    /**
     * @brief Calculate appropriate gizmo scale based on distance to camera
     * @param gizmoPosition Position of gizmo in world space
     * @param camera Camera for distance calculation
     * @param baseScale Base scale factor
     * @return Appropriate scale for gizmo
     */
    static float CalculateGizmoScale(
        const glm::vec3& gizmoPosition,
        std::shared_ptr<Camera> camera,
        float baseScale = 1.0f
    );
    
    /**
     * @brief Check if a node is suitable for gizmo manipulation
     * @param node Node to check
     * @return True if node can be manipulated with gizmos
     */
    static bool IsNodeGizmoCompatible(std::shared_ptr<SceneNode> node);
    
    /**
     * @brief Calculate snap position for gizmo operations
     * @param position Original position
     * @param snapIncrement Snap increment (0 = no snapping)
     * @return Snapped position
     */
    static glm::vec3 SnapPosition(const glm::vec3& position, float snapIncrement = 0.0f);
    
    /**
     * @brief Calculate snap rotation for gizmo operations
     * @param rotation Original rotation (in radians)
     * @param snapIncrement Snap increment in degrees (0 = no snapping)
     * @return Snapped rotation
     */
    static glm::vec3 SnapRotation(const glm::vec3& rotation, float snapIncrement = 0.0f);
    
    /**
     * @brief Calculate bounding box for multiple selected nodes
     * @param nodes Vector of selected nodes
     * @return Combined bounding box
     */
    static std::pair<glm::vec3, glm::vec3> CalculateMultiSelectionBounds(
        const std::vector<std::shared_ptr<SceneNode>>& nodes
    );
    
    /**
     * @brief Test if a point is within gizmo interaction area
     * @param point Point to test in screen space
     * @param gizmoCenter Gizmo center in screen space
     * @param gizmoSize Gizmo size in screen pixels
     * @param tolerance Additional tolerance in pixels
     * @return True if point is within interaction area
     */
    static bool IsPointInGizmoArea(
        const glm::vec2& point,
        const glm::vec2& gizmoCenter,
        float gizmoSize,
        float tolerance = 10.0f
    );

private:
    /**
     * @brief Filter function for gizmo-compatible nodes
     */
    static bool GizmoNodeFilter(std::shared_ptr<SceneNode> node);
    
    /**
     * @brief Calculate selection priority for a node with advanced size and precision bias
     */
    static float CalculateSelectionPriority(
        std::shared_ptr<SceneNode> node,
        const RayCast::HitResult& hit,
        std::shared_ptr<SceneNode> currentSelection,
        const GizmoConfig& config
    );
    
    /**
     * @brief Calculate size-based bonus (smaller objects get higher priority)
     */
    static float CalculateNodeSizeBonus(std::shared_ptr<SceneNode> node);
    
    /**
     * @brief Calculate precision bonus based on intersection accuracy
     */
    static float CalculateIntersectionPrecision(std::shared_ptr<SceneNode> node, const RayCast::HitResult& hit);
    
    /**
     * @brief Calculate visibility bonus for fully visible objects
     */
    static float CalculateVisibilityBonus(std::shared_ptr<SceneNode> node, const RayCast::HitResult& hit);
    
    /**
     * @brief Calculate special priority for light nodes
     */
    static float CalculateLightNodePriority(std::shared_ptr<class LightNode> lightNode, const RayCast::HitResult& hit);
};