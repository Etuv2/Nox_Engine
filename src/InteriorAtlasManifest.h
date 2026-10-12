#pragma once
#include "SceneRenderExtras.h"

#include <string>

/*
* Generic interior-mapping atlas loader. Reads an atlas manifest (schema in textures/window_rooms/README.md:
* "assets" with per-face sample rectangles, plus a "nox_interior" block for face order, room light, style
* weights, room shape and pane UV layout) from a logical directory and fills SceneRenderExtras::interiorAtlas.
* Serves the generic scene key "interior_atlas" (and the compatibility key "window_room_root"); it knows no
* content names. Errors are printed with the "[InteriorAtlas]" tag.
*/
namespace InteriorAtlasManifest {
	// logicalRoot: directory holding atlas_manifest.json (AssetIO logical path, e.g. "textures/window_rooms").
	bool Load(const std::string& logicalRoot, InteriorAtlas& out);
}
