#ifndef VERTEX_H
#define VERTEX_H

#include <glm/glm.hpp>

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 tangent;  // Include w component for handedness
    glm::vec4 color;

    glm::ivec4 boneIDs = glm::ivec4(0);
    glm::vec4  boneWeights = glm::vec4(0.0f);
};

#endif
