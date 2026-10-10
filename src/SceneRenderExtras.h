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
*                   five-face rooms behind window panes
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

// Rooms of five faces each; faceLayers are ordered room * 5 + face with faces back, left, right,
// ceiling, floor (orientation as in interior_mapping.glsl). Variant choice per window is driven by
// styleWeights[materialStyle][room]; roomLight[room] = (brightness when occupied, chance of being
// on while the window is otherwise unoccupied, brightness when on that way).
struct InteriorAtlas {
	static constexpr int kMaxRooms = 8;
	static constexpr int kMaxStyles = 4;
	std::vector<TextureArrayBuilder::Layer> faceLayers;
	TextureArrayBuilder::Options textureOptions;
	std::vector<glm::vec3> faceMean;                   // mean linear radiance per face layer
	std::vector<glm::vec3> roomLight;                  // per room, see above
	std::vector<std::vector<float>> styleWeights;      // [style][room], need not be normalised
	int RoomCount() const { return static_cast<int>(faceLayers.size()) / 5; }
};

struct SceneRenderExtras {
	std::vector<DecalSet> decalSets;
	LensFlareSet lensFlares;
	InteriorAtlas interiorAtlas;
	uint64_t revision = 0;   // bumped whenever the content is replaced (GPU re-upload)
};
