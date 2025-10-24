// FrustumCulling.h
#pragma once
#include <glm/glm.hpp>
#include <array>

// Each plane: (a, b, c, d) in ax + by + cz + d = 0 form
struct Plane {
    glm::vec4 p;
};

class Frustum {
public:
    std::array<Plane, 6> planes;
};

Frustum ExtractFrustum(const glm::mat4& lightSpaceMatrix);

// AABB vs. frustum intersection
bool AABBInFrustum(const glm::vec3& minAABB, const glm::vec3& maxAABB, const Frustum& frustum);
