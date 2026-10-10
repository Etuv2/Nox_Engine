#pragma once
#include "SceneRenderExtras.h"
#include <glm/glm.hpp>
#include <array>
#include <string>

/*
* Night-street game content: knowledge of the three texture packs under textures/ (road_decals,
* lens_flares, window_rooms; see each README.md) and how the street generators use them. Produces the
* generic engine descriptors in SceneRenderExtras.h; contains no rendering code.
*/
namespace NightStreetPacks {

	// Road decal layers (order of the DecalSet built by BuildRoadDecalSet).
	enum class RoadDecal : int {
		ArrowStraight, ArrowLeft, ArrowRight, ArrowStraightLeft, ArrowStraightRight,
		LineDoubleYellow, LineSingleYellow, LaneDashWhite, LineSingleWhite, StopBarWhite, CrosswalkWhite,
		Count
	};

	// Lens flare styles (order of LensFlareSet::styles built by BuildLensFlareSet).
	enum FlareStyle : int {
		FlareSodium = 0,   // warm HPS streetlight: sodium glow + warm four-ray starburst
		FlareLED,          // cool LED streetlight: LED glow + cool six-ray starburst
		FlareFlood,        // floodlight: LED glow, six-ray starburst, faint streak, halo at strong glare
		FlareSignal,       // small coloured signal lens: source-tinted LED glow only
		FlareStyleCount
	};

	// Interior atlas weight rows (material interior "style").
	enum RoomStyle : int { RoomApartment = 0, RoomShop = 1, RoomOffice = 2 };

	// Measured paint footprint of every decal (normalised top-left image coordinates).
	struct DecalPaintInfo {
		std::array<glm::vec4, static_cast<size_t>(RoadDecal::Count)> bounds{}; // (left, top, right, bottom)
		std::array<float, static_cast<size_t>(RoadDecal::Count)> tailU{};      // arrow stem centre (u), else bounds centre
	};
	// Cached per root; nullptr when the pack or its metadata is missing.
	const DecalPaintInfo* LoadDecalPaintInfo(const std::string& root);

	// Fill the generic descriptors (texture layers, materials, styles, weights). Placement is left to
	// the caller. Each returns false (and leaves `out` untouched) when its pack is incomplete.
	bool BuildRoadDecalSet(const std::string& root, DecalSet& out);
	bool BuildLensFlareSet(const std::string& root, LensFlareSet& out);
	bool BuildInteriorAtlas(const std::string& root, InteriorAtlas& out);
}
