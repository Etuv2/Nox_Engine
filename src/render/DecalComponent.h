#pragma once
// Decal component state (Decal on a node): a textured quad on the surface under the node, drawn by
// DecalPass with the scene's decal sets (SceneRenderExtras::decalSets).

#include <string>
#include <glm/glm.hpp>

namespace nox::render {

struct DecalComponent {
	std::string texture;             // logical image path (straight-alpha colour); empty = not drawn
	glm::vec3 size{ 1.0f, 0.1f, 1.0f };  // x across, z along the node's local axes (node scale applies); y reserved
	glm::vec4 color{ 1.0f };         // rgb multiplier, a coverage multiplier
	float opacity = 1.0f;           // [0,1] coverage multiplier applied on top of color.a
};

} // namespace nox::render
