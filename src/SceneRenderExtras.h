#pragma once
#include "TextureArrayBuilder.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

/*
* Optional per-scene render data consumed by generic engine passes. Any loader, generator or game
* layer may fill it; the engine knows nothing about where the textures or placements come from.
*
*   decalSets    -> DecalPass: textured quads blended into the G-buffer (albedo + roughness)
*   lensFlares   -> LensFlarePass: occlusion-tested, camera-facing glare sprites on bright sources
*   interiorAtlas-> G-buffer interior mapping (shaders/includes/interior_mapping.glsl): baked
*                   room boxes behind window panes
*
* Texture sources are file lists (TextureArrayBuilder); passes build and cache the GL arrays lazily.
*/

// ------------------------------------------------------------------------------------------------
// Decals
// ------------------------------------------------------------------------------------------------

// One quad spanning center +- axisU +- axisV (half extents folded in). axisU points to the image's
// right, axisV to its top; the surface normal is cross(axisU, axisV). uvRect is the sampled region
// in top-left image coordinates (u0, v0, u1, v1).
struct DecalInstance {
	glm::vec3 center{ 0.0f };
	glm::vec3 axisU{ 1.0f, 0.0f, 0.0f };
	glm::vec3 axisV{ 0.0f, 0.0f, -1.0f };
	glm::vec4 uvRect{ 0.0f, 0.0f, 1.0f, 1.0f };
	int layer = 0;                 // texture array layer
	glm::vec4 tint{ 1.0f };        // rgb albedo multiplier, a coverage multiplier
};

// Surface response of everything in a set (the decal replaces albedo and roughness by coverage).
struct DecalMaterial {
	float albedoScale = 1.0f;
	float roughness = 0.5f;
	float wetnessResponse = 0.0f;  // weather_common.glsl wetness / puddle model
	float puddleResponse = 0.0f;
};

struct DecalSet {
	std::string name;
	std::vector<TextureArrayBuilder::Layer> layers;   // straight-alpha colour images
	TextureArrayBuilder::Options textureOptions;
	DecalMaterial material;
	std::vector<DecalInstance> instances;
};

// ------------------------------------------------------------------------------------------------
// Lens flares
// ------------------------------------------------------------------------------------------------

// One sprite layer of a flare style. Sizes are relative to the style's base glow size.
struct LensFlareLayer {
	int textureLayer = 0;
	float gain = 1.0f;             // relative to the source's glare radiance
	float width = 1.0f;            // sprite width relative to the base glow size
	glm::vec3 tint{ 1.0f };
	bool useSourceTint = false;    // multiply by LensFlareSource::tint as well
	float glareMin = 0.0f;         // layer fades in over [glareMin, glareMin + 0.4] of glare strength
	float glareWeight = 0.0f;      // 0 = always on (above glareMin); 1 = gain scales with glare
};

struct LensFlareStyle {
	static constexpr int kMaxLayers = 4;
	std::vector<LensFlareLayer> layers;  // up to kMaxLayers
};

// A visible bright source. facing = main emission direction (zero = omnidirectional);
// luminance in cd/m^2 (typically the emitter's emissive strength).
struct LensFlareSource {
	glm::vec3 position{ 0.0f };
	int style = 0;
	glm::vec3 facing{ 0.0f };
	float luminance = 1000.0f;
	glm::vec3 tint{ 1.0f };
	float size = 1.0f;             // apparent glow diameter in metres before screen clamping
};

struct LensFlareSet {
	std::vector<TextureArrayBuilder::Layer> layers;   // straight-alpha sprite images
	TextureArrayBuilder::Options textureOptions;
	std::vector<LensFlareStyle> styles;
	std::vector<LensFlareSource> sources;
	float luminanceToGlare = 1.2e-3f;  // glare cd/m^2 per lens cd/m^2
	float gain = 1.0f;                 // scene-level multiplier
	float minPixels = 22.0f;           // base glow size clamp at referenceHeight
	float maxPixels = 64.0f;
	float referenceHeight = 1440.0f;
	float fadeStart = 90.0f;           // distance fade (m)
	float fadeEnd = 220.0f;
	float visibilityRate = 14.0f;      // 1/s, occlusion easing
};

// ------------------------------------------------------------------------------------------------
// Interior mapping atlas
// ------------------------------------------------------------------------------------------------

// Rooms behind window panes (interior mapping). Every room has the same face slots; faceRoles[slot]
// says which side of the room box a slot shows, so faces per room and their order come from data.
// faceLayers are ordered room * FacesPerRoom() + slot (one texture-array layer per face); a box side
// without a slot shows the room's mean radiance. faceMean[layer] = mean linear radiance of a face.
// Variant choice per window: styleWeights[material style][room] (rows need not be normalised; the
// material style is clamped to the row count). roomLight[room] = (brightness when occupied, chance
// of being on while the window is otherwise unoccupied, brightness when on that way).
// Data beyond the engine limits is clamped with a warning by the uploader (GBufferPass).
struct InteriorAtlas {
	static constexpr int kMaxRooms = 512;          // shader room search is bounded by 2^10
	static constexpr int kMaxFacesPerRoom = 8;
	static constexpr int kMaxStyles = 64;

	enum class FaceRole : int { Back = 0, Left, Right, Ceiling, Floor, Count };
	// "back", "left", "right", "ceiling", "floor" (case-sensitive).
	static bool FaceRoleFromName(const std::string& name, FaceRole& out) {
		static const char* kNames[] = { "back", "left", "right", "ceiling", "floor" };
		for (int i = 0; i < static_cast<int>(FaceRole::Count); ++i) {
			if (name == kNames[i]) { out = static_cast<FaceRole>(i); return true; }
		}
		return false;
	}

	// Room box proportions around a pane (metres): width = pane width + 2 * sideMargin, randomised
	// per window within [min, max]; floor-to-ceiling height in [min, max]; depth = material depth
	// * (1 +- depthJitter); floor below the pane = material sill * [sillScale min, max].
	struct RoomShape {
		glm::vec2 sideMargin{ 1.0f, 1.4f };
		glm::vec2 height{ 2.8f, 3.2f };
		float depthJitter = 0.1f;
		glm::vec2 sillScale{ 1.0f, 1.0f };
	};
	// Default UV layout of window panes for materials that do not state one: the pane occupies
	// [cell / grid + inset, + extent] of a grid x grid tiling of TEXCOORD_0 (v down). grid 0 (default)
	// infers the layout per pane, assuming the pane is centred in its cell.
	struct PaneUV {
		float grid = 0.0f;
		float inset = 0.0f;
		float extent = 1.0f;
	};

	std::vector<FaceRole> faceRoles;                   // per face slot; size = faces per room
	std::vector<TextureArrayBuilder::Layer> faceLayers;
	TextureArrayBuilder::Options textureOptions;
	std::vector<glm::vec3> faceMean;                   // per face layer
	std::vector<glm::vec3> roomLight;                  // per room, see above
	std::vector<std::vector<float>> styleWeights;      // [style][room]
	RoomShape roomShape;
	PaneUV paneUV;
	float farDistance = 0.0f;   // default distance (m) beyond which panes use the flat mean (0 = never)

	int FacesPerRoom() const { return static_cast<int>(faceRoles.size()); }
	int RoomCount() const { return faceRoles.empty() ? 0 : static_cast<int>(faceLayers.size() / faceRoles.size()); }
};

struct SceneRenderExtras {
	std::vector<DecalSet> decalSets;
	LensFlareSet lensFlares;
	InteriorAtlas interiorAtlas;
	uint64_t revision = 0;   // bumped whenever the content is replaced (GPU re-upload)
};
