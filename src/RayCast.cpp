#include "RayCast.h"
#include "SceneNode.h"
#include "Camera.h"
#include "Scene.h"
#include <iostream>
#include <algorithm>
#include <limits>

RayCast::Ray RayCast::ScreenToWorldRay(int mouseX, int mouseY, std::shared_ptr<Camera> camera, 
                                      int windowWidth, int windowHeight) {
    if (!camera) {
        std::cout << "[RayCast] Error: Camera is null" << std::endl;
        return Ray(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    }

    // Validate input coordinates
    if (mouseX < 0 || mouseX >= windowWidth || mouseY < 0 || mouseY >= windowHeight) {
        std::cout << "[RayCast] Warning: Mouse coordinates outside window bounds" << std::endl;
    }

    // Convert screen coordinates to normalized device coordinates
    float x = (2.0f * mouseX) / windowWidth - 1.0f;
    float y = 1.0f - (2.0f * mouseY) / windowHeight;

    // Create ray in clip space
    glm::vec4 rayClip = glm::vec4(x, y, -1.0f, 1.0f);

    // Convert to eye space
    glm::mat4 projectionMatrix = camera->GetProjectionMatrix();
    glm::mat4 invProjection = glm::inverse(projectionMatrix);
    glm::vec4 rayEye = invProjection * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);

    // Convert to world space
    glm::mat4 viewMatrix = camera->GetViewMatrix();
    glm::mat4 invView = glm::inverse(viewMatrix);
    glm::vec3 rayWorld = glm::vec3(invView * rayEye);
    
    // Create and return ray
    return Ray(camera->GetCameraPosition(), glm::normalize(rayWorld));
}

RayCast::HitResult RayCast::RayIntersectAABB(const Ray& ray, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
    // Ray-AABB intersection using slab method
    glm::vec3 invDir = 1.0f / ray.direction;
    glm::vec3 t1 = (aabbMin - ray.origin) * invDir;
    glm::vec3 t2 = (aabbMax - ray.origin) * invDir;

    glm::vec3 tMin = glm::min(t1, t2);
    glm::vec3 tMax = glm::max(t1, t2);

    float tNear = glm::max(glm::max(tMin.x, tMin.y), tMin.z);
    float tFar = glm::min(glm::min(tMax.x, tMax.y), tMax.z);

    if (tNear <= tFar && tFar >= 0.0f) {
        float hitDistance = tNear >= 0.0f ? tNear : tFar;
        glm::vec3 hitPoint = ray.GetPoint(hitDistance);
        
        // Calculate normal based on which face was hit
        glm::vec3 center = (aabbMin + aabbMax) * 0.5f;
        glm::vec3 extents = (aabbMax - aabbMin) * 0.5f;
        glm::vec3 localHit = hitPoint - center;
        
        glm::vec3 normal(0.0f);
        float maxComponent = 0.0f;
        
        // Find the axis with the largest relative distance
        for (int i = 0; i < 3; ++i) {
            float absComponent = glm::abs(localHit[i] / extents[i]);
            if (absComponent > maxComponent) {
                maxComponent = absComponent;
                normal = glm::vec3(0.0f);
                normal[i] = localHit[i] > 0.0f ? 1.0f : -1.0f;
            }
        }
        
        return HitResult(true, hitDistance, hitPoint, normal);
    }
    
    return HitResult();
}

RayCast::HitResult RayCast::RayIntersectOBB(const Ray& ray, const glm::vec3& center, 
                                          const glm::vec3& halfExtents, const glm::mat3& orientation) {
    // Transform ray to OBB local space
    glm::mat3 invOrientation = glm::transpose(orientation);
    glm::vec3 localOrigin = invOrientation * (ray.origin - center);
    glm::vec3 localDirection = invOrientation * ray.direction;
    
    // Perform AABB test in local space
    Ray localRay(localOrigin, localDirection);
    HitResult result = RayIntersectAABB(localRay, -halfExtents, halfExtents);
    
    if (result.hit) {
        // Transform results back to world space
        result.point = orientation * result.point + center;
        result.normal = orientation * result.normal;
    }
    
    return result;
}

RayCast::HitResult RayCast::RayIntersectSphere(const Ray& ray, const glm::vec3& center, float radius) {
    glm::vec3 oc = ray.origin - center;
    float a = glm::dot(ray.direction, ray.direction);
    float b = 2.0f * glm::dot(oc, ray.direction);
    float c = glm::dot(oc, oc) - radius * radius;
    
    float discriminant = b * b - 4 * a * c;
    
    if (discriminant < 0) {
        return HitResult(); // No intersection
    }
    
    float sqrtDiscriminant = glm::sqrt(discriminant);
    float t1 = (-b - sqrtDiscriminant) / (2.0f * a);
    float t2 = (-b + sqrtDiscriminant) / (2.0f * a);
    
    float t = (t1 >= 0.0f) ? t1 : t2;
    
    if (t >= 0.0f) {
        glm::vec3 hitPoint = ray.GetPoint(t);
        glm::vec3 normal = glm::normalize(hitPoint - center);
        return HitResult(true, t, hitPoint, normal);
    }
    
    return HitResult();
}

RayCast::HitResult RayCast::RayIntersectPlane(const Ray& ray, const glm::vec3& planePoint, const glm::vec3& planeNormal) {
    float denom = glm::dot(planeNormal, ray.direction);
    
    if (glm::abs(denom) < 1e-6f) {
        return HitResult(); // Ray is parallel to plane
    }
    
    float t = glm::dot(planePoint - ray.origin, planeNormal) / denom;
    
    if (t >= 0.0f) {
        glm::vec3 hitPoint = ray.GetPoint(t);
        return HitResult(true, t, hitPoint, planeNormal);
    }
    
    return HitResult();
}

RayCast::HitResult RayCast::RayIntersectTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) {
    // Möller-Trumbore intersection algorithm
    const float EPSILON = 0.0000001f;
    
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(ray.direction, edge2);
    float a = glm::dot(edge1, h);
    
    if (a > -EPSILON && a < EPSILON) {
        return HitResult(); // Ray is parallel to triangle
    }
    
    float f = 1.0f / a;
    glm::vec3 s = ray.origin - v0;
    float u = f * glm::dot(s, h);
    
    if (u < 0.0f || u > 1.0f) {
        return HitResult();
    }
    
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(ray.direction, q);
    
    if (v < 0.0f || u + v > 1.0f) {
        return HitResult();
    }
    
    float t = f * glm::dot(edge2, q);
    
    if (t > EPSILON) {
        glm::vec3 hitPoint = ray.GetPoint(t);
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));
        return HitResult(true, t, hitPoint, normal);
    }
    
    return HitResult();
}

RayCast::HitResult RayCast::RayIntersectNode(const Ray& ray, std::shared_ptr<SceneNode> node, const glm::mat4& worldTransform) {
    if (!node || !node->GetModel()) {
        return HitResult();
    }

    // Get the node's bounding box in local space
    auto [localMin, localMax] = node->GetBoundingBox();
    
    // Check if bounding box is valid
    if (localMin.x >= localMax.x || localMin.y >= localMax.y || localMin.z >= localMax.z) {
        return HitResult();
    }
    
    // Transform bounding box to world space
    glm::vec3 corners[8] = {
        glm::vec3(localMin.x, localMin.y, localMin.z),
        glm::vec3(localMax.x, localMin.y, localMin.z),
        glm::vec3(localMin.x, localMax.y, localMin.z),
        glm::vec3(localMax.x, localMax.y, localMin.z),
        glm::vec3(localMin.x, localMin.y, localMax.z),
        glm::vec3(localMax.x, localMin.y, localMax.z),
        glm::vec3(localMin.x, localMax.y, localMax.z),
        glm::vec3(localMax.x, localMax.y, localMax.z)
    };

    glm::vec3 worldMin = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 worldMax = glm::vec3(std::numeric_limits<float>::lowest());

    for (int i = 0; i < 8; ++i) {
        glm::vec3 worldCorner = glm::vec3(worldTransform * glm::vec4(corners[i], 1.0f));
        worldMin = glm::min(worldMin, worldCorner);
        worldMax = glm::max(worldMax, worldCorner);
    }

    HitResult result = RayIntersectAABB(ray, worldMin, worldMax);
    if (result.hit) {
        result.node = node;
    }
    
    return result;
}

RayCast::HitResult RayCast::RayIntersectScene(const Ray& ray, std::shared_ptr<SceneNode> rootNode,
                                            std::function<bool(std::shared_ptr<SceneNode>)> filter) {
    std::vector<HitResult> allResults;
    TraverseSceneForIntersection(ray, rootNode, glm::mat4(1.0f), allResults, filter);
    
    if (allResults.empty()) {
        return HitResult();
    }
    
    // Sort by distance and return closest
    std::sort(allResults.begin(), allResults.end(), 
              [](const HitResult& a, const HitResult& b) {
                  return a.distance < b.distance;
              });
    
    return allResults[0];
}

std::vector<RayCast::HitResult> RayCast::RayIntersectSceneAll(const Ray& ray, std::shared_ptr<SceneNode> rootNode,
                                                            size_t maxResults,
                                                            std::function<bool(std::shared_ptr<SceneNode>)> filter) {
    std::vector<HitResult> results;
    TraverseSceneForIntersection(ray, rootNode, glm::mat4(1.0f), results, filter);
    
    // Sort by distance
    std::sort(results.begin(), results.end(), 
              [](const HitResult& a, const HitResult& b) {
                  return a.distance < b.distance;
              });
    
    // Limit results if requested
    if (maxResults > 0 && results.size() > maxResults) {
        results.resize(maxResults);
    }
    
    return results;
}

bool RayCast::PointInAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
    return (point.x >= aabbMin.x && point.x <= aabbMax.x &&
            point.y >= aabbMin.y && point.y <= aabbMax.y &&
            point.z >= aabbMin.z && point.z <= aabbMax.z);
}

glm::vec3 RayCast::ClosestPointOnAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
    return glm::clamp(point, aabbMin, aabbMax);
}

float RayCast::DistanceToAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
    glm::vec3 closest = ClosestPointOnAABB(point, aabbMin, aabbMax);
    return glm::distance(point, closest);
}

void RayCast::TraverseSceneForIntersection(const Ray& ray, std::shared_ptr<SceneNode> node, 
                                         const glm::mat4& parentTransform,
                                         std::vector<HitResult>& results,
                                         std::function<bool(std::shared_ptr<SceneNode>)> filter) {
    if (!node) return;
    
    // Apply filter if provided
    if (filter && !filter(node)) {
        return;
    }
    
    glm::mat4 worldTransform = parentTransform * node->GetTransform();
    
    // Test intersection with this node if it has a model
    if (node->GetModel()) {
        HitResult result = RayIntersectNode(ray, node, worldTransform);
        if (result.hit) {
            results.push_back(result);
        }
    }

    // Traverse children
    for (auto& child : node->children) {
        TraverseSceneForIntersection(ray, child, worldTransform, results, filter);
    }
}