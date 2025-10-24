#pragma once
#include <glm/glm.hpp>

struct Contact {
    glm::vec3 point;
    glm::vec3 normal;
    float penetration;
};
