#pragma once

#include <glm/glm.hpp>
#include <cstdint>

struct alignas(16) TransformHistoryRecord {
    glm::vec4 worldPosition{ 0.0f, 0.0f, 0.0f, 1.0f };
    glm::vec4 prevWorldPosition{ 0.0f, 0.0f, 0.0f, 1.0f };
    glm::uvec4 metadata{ 0u, 0u, 0u, 0u }; // x=lastSeenFrame, y=visiblePixelCount, z=generation, w=flags
};

struct alignas(16) VisibleTransformListHeader {
    glm::uvec4 counts{ 0u, 0u, 0u, 0u }; // x=visibleCount
};
