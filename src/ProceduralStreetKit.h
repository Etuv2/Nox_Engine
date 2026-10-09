#pragma once
#include <string>
#include "json.hpp"

/*
* Procedural night-street kit (docs/nfs2015/README.md, milestone M0).
*
* Generates a glTF 2.0 scene (.gltf + .bin) containing a street, sidewalks, road markings,
* building blocks with lit/dark windows, neon signage, streetlight poles, a tunnel and an
* industrial strip. Textures are referenced from textures/cc0 (git-ignored, CC0 Poly Haven
* sets, see textures/cc0/SOURCES.md); missing textures fall back to flat factors.
*
* The geometry is loaded through the normal glTF import path so shadows, BVH/RT, surfel GI
* and material handling all work unchanged. Lights are returned as scene-schema JSON nodes
* that SceneLoader instantiates like authored light nodes.
*
* Units: metres, +Y up, the street runs along +Z starting at z = 0.
*/
namespace ProceduralStreetKit {

	struct Result {
		bool ok = false;
		std::string gltfPath;          // Generated .gltf, relative to the working directory
		nlohmann::json lightNodes = nlohmann::json::array(); // Scene-schema "light" nodes
		nlohmann::json reflectionProbes = nlohmann::json::array(); // scene "reflection_probes" entries
		int triangleCount = 0;
		int primitiveCount = 0;
	};

	// params (all optional):
	//   seed (int), length (m), output (path stem, default "generated/nfs_street"),
	//   texture_root (default "textures/cc0"), streetlight_spacing (m),
	//   neon_lights (bool), tunnel (bool), industrial (bool), shadowed_streetlights (int)
	Result Generate(const nlohmann::json& params);
}
