#include "ProceduralStreetKit.h"
#include "RoadNetwork.h"
#include "NightStreetPacks.h"
#include "stb_image_write.h"
#include "stb_image_write.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

	// ------------------------------------------------------------------------------------------
	// Materials
	// ------------------------------------------------------------------------------------------
	struct MatDef {
		std::string name;
		std::string texSet;          // Folder under texture_root; empty = factors only
		float tile = 1.0f;           // Real-world size of one texture tile in metres
		glm::vec3 baseColor{ 1.0f };
		float roughness = 1.0f;
		float metallic = 0.0f;
		glm::vec3 emissive{ 0.0f };
		float emissiveStrength = 0.0f; // Luminance in cd/m^2 (photometric scenes)
		float normalScale = 1.0f;
		float wetness = 0.0f;
		float puddle = 0.0f;
		float rainBeads = 0.0f;
		bool doubleSided = false;
		std::string emissiveTexture;  // generated image (path relative to the working directory)
		json interior;                // nox.interior extras (window interior mapping); null = off
	};

	enum Mat : int {
		M_ROAD, M_ROAD_WORN, M_PAINT_WHITE, M_PAINT_YELLOW, M_CURB, M_SIDEWALK,
		M_BRICK, M_CONCRETE_FACADE, M_ROOF, M_WINDOW_DARK, M_WINDOW_WARM, M_WINDOW_COOL,
		M_WINDOW_DIM, M_SHOPFRONT, M_SIGN_BOX, M_NEON_PINK, M_NEON_CYAN, M_NEON_RED,
		M_NEON_PURPLE, M_NEON_AMBER, M_POLE, M_LAMP_SODIUM, M_LAMP_LED, M_TUNNEL_TILES,
		M_TUNNEL_CONCRETE, M_TUNNEL_STRIP, M_CONTAINER_RED, M_CONTAINER_BLUE,
		M_CONTAINER_GREY, M_GROUND, M_FENCE, M_COUNT
	};

	std::vector<MatDef> BuildMaterials() {
		std::vector<MatDef> m(M_COUNT);
		// Night asphalt albedo ~0.08-0.12; the CC0 albedo maps are scaled down toward that.
		m[M_ROAD] = { "street_asphalt", "asphalt_road", 2.0f, glm::vec3(0.75f), 1.0f, 0.0f, {}, 0, 1.0f, 1.0f, 1.0f };
		m[M_ROAD_WORN] = { "street_asphalt_worn", "asphalt_worn", 4.04f, glm::vec3(0.42f), 1.0f, 0.0f, {}, 0, 1.0f, 1.0f, 1.0f };
		m[M_PAINT_WHITE] = { "road_paint_white", "", 1.0f, glm::vec3(0.72f), 0.55f, 0.0f, {}, 0, 1.0f, 0.75f, 0.5f };
		m[M_PAINT_YELLOW] = { "road_paint_yellow", "", 1.0f, glm::vec3(0.78f, 0.52f, 0.06f), 0.55f, 0.0f, {}, 0, 1.0f, 0.75f, 0.5f };
		m[M_CURB] = { "curb_concrete", "concrete_wall", 1.15f, glm::vec3(0.55f), 1.0f, 0.0f, {}, 0, 1.0f, 1.0f, 0.25f };
		m[M_SIDEWALK] = { "sidewalk_concrete", "concrete_sidewalk", 1.825f, glm::vec3(0.6f), 1.0f, 0.0f, {}, 0, 1.0f, 1.0f, 0.55f };
		m[M_BRICK] = { "facade_brick", "brick_facade", 3.0f, glm::vec3(0.7f), 1.0f, 0.0f, {}, 0, 1.0f, 0.55f, 0.0f };
		m[M_CONCRETE_FACADE] = { "facade_concrete", "concrete_wall", 2.3f, glm::vec3(0.55f), 1.0f, 0.0f, {}, 0, 1.0f, 0.55f, 0.0f };
		m[M_ROOF] = { "roof_concrete", "concrete_wall", 2.3f, glm::vec3(0.35f), 1.0f, 0.0f, {}, 0, 1.0f, 0.8f, 0.3f };
		m[M_WINDOW_DARK] = { "window_dark_glass", "", 1.0f, glm::vec3(0.02f, 0.025f, 0.03f), 0.06f, 0.0f, {}, 0, 1.0f, 0.3f, 0.0f, 0.6f };
		m[M_WINDOW_WARM] = { "window_lit_warm", "", 1.0f, glm::vec3(0.03f), 0.1f, 0.0f, glm::vec3(1.0f, 0.74f, 0.45f), 18.0f, 1.0f, 0.3f, 0.0f, 0.6f };
		m[M_WINDOW_COOL] = { "window_lit_cool", "", 1.0f, glm::vec3(0.03f), 0.1f, 0.0f, glm::vec3(0.72f, 0.84f, 1.0f), 14.0f, 1.0f, 0.3f, 0.0f, 0.6f };
		m[M_WINDOW_DIM] = { "window_lit_dim", "", 1.0f, glm::vec3(0.03f), 0.1f, 0.0f, glm::vec3(1.0f, 0.82f, 0.62f), 5.0f, 1.0f, 0.3f, 0.0f, 0.6f };
		m[M_SHOPFRONT] = { "shopfront_glow", "", 1.0f, glm::vec3(0.03f), 0.08f, 0.0f, glm::vec3(1.0f, 0.86f, 0.7f), 40.0f, 1.0f, 0.3f, 0.0f, 0.6f };
		m[M_SIGN_BOX] = { "sign_housing", "", 1.0f, glm::vec3(0.04f), 0.45f, 0.0f, {}, 0, 1.0f, 0.6f, 0.0f, 0.3f };
		// Neon tubes: luminance in the hundreds-to-thousands of cd/m^2.
		m[M_NEON_PINK] = { "neon_pink", "", 1.0f, glm::vec3(0.1f), 0.3f, 0.0f, glm::vec3(1.0f, 0.12f, 0.55f), 1600.0f };
		m[M_NEON_CYAN] = { "neon_cyan", "", 1.0f, glm::vec3(0.1f), 0.3f, 0.0f, glm::vec3(0.1f, 0.85f, 1.0f), 1400.0f };
		m[M_NEON_RED] = { "neon_red", "", 1.0f, glm::vec3(0.1f), 0.3f, 0.0f, glm::vec3(1.0f, 0.08f, 0.04f), 1800.0f };
		m[M_NEON_PURPLE] = { "neon_purple", "", 1.0f, glm::vec3(0.1f), 0.3f, 0.0f, glm::vec3(0.55f, 0.2f, 1.0f), 1500.0f };
		m[M_NEON_AMBER] = { "neon_amber", "", 1.0f, glm::vec3(0.1f), 0.3f, 0.0f, glm::vec3(1.0f, 0.55f, 0.08f), 1500.0f };
		m[M_POLE] = { "pole_painted_metal", "metal_painted", 2.0f, glm::vec3(0.22f, 0.23f, 0.24f), 0.7f, 0.0f, {}, 0, 0.6f, 0.7f, 0.0f, 0.3f };
		// Lamp lenses: real HPS/LED lens luminance is 1e5+ cd/m^2; clamped here to stay well
		// inside RGB16F until the pre-exposed light buffer lands (docs/nfs2015 M2/M3).
		m[M_LAMP_SODIUM] = { "lamp_sodium_lens", "", 1.0f, glm::vec3(0.2f), 0.2f, 0.0f, glm::vec3(1.0f, 0.55f, 0.18f), 20000.0f };
		m[M_LAMP_LED] = { "lamp_led_lens", "", 1.0f, glm::vec3(0.2f), 0.2f, 0.0f, glm::vec3(1.0f, 0.9f, 0.8f), 20000.0f };
		m[M_TUNNEL_TILES] = { "tunnel_tiles", "tunnel_tiles", 1.27f, glm::vec3(0.8f), 1.0f, 0.0f, {}, 0, 1.0f, 0.25f, 0.0f };
		m[M_TUNNEL_CONCRETE] = { "tunnel_concrete", "concrete_wall", 2.3f, glm::vec3(0.35f), 1.0f, 0.0f, {}, 0, 1.0f, 0.2f, 0.0f };
		m[M_TUNNEL_STRIP] = { "tunnel_light_strip", "", 1.0f, glm::vec3(0.2f), 0.3f, 0.0f, glm::vec3(1.0f, 0.6f, 0.22f), 6000.0f };
		m[M_CONTAINER_RED] = { "container_red", "metal_painted", 2.0f, glm::vec3(0.45f, 0.08f, 0.05f), 0.8f, 0.0f, {}, 0, 1.0f, 0.7f, 0.0f, 0.2f };
		m[M_CONTAINER_BLUE] = { "container_blue", "metal_painted", 2.0f, glm::vec3(0.06f, 0.14f, 0.3f), 0.8f, 0.0f, {}, 0, 1.0f, 0.7f, 0.0f, 0.2f };
		m[M_CONTAINER_GREY] = { "container_grey", "metal_painted", 2.0f, glm::vec3(0.3f), 0.8f, 0.0f, {}, 0, 1.0f, 0.7f, 0.0f, 0.2f };
		m[M_GROUND] = { "ground_lot", "asphalt_worn", 4.04f, glm::vec3(0.45f), 1.0f, 0.0f, {}, 0, 1.0f, 1.0f, 0.8f };
		m[M_FENCE] = { "fence_metal", "", 1.0f, glm::vec3(0.35f), 0.45f, 1.0f, {}, 0, 1.0f, 0.6f, 0.0f, 0.2f };
		return m;
	}

	// ------------------------------------------------------------------------------------------
	// Geometry builder
	// ------------------------------------------------------------------------------------------
	struct MeshBuilder {
		std::vector<glm::vec3> pos;
		std::vector<glm::vec3> nrm;
		std::vector<glm::vec4> tan;
		std::vector<glm::vec2> uv;
		std::vector<uint32_t> idx;
	};

	class Kit {
	public:
		explicit Kit(const std::vector<MatDef>& materials) : m_materials(materials) {}

		// Planar quad. U,V span the face (half extents folded in); uvU/uvV are the texture axes.
		void Quad(int mat, const glm::vec3& center, const glm::vec3& U, const glm::vec3& V,
			const glm::vec3& N, const glm::vec3& texU, const glm::vec3& texV) {
			MeshBuilder& b = Builder(mat, center);
			glm::vec3 c[4] = { center - U - V, center + U - V, center + U + V, center - U + V };
			if (glm::dot(glm::cross(U, V), N) < 0.0f) {
				std::swap(c[1], c[3]);
			}
			const float tile = std::max(m_materials[mat].tile, 0.01f);
			const glm::vec3 T = glm::normalize(texU);
			// glTF: bitangent = cross(N, T) * w must point toward image "up" (decreasing v).
			const float w = glm::dot(glm::cross(N, T), -glm::normalize(texV)) >= 0.0f ? 1.0f : -1.0f;
			const uint32_t base = static_cast<uint32_t>(b.pos.size());
			for (const glm::vec3& p : c) {
				b.pos.push_back(p);
				b.nrm.push_back(N);
				b.tan.push_back(glm::vec4(T, w));
				b.uv.push_back(glm::vec2(glm::dot(p, texU), glm::dot(p, texV)) / tile);
			}
			b.idx.insert(b.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}

		// Planar quad mapped to an explicit UV rectangle (atlas tiles). U points to the image's right,
		// V to its top.
		void QuadUV(int mat, const glm::vec3& center, const glm::vec3& U, const glm::vec3& V,
			const glm::vec3& N, const glm::vec2& uvMin, const glm::vec2& uvMax) {
			MeshBuilder& b = Builder(mat, center);
			glm::vec3 c[4] = { center - U - V, center + U - V, center + U + V, center - U + V };
			glm::vec2 t[4] = { {uvMin.x, uvMax.y}, {uvMax.x, uvMax.y}, {uvMax.x, uvMin.y}, {uvMin.x, uvMin.y} };
			if (glm::dot(glm::cross(U, V), N) < 0.0f) {
				std::swap(c[1], c[3]);
				std::swap(t[1], t[3]);
			}
			const glm::vec3 T = glm::normalize(U);
			const float w = glm::dot(glm::cross(N, T), glm::normalize(V)) >= 0.0f ? 1.0f : -1.0f;
			const uint32_t base = static_cast<uint32_t>(b.pos.size());
			for (int i = 0; i < 4; ++i) {
				b.pos.push_back(c[i]);
				b.nrm.push_back(N);
				b.tan.push_back(glm::vec4(T, w));
				b.uv.push_back(t[i]);
			}
			b.idx.insert(b.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}

		// Horizontal rectangle at height y, facing +Y (or -Y when down = true).
		void Flat(int mat, float x0, float x1, float z0, float z1, float y, bool down = false) {
			const glm::vec3 c((x0 + x1) * 0.5f, y, (z0 + z1) * 0.5f);
			const glm::vec3 U((x1 - x0) * 0.5f, 0, 0);
			const glm::vec3 V(0, 0, (z1 - z0) * 0.5f);
			Quad(mat, c, U, V, glm::vec3(0, down ? -1.0f : 1.0f, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1));
		}

		// Axis-aligned box. faceMask bits: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.
		void Box(int mat, const glm::vec3& mn, const glm::vec3& mx, uint32_t faceMask = 0x37u) {
			const glm::vec3 c = (mn + mx) * 0.5f;
			const glm::vec3 h = (mx - mn) * 0.5f;
			const glm::vec3 down(0, -1, 0);
			if (faceMask & 1u)  Quad(mat, c + glm::vec3(h.x, 0, 0), glm::vec3(0, 0, h.z), glm::vec3(0, h.y, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), down);
			if (faceMask & 2u)  Quad(mat, c - glm::vec3(h.x, 0, 0), glm::vec3(0, 0, h.z), glm::vec3(0, h.y, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), down);
			if (faceMask & 4u)  Quad(mat, c + glm::vec3(0, h.y, 0), glm::vec3(h.x, 0, 0), glm::vec3(0, 0, h.z), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1));
			if (faceMask & 8u)  Quad(mat, c - glm::vec3(0, h.y, 0), glm::vec3(h.x, 0, 0), glm::vec3(0, 0, h.z), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1));
			if (faceMask & 16u) Quad(mat, c + glm::vec3(0, 0, h.z), glm::vec3(h.x, 0, 0), glm::vec3(0, h.y, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), down);
			if (faceMask & 32u) Quad(mat, c - glm::vec3(0, 0, h.z), glm::vec3(h.x, 0, 0), glm::vec3(0, h.y, 0), glm::vec3(0, 0, -1), glm::vec3(-1, 0, 0), down);
		}

		// Vertical cylinder (no caps), used for poles.
		void Cylinder(int mat, const glm::vec3& base, float height, float radius, int segments = 10) {
			MeshBuilder& b = Builder(mat, base);
			const float tile = std::max(m_materials[mat].tile, 0.01f);
			const uint32_t start = static_cast<uint32_t>(b.pos.size());
			for (int i = 0; i <= segments; ++i) {
				const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
				const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
				const glm::vec3 t(-std::sin(a), 0.0f, std::cos(a));
				for (int j = 0; j < 2; ++j) {
					const float y = j == 0 ? 0.0f : height;
					b.pos.push_back(base + n * radius + glm::vec3(0, y, 0));
					b.nrm.push_back(n);
					b.tan.push_back(glm::vec4(t, 1.0f));
					b.uv.push_back(glm::vec2(a * radius, -y) / tile);
				}
			}
			for (int i = 0; i < segments; ++i) {
				const uint32_t a0 = start + static_cast<uint32_t>(i) * 2u;
				const uint32_t a1 = a0 + 2u;
				b.idx.insert(b.idx.end(), { a0, a0 + 1, a1 + 1, a0, a1 + 1, a1 });
			}
		}

		const std::map<std::pair<int, int>, MeshBuilder>& Builders() const { return m_builders; }

		// false (default): 40 m chunks along Z only (street kit). true: 40 m XZ cells (district).
		void SetChunkXZ(bool enable) { m_chunkXZ = enable; }

	private:
		MeshBuilder& Builder(int mat, const glm::vec3& c) {
			// 40 m chunks along the street so frustum and shadow culling stay effective.
			int chunk = static_cast<int>(std::floor(c.z / 40.0f));
			if (m_chunkXZ) {
				const int cx = static_cast<int>(std::floor(c.x / 40.0f)) + 512;
				chunk = cx * 1024 + (chunk + 512);
			}
			return m_builders[{ mat, chunk }];
		}

		const std::vector<MatDef>& m_materials;
		std::map<std::pair<int, int>, MeshBuilder> m_builders;
		bool m_chunkXZ = false;
	};

	// ------------------------------------------------------------------------------------------
	// Lights (scene-schema JSON nodes)
	// ------------------------------------------------------------------------------------------
	json MakeLight(const std::string& name, const std::string& type, const glm::vec3& position,
		const glm::vec3& rotationDeg, const glm::vec3& color, float lumens, float range,
		float innerDeg, float outerDeg, bool shadows, float sourceRadius) {
		json node;
		node["type"] = "light";
		node["name"] = name;
		node["position"] = { position.x, position.y, position.z };
		node["rotation"] = { rotationDeg.x, rotationDeg.y, rotationDeg.z };
		json light;
		light["type"] = type;
		light["color"] = { color.r, color.g, color.b };
		light["luminous_power"] = lumens;   // lm; interpreted by photometric scenes
		light["intensity"] = 1.0f;          // legacy fallback
		light["range"] = range;
		light["source_radius"] = sourceRadius;
		light["castsShadows"] = shadows;
		if (type == "spot") {
			light["cutOff"] = innerDeg;
			light["outerCutOff"] = outerDeg;
		}
		node["light"] = light;
		return node;
	}

	// Approximate linear-sRGB chromaticity of common street sources (normalised to max 1).
	const glm::vec3 kSodiumHPS(1.0f, 0.56f, 0.2f);   // ~2000 K high-pressure sodium
	const glm::vec3 kLED4000(1.0f, 0.86f, 0.72f);    // ~4000 K LED
	const glm::vec3 kMetalHalide(0.9f, 0.95f, 1.0f); // cool floodlight

	// ------------------------------------------------------------------------------------------
	// Texture packs (NightStreetPacks): road paint decals, lens flare sources, window room atlas.
	// The generators fill the engine's generic SceneRenderExtras.
	// ------------------------------------------------------------------------------------------
	using NightStreetPacks::RoadDecal;

	struct PackSetup {
		std::string decalRoot;   // empty = no decal pack (geometry paint fallback)
		bool flares = false;
		bool rooms = false;
	};

	// Resolves the packs named by the generator params and installs their descriptors (textures,
	// materials, styles, room weights). A pack is used only when it is complete.
	PackSetup SetupPacks(const json& params, SceneRenderExtras& extras) {
		PackSetup setup;
		const std::string decals = params.value("decal_root", std::string("textures/road_decals"));
		DecalSet paint;
		if (NightStreetPacks::BuildRoadDecalSet(decals, paint)) {
			extras.decalSets.push_back(std::move(paint));
			setup.decalRoot = decals;
		}
		setup.flares = NightStreetPacks::BuildLensFlareSet(params.value("lens_flare_root", std::string("textures/lens_flares")), extras.lensFlares);
		extras.lensFlares.gain = std::max(params.value("lens_flare_gain", 1.0f), 0.0f);
		setup.rooms = NightStreetPacks::BuildInteriorAtlas(params.value("window_room_root", std::string("textures/window_rooms")), extras.interiorAtlas);
		return setup;
	}

	size_t DecalCount(const SceneRenderExtras& extras) {
		size_t n = 0;
		for (const DecalSet& set : extras.decalSets) n += set.instances.size();
		return n;
	}

	// Lens flare source at a lamp lens. The pivot sits just in front of the emitting face so the lens
	// surface itself never occludes it in the depth test.
	void AddFlare(SceneRenderExtras& extras, NightStreetPacks::FlareStyle style, const glm::vec3& lensCentre,
		const glm::vec3& facing, float luminance, float size, const glm::vec3& tint = glm::vec3(1.0f)) {
		if (extras.lensFlares.styles.empty()) return;
		LensFlareSource s;
		s.position = lensCentre + facing * 0.05f;
		s.style = style;
		s.facing = facing;
		s.luminance = luminance;
		s.size = size;
		s.tint = tint;
		extras.lensFlares.sources.push_back(s);
	}

	// Road paint as decals (textures/road_decals/README.md). Image "up" follows dir, image right is
	// cross(dir, +Y) (the driver's right when travelling along dir). Paint bounds come from the pack's
	// generation_prompts.json, so sizes refer to the painted area, not the padded canvas.
	class DecalWriter {
	public:
		DecalWriter(const PackSetup& packs, SceneRenderExtras& extras, uint32_t seed)
			: m_info(NightStreetPacks::LoadDecalPaintInfo(packs.decalRoot)),
			m_out(m_info && !extras.decalSets.empty() ? &extras.decalSets.back().instances : nullptr),
			m_rng(seed ^ 0xDECA15u) {}

		bool Active() const { return m_info != nullptr && m_out != nullptr; }

		static glm::vec3 RightOf(const glm::vec3& dir) { return glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f))); }

		// Whole image, scaled so its paint bounds measure paintL along dir and paintW across, with the
		// painted centre (or the arrow tail when anchorTail) on c.
		void Painted(RoadDecal layer, const glm::vec3& c, const glm::vec3& dir, float paintL, float paintW, bool anchorTail = false) {
			if (!m_info) return;
			const int i = static_cast<int>(layer);
			const glm::vec4 b = m_info->bounds[i];
			const glm::vec3 right = RightOf(dir);
			const float fullW = paintW / std::max(b.z - b.x, 1e-3f);
			const float fullL = paintL / std::max(b.w - b.y, 1e-3f);
			const float u0 = anchorTail ? m_info->tailU[i] : (b.x + b.z) * 0.5f;
			const float vMid = (b.y + b.w) * 0.5f;
			DecalInstance dcl;
			dcl.center = c - right * ((u0 - 0.5f) * fullW) + dir * ((vMid - 0.5f) * fullL);
			dcl.axisU = right * (fullW * 0.5f);
			dcl.axisV = dir * (fullL * 0.5f);
			dcl.layer = i;
			Wear(dcl);
			m_out->push_back(dcl);
		}

		// Arrow of paintL metres with its tail on the lane centre c; width keeps the image aspect.
		void Arrow(RoadDecal layer, const glm::vec3& c, const glm::vec3& dir, float paintL) {
			if (!m_info) return;
			const glm::vec4 b = m_info->bounds[static_cast<int>(layer)];
			Painted(layer, c, dir, paintL, paintL * (b.z - b.x) / std::max(b.w - b.y, 1e-3f), true);
		}

		// Continuous line from A to B. The segment textures are not seamless tiles: every piece maps a
		// different longitudinal window from inside the painted segment (padded ends excluded), and
		// consecutive pieces overlap slightly so the alpha-blended joins have no gaps.
		void Line(RoadDecal layer, const glm::vec3& A, const glm::vec3& B, float paintW) {
			if (!m_info) return;
			const glm::vec3 dv = B - A;
			const float L = glm::length(dv);
			if (L < 0.2f) return;
			const glm::vec3 dir = dv / L;
			const glm::vec3 right = RightOf(dir);
			const glm::vec4 b = m_info->bounds[static_cast<int>(layer)];
			const float margin = 0.08f * (b.w - b.y);
			const float vIn0 = b.y + margin, vIn1 = b.w - margin;
			const float pad = 0.3f * (b.z - b.x);
			const float quadW = paintW * (b.z - b.x + 2.0f * pad) / std::max(b.z - b.x, 1e-3f);
			const float overlap = 0.12f;
			float t = 0.0f;
			while (t < L - 0.05f) {
				const float seg = Uniform(2.2f, 3.4f);
				const float t0 = std::max(t - (t > 0.0f ? overlap : 0.0f), 0.0f);
				const float t1 = std::min(t + seg, L);
				// Same stretch for a short final piece: sample a proportionally shorter window.
				const float frac = Uniform(0.55f, 0.8f) * std::min((t1 - t0) / seg, 1.0f);
				const float span = frac * (vIn1 - vIn0);
				const float va = vIn0 + Uniform(0.0f, 1.0f) * (vIn1 - vIn0 - span);
				DecalInstance dcl;
				dcl.center = A + dir * ((t0 + t1) * 0.5f);
				dcl.axisU = right * (quadW * 0.5f);
				dcl.axisV = dir * ((t1 - t0) * 0.5f);
				dcl.uvRect = glm::vec4(b.x - pad, va, b.z + pad, va + span);
				dcl.layer = static_cast<int>(layer);
				Wear(dcl);
				m_out->push_back(dcl);
				t = t1;
			}
		}

		// Crossing across a road of `width` (across = cross(dir, +Y)), bars `length` long along dir.
		// Wider roads repeat the six-bar decal instead of stretching its bars: each copy holds six
		// bars and six gaps, bar width stays near 0.5 m.
		void Crosswalk(const glm::vec3& c, const glm::vec3& dir, float width, float length) {
			if (!m_info || width < 1.0f) return;
			const glm::vec3 right = RightOf(dir);
			const int copies = std::max(1, static_cast<int>(std::lround(width / 6.0f)));
			const float copyW = width / static_cast<float>(copies);
			for (int k = 0; k < copies; ++k) {
				const glm::vec3 cc = c + right * ((static_cast<float>(k) + 0.5f) * copyW - width * 0.5f);
				Painted(RoadDecal::CrosswalkWhite, cc, dir, length, copyW * 11.0f / 12.0f);
			}
		}

	private:
		float Uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(m_rng); }
		void Wear(DecalInstance& d) {
			const float coverage = Uniform(0.82f, 1.0f);
			d.tint = glm::vec4(glm::vec3(Uniform(0.78f, 1.0f)), coverage);
		}

		const NightStreetPacks::DecalPaintInfo* m_info;
		std::vector<DecalInstance>* m_out;
		std::mt19937 m_rng;   // separate stream: the layout RNG sequence is unchanged by decals
	};

	// ------------------------------------------------------------------------------------------
	// glTF writer
	// ------------------------------------------------------------------------------------------
	bool WriteGltf(const fs::path& gltfPath, const std::vector<MatDef>& materials, const Kit& kit,
		const fs::path& textureRoot, int& outTriangles, int& outPrimitives) {
		std::vector<uint8_t> bin;
		json doc;
		doc["asset"] = { {"version", "2.0"}, {"generator", "NOX ProceduralStreetKit"} };
		doc["extensionsUsed"] = json::array({ "KHR_materials_emissive_strength" });
		json bufferViews = json::array();
		json accessors = json::array();
		json meshes = json::array();
		json nodes = json::array();
		json sceneNodes = json::array();

		auto appendView = [&](const void* data, size_t bytes, int target) -> int {
			while (bin.size() % 4u) bin.push_back(0);
			const size_t offset = bin.size();
			bin.resize(offset + bytes);
			std::memcpy(bin.data() + offset, data, bytes);
			bufferViews.push_back({ {"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}, {"target", target} });
			return static_cast<int>(bufferViews.size()) - 1;
		};
		auto appendAccessor = [&](int view, int componentType, size_t count, const char* type) -> json& {
			accessors.push_back({ {"bufferView", view}, {"componentType", componentType}, {"count", count}, {"type", type} });
			return accessors.back();
		};

		// Images / textures: one image per unique file, one shared repeat sampler.
		std::error_code dirEc;
		fs::create_directories(gltfPath.parent_path(), dirEc);
		json images = json::array();
		json textures = json::array();
		std::map<std::string, int> textureIndexByPath;
		const fs::path outDir = gltfPath.parent_path();
		auto textureFor = [&](const std::string& set, const char* file) -> int {
			if (set.empty()) return -1;
			fs::path p = textureRoot / set / file;
			std::error_code ec;
			if (!fs::exists(p, ec)) {
				// Allow png fallback for normal maps.
				fs::path alt = p;
				alt.replace_extension(".png");
				if (!fs::exists(alt, ec)) return -1;
				p = alt;
			}
			const std::string key = p.generic_string();
			auto it = textureIndexByPath.find(key);
			if (it != textureIndexByPath.end()) return it->second;
			fs::path rel = fs::relative(fs::absolute(p), fs::absolute(outDir), ec);
			const std::string uri = ec ? fs::absolute(p).generic_string() : rel.generic_string();
			images.push_back({ {"uri", uri} });
			textures.push_back({ {"source", static_cast<int>(images.size()) - 1}, {"sampler", 0} });
			const int index = static_cast<int>(textures.size()) - 1;
			textureIndexByPath[key] = index;
			return index;
		};

		json gltfMaterials = json::array();
		for (const MatDef& m : materials) {
			json mat;
			mat["name"] = m.name;
			json pbr;
			pbr["baseColorFactor"] = { m.baseColor.r, m.baseColor.g, m.baseColor.b, 1.0f };
			pbr["metallicFactor"] = m.metallic;
			pbr["roughnessFactor"] = m.roughness;
			if (int t = textureFor(m.texSet, "albedo.jpg"); t >= 0) pbr["baseColorTexture"] = { {"index", t} };
			// Grayscale roughness map: G = roughness, B is ignored because metallicFactor is 0.
			if (int t = textureFor(m.texSet, "roughness.jpg"); t >= 0) pbr["metallicRoughnessTexture"] = { {"index", t} };
			mat["pbrMetallicRoughness"] = pbr;
			if (int t = textureFor(m.texSet, "normal.jpg"); t >= 0) mat["normalTexture"] = { {"index", t}, {"scale", m.normalScale} };
			if (int t = textureFor(m.texSet, "ao.jpg"); t >= 0) mat["occlusionTexture"] = { {"index", t}, {"strength", 1.0f} };
			if (!m.emissiveTexture.empty()) {
				std::error_code relEc;
				const fs::path rel = fs::relative(fs::absolute(m.emissiveTexture), fs::absolute(outDir), relEc);
				const std::string uri = relEc ? fs::absolute(m.emissiveTexture).generic_string() : rel.generic_string();
				auto it = textureIndexByPath.find(uri);
				int t = -1;
				if (it != textureIndexByPath.end()) {
					t = it->second;
				}
				else {
					images.push_back({ {"uri", uri} });
					textures.push_back({ {"source", static_cast<int>(images.size()) - 1}, {"sampler", 0} });
					t = static_cast<int>(textures.size()) - 1;
					textureIndexByPath[uri] = t;
				}
				mat["emissiveTexture"] = { {"index", t} };
			}
			if (m.emissiveStrength > 0.0f) {
				mat["emissiveFactor"] = { m.emissive.r, m.emissive.g, m.emissive.b };
				mat["extensions"]["KHR_materials_emissive_strength"] = { {"emissiveStrength", m.emissiveStrength} };
			}
			mat["doubleSided"] = m.doubleSided;
			mat["extras"]["nox"] = {
				{"wetnessResponse", m.wetness},
				{"puddleResponse", m.puddle},
				{"rainBeadResponse", m.rainBeads}
			};
			if (!m.interior.is_null()) mat["extras"]["nox"]["interior"] = m.interior;
			gltfMaterials.push_back(mat);
		}

		outTriangles = 0;
		outPrimitives = 0;
		for (const auto& [key, b] : kit.Builders()) {
			if (b.idx.empty()) continue;
			glm::vec3 mn(1e30f), mx(-1e30f);
			for (const glm::vec3& p : b.pos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }

			const int vPos = appendView(b.pos.data(), b.pos.size() * sizeof(glm::vec3), 34962);
			json& aPos = appendAccessor(vPos, 5126, b.pos.size(), "VEC3");
			aPos["min"] = { mn.x, mn.y, mn.z };
			aPos["max"] = { mx.x, mx.y, mx.z };
			const int iPos = static_cast<int>(accessors.size()) - 1;
			appendAccessor(appendView(b.nrm.data(), b.nrm.size() * sizeof(glm::vec3), 34962), 5126, b.nrm.size(), "VEC3");
			const int iNrm = static_cast<int>(accessors.size()) - 1;
			appendAccessor(appendView(b.tan.data(), b.tan.size() * sizeof(glm::vec4), 34962), 5126, b.tan.size(), "VEC4");
			const int iTan = static_cast<int>(accessors.size()) - 1;
			appendAccessor(appendView(b.uv.data(), b.uv.size() * sizeof(glm::vec2), 34962), 5126, b.uv.size(), "VEC2");
			const int iUv = static_cast<int>(accessors.size()) - 1;
			appendAccessor(appendView(b.idx.data(), b.idx.size() * sizeof(uint32_t), 34963), 5125, b.idx.size(), "SCALAR");
			const int iIdx = static_cast<int>(accessors.size()) - 1;

			json prim;
			prim["attributes"] = { {"POSITION", iPos}, {"NORMAL", iNrm}, {"TANGENT", iTan}, {"TEXCOORD_0", iUv} };
			prim["indices"] = iIdx;
			prim["material"] = key.first;
			const std::string meshName = materials[key.first].name + "_c" + std::to_string(key.second);
			meshes.push_back({ {"name", meshName}, {"primitives", json::array({ prim })} });
			nodes.push_back({ {"name", meshName}, {"mesh", static_cast<int>(meshes.size()) - 1} });
			sceneNodes.push_back(static_cast<int>(nodes.size()) - 1);
			outTriangles += static_cast<int>(b.idx.size() / 3);
			++outPrimitives;
		}

		const fs::path binPath = fs::path(gltfPath).replace_extension(".bin");
		doc["buffers"] = json::array({ { {"uri", binPath.filename().generic_string()}, {"byteLength", bin.size()} } });
		doc["bufferViews"] = bufferViews;
		doc["accessors"] = accessors;
		doc["materials"] = gltfMaterials;
		doc["meshes"] = meshes;
		doc["nodes"] = nodes;
		doc["scenes"] = json::array({ { {"nodes", sceneNodes} } });
		doc["scene"] = 0;
		if (!images.empty()) {
			doc["images"] = images;
			doc["textures"] = textures;
			// 10497 REPEAT, 9987 LINEAR_MIPMAP_LINEAR, 9729 LINEAR
			doc["samplers"] = json::array({ { {"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497} } });
		}

		std::error_code ec;
		fs::create_directories(gltfPath.parent_path(), ec);
		std::ofstream binOut(binPath, std::ios::binary);
		if (!binOut) return false;
		binOut.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
		std::ofstream jsonOut(gltfPath);
		if (!jsonOut) return false;
		jsonOut << doc.dump(1);
		return static_cast<bool>(jsonOut) && static_cast<bool>(binOut);
	}


	// ------------------------------------------------------------------------------------------
	// Generated emissive textures: lit window interiors (4x4 atlas) and a shop-interior strip.
	// Colours are sRGB; brightness classes come from the material's emissive strength.
	// ------------------------------------------------------------------------------------------
	struct Image8 {
		int w = 0, h = 0;
		std::vector<uint8_t> px;
		Image8(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 3, 0) {}
		void Set(int x, int y, const glm::vec3& c) {
			if (x < 0 || y < 0 || x >= w || y >= h) return;
			const size_t i = (static_cast<size_t>(y) * w + x) * 3;
			px[i] = static_cast<uint8_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
			px[i + 1] = static_cast<uint8_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
			px[i + 2] = static_cast<uint8_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		void Rect(int x0, int y0, int x1, int y1, const glm::vec3& c) {
			for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) Set(x, y, c);
		}
	};

	void DrawWindowTile(Image8& img, int ox, int oy, int size, int variant, std::mt19937& rng) {
		auto u01 = [&rng]() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
		const glm::vec3 warm(1.0f, 0.78f, 0.52f), cool(0.78f, 0.88f, 1.0f), red(0.95f, 0.45f, 0.35f);
		const int kind = variant % 8;
		const glm::vec3 base = kind == 1 || kind == 5 ? cool : (kind == 6 ? red : warm);
		for (int y = 0; y < size; ++y) {
			const float fy = static_cast<float>(y) / size;   // 0 = top of the window
			for (int x = 0; x < size; ++x) {
				const float fx = static_cast<float>(x) / size;
				glm::vec3 c = base * (0.55f + 0.45f * (1.0f - fy)); // ceiling-lit gradient
				switch (kind) {
				case 0: { // warm room with a lamp pool and furniture silhouette
					const float d = glm::length(glm::vec2(fx - 0.3f, fy - 0.35f));
					c = base * (0.35f + 0.75f * std::exp(-d * d * 9.0f));
					if (fy > 0.68f && fx > 0.45f && fx < 0.9f) c *= 0.25f;
					break; }
				case 1: // office: fluorescent rows
					c = base * (0.45f + ((static_cast<int>(fy * 6.0f) % 2 == 0 && fy < 0.4f) ? 0.6f : 0.0f));
					if (fy > 0.62f && static_cast<int>(fx * 5.0f) % 2 == 0) c *= 0.4f;
					break;
				case 2: // venetian blinds
					c *= (static_cast<int>(fy * 22.0f) % 2 == 0) ? 1.0f : 0.35f;
					break;
				case 3: // curtains with folds, partly drawn
					if (fx < 0.3f || fx > 0.72f) c *= 0.55f + 0.35f * std::sin(fx * 60.0f) * std::sin(fx * 60.0f);
					break;
				case 4: { // dark room, TV glow
					const float d = glm::length(glm::vec2(fx - 0.6f, fy - 0.6f));
					c = glm::vec3(0.35f, 0.55f, 1.0f) * (0.08f + 0.9f * std::exp(-d * d * 14.0f));
					break; }
				case 5: // cool apartment, plant silhouette
					if (glm::length(glm::vec2(fx - 0.2f, fy - 0.75f)) < 0.16f) c *= 0.2f;
					break;
				case 6: // red-lit room
					c *= 0.7f;
					break;
				default: { // single small lamp in a dim room
					const float d = glm::length(glm::vec2(fx - 0.75f, fy - 0.55f));
					c = warm * (0.12f + 0.9f * std::exp(-d * d * 40.0f));
					break; }
				}
				c *= 0.9f + 0.1f * u01();
				img.Set(ox + x, oy + y, c);
			}
		}
		// Frame and mullions.
		const glm::vec3 frame(0.02f);
		const int fw = std::max(size / 24, 2);
		img.Rect(ox, oy, ox + size, oy + fw, frame);
		img.Rect(ox, oy + size - fw, ox + size, oy + size, frame);
		img.Rect(ox, oy, ox + fw, oy + size, frame);
		img.Rect(ox + size - fw, oy, ox + size, oy + size, frame);
		img.Rect(ox + size / 2 - fw / 2, oy, ox + size / 2 + fw / 2, oy + size, frame);
		if (variant % 3 == 0) img.Rect(ox, oy + size / 3, ox + size, oy + size / 3 + fw, frame);
	}

	bool WriteWindowAtlas(const fs::path& path, uint32_t seed) {
		const int tile = 256;
		Image8 img(tile * 4, tile * 4);
		std::mt19937 rng(seed ^ 0x9e3779b9u);
		for (int i = 0; i < 16; ++i) DrawWindowTile(img, (i % 4) * tile, (i / 4) * tile, tile, i, rng);
		return stbi_write_png(path.string().c_str(), img.w, img.h, 3, img.px.data(), img.w * 3) != 0;
	}

	bool WriteShopInterior(const fs::path& path, uint32_t seed) {
		Image8 img(1024, 256);
		std::mt19937 rng(seed ^ 0x51ed270bu);
		auto u01 = [&rng]() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
		for (int y = 0; y < img.h; ++y) {
			for (int x = 0; x < img.w; ++x) {
				const float fy = static_cast<float>(y) / img.h;
				img.Set(x, y, glm::vec3(1.0f, 0.93f, 0.82f) * (0.55f + 0.45f * (1.0f - fy)));
			}
		}
		// Shelves with coloured goods, a counter, ceiling lights and a few silhouettes.
		for (int shelf = 0; shelf < 3; ++shelf) {
			const int y0 = 70 + shelf * 45;
			img.Rect(0, y0 + 26, img.w, y0 + 30, glm::vec3(0.25f, 0.22f, 0.2f));
			for (int x = 0; x < img.w; x += 10 + static_cast<int>(u01() * 14.0f)) {
				const glm::vec3 c(0.3f + 0.7f * u01(), 0.3f + 0.7f * u01(), 0.3f + 0.7f * u01());
				img.Rect(x, y0 + 6 + static_cast<int>(u01() * 8.0f), x + 7 + static_cast<int>(u01() * 6.0f), y0 + 26, c * 0.8f);
			}
		}
		img.Rect(0, 215, img.w, 256, glm::vec3(0.18f, 0.16f, 0.15f));
		for (int x = 40; x < img.w; x += 160) img.Rect(x, 6, x + 90, 14, glm::vec3(1.0f));
		for (int k = 0; k < 3; ++k) {
			const int cx = 120 + static_cast<int>(u01() * 780.0f);
			img.Rect(cx - 14, 120, cx + 14, 256, glm::vec3(0.05f));
			img.Rect(cx - 9, 96, cx + 9, 120, glm::vec3(0.05f));
		}
		img.Rect(0, 0, img.w, 4, glm::vec3(0.02f));
		img.Rect(0, 252, img.w, 256, glm::vec3(0.02f));
		for (int x = 0; x < img.w; x += 256) img.Rect(x, 0, x + 4, 256, glm::vec3(0.02f));
		return stbi_write_png(path.string().c_str(), img.w, img.h, 3, img.px.data(), img.w * 3) != 0;
	}

} // namespace

namespace ProceduralStreetKit {

	Result Generate(const json& params) {
		Result result;
		const uint32_t seed = params.value("seed", 2015u);
		const float length = std::clamp(params.value("length", 330.0f), 120.0f, 2000.0f);
		const std::string output = params.value("output", std::string("generated/nfs_street"));
		const fs::path textureRoot = params.value("texture_root", std::string("textures/cc0"));
		const float lampSpacing = std::clamp(params.value("streetlight_spacing", 28.0f), 8.0f, 100.0f);
		const bool neonLights = params.value("neon_lights", true);
		const bool withTunnel = params.value("tunnel", true);
		const bool withIndustrial = params.value("industrial", true);
		const int shadowedStreetlights = std::max(params.value("shadowed_streetlights", 2), 0);

		std::mt19937 rng(seed);
		auto uniform = [&rng](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
		auto chance = [&](float p) { return uniform(0.0f, 1.0f) < p; };

		std::vector<MatDef> materials = BuildMaterials();
		{
			// Lit windows sample a generated interior atlas; the three window materials are now
			// brightness classes (bright / medium / dim) over the same atlas.
			std::error_code ec;
			const fs::path outDir = fs::path(output).parent_path();
			fs::create_directories(outDir, ec);
			const fs::path atlasPath = outDir / "window_atlas.png";
			const fs::path shopPath = outDir / "shop_interior.png";
			if (WriteWindowAtlas(atlasPath, seed)) {
				for (int m : { M_WINDOW_WARM, M_WINDOW_COOL, M_WINDOW_DIM }) {
					materials[m].emissiveTexture = atlasPath.generic_string();
					if (materials[m].interior.is_null()) materials[m].emissive = glm::vec3(1.0f);
				}
			}
			if (WriteShopInterior(shopPath, seed)) {
				materials[M_SHOPFRONT].emissiveTexture = shopPath.generic_string();
				materials[M_SHOPFRONT].emissive = glm::vec3(1.0f);
			}
		}
		SceneRenderExtras& extras = result.renderExtras;
		const PackSetup packs = SetupPacks(params, extras);
		if (packs.rooms) {
			// Parallax rooms from textures/window_rooms behind the lit panes. The atlases carry their own
			// lighting and colour, so the emissive colour is near-neutral (brightness classes only).
			materials[M_WINDOW_WARM].interior = json{ {"depth", 4.5}, {"lighting", 0.85}, {"blinds", 0.45} };
			materials[M_WINDOW_COOL].interior = json{ {"depth", 4.5}, {"lighting", 0.8}, {"blinds", 0.5}, {"seed", 1.0}, {"style", NightStreetPacks::RoomOffice} };
			materials[M_WINDOW_DIM].interior = json{ {"depth", 4.0}, {"lighting", 0.6}, {"blinds", 0.6}, {"seed", 2.0} };
			materials[M_WINDOW_WARM].emissive = glm::vec3(1.0f, 0.95f, 0.88f);
			materials[M_WINDOW_COOL].emissive = glm::vec3(0.93f, 0.97f, 1.0f);
			materials[M_WINDOW_DIM].emissive = glm::vec3(1.0f, 0.93f, 0.85f);
		}
		DecalWriter decals(packs, extras, seed);
		Kit kit(materials);

		// Layout (metres). Road |x| < 6, curb 6..6.25 (0.15 high), sidewalk 6.25..10.5.
		const float roadHalf = 6.0f;
		const float curbOuter = 6.25f;
		const float sidewalkOuter = 10.5f;
		const float curbH = 0.15f;
		const float zStart = -30.0f;
		const float crossZ0 = 100.0f, crossZ1 = 114.0f;  // cross street
		const float tunnelZ0 = 170.0f, tunnelZ1 = withTunnel ? 240.0f : 170.0f;
		const float zEnd = length;
		const float paintY = 0.004f;

		// --- Ground lot under everything (avoids a void beyond the blocks).
		kit.Flat(M_GROUND, -140.0f, 140.0f, zStart - 80.0f, zEnd + 80.0f, -0.02f);

		// --- Main road, split around the intersection.
		kit.Flat(M_ROAD, -roadHalf, roadHalf, zStart, crossZ0, 0.0f);
		kit.Flat(M_ROAD, -roadHalf, roadHalf, crossZ0, crossZ1, 0.0f);
		kit.Flat(M_ROAD, -roadHalf, roadHalf, crossZ1, zEnd, 0.0f);
		// Cross street (worn asphalt).
		kit.Flat(M_ROAD_WORN, -80.0f, -roadHalf, crossZ0, crossZ1, 0.0f);
		kit.Flat(M_ROAD_WORN, roadHalf, 80.0f, crossZ0, crossZ1, 0.0f);

		// --- Road markings (US style: double yellow centre, dashed white lanes, white edges).
		auto paintRun = [&](int mat, float x, float w, float z0, float z1, float dash, float gap) {
			for (float z = z0; z < z1; z += dash + gap) {
				kit.Flat(mat, x - w * 0.5f, x + w * 0.5f, z, std::min(z + dash, z1), paintY);
			}
		};
		auto markingsFor = [&](float z0, float z1) {
			paintRun(M_PAINT_YELLOW, -0.12f, 0.12f, z0, z1, 1e6f, 0.0f);
			paintRun(M_PAINT_YELLOW, 0.12f, 0.12f, z0, z1, 1e6f, 0.0f);
			paintRun(M_PAINT_WHITE, -3.0f, 0.12f, z0, z1, 3.0f, 9.0f);
			paintRun(M_PAINT_WHITE, 3.0f, 0.12f, z0, z1, 3.0f, 9.0f);
			paintRun(M_PAINT_WHITE, -roadHalf + 0.3f, 0.15f, z0, z1, 1e6f, 0.0f);
			paintRun(M_PAINT_WHITE, roadHalf - 0.3f, 0.15f, z0, z1, 1e6f, 0.0f);
		};
		// Decal version of the same layout (textures/road_decals): one double-yellow footprint over both
		// centre stripes, explicitly spaced lane dashes, edge lines, stop bars on the approach lanes,
		// tiled crossings and lane arrows ahead of the junction.
		const float decalY = 0.005f;
		auto decalMarkingsFor = [&](float z0, float z1) {
			const glm::vec3 fwd(0.0f, 0.0f, 1.0f);
			decals.Line(RoadDecal::LineDoubleYellow, glm::vec3(0.0f, decalY, z0), glm::vec3(0.0f, decalY, z1), 0.36f);
			for (float x : { -3.0f, 3.0f }) {
				for (float z = z0; z + 3.0f <= z1; z += 12.0f) {
					decals.Painted(RoadDecal::LaneDashWhite, glm::vec3(x, decalY, z + 1.5f), fwd, 3.0f, 0.13f);
				}
			}
			for (float x : { -roadHalf + 0.3f, roadHalf - 0.3f }) {
				decals.Line(RoadDecal::LineSingleWhite, glm::vec3(x, decalY, z0), glm::vec3(x, decalY, z1), 0.15f);
			}
		};
		if (decals.Active()) {
			decalMarkingsFor(zStart, crossZ0 - 4.0f);
			decalMarkingsFor(crossZ1 + 4.0f, zEnd);
			// Approach from -Z (travel +Z, lanes at x < 0) and from +Z (travel -Z, lanes at x > 0).
			for (float s : { 1.0f, -1.0f }) {
				const glm::vec3 dir(0.0f, 0.0f, s);
				const float zStop = s > 0.0f ? crossZ0 - 3.4f : crossZ1 + 3.4f;
				const float laneSign = -s;   // driver's right side is -X when travelling +Z
				decals.Painted(RoadDecal::StopBarWhite, glm::vec3(laneSign * (roadHalf * 0.5f), decalY, zStop), dir, 0.4f, roadHalf - 0.45f);
				const float zArrow = zStop - s * 6.5f;
				decals.Arrow(RoadDecal::ArrowStraightLeft, glm::vec3(laneSign * 1.5f, decalY, zArrow), dir, 3.6f);
				decals.Arrow(RoadDecal::ArrowStraightRight, glm::vec3(laneSign * 4.5f, decalY, zArrow), dir, 3.6f);
				decals.Arrow(RoadDecal::ArrowStraight, glm::vec3(laneSign * 1.5f, decalY, zArrow - s * 30.0f), dir, 3.6f);
				decals.Arrow(RoadDecal::ArrowStraight, glm::vec3(laneSign * 4.5f, decalY, zArrow - s * 30.0f), dir, 3.6f);
			}
			decals.Crosswalk(glm::vec3(0.0f, decalY, crossZ0 - 1.7f), glm::vec3(0.0f, 0.0f, 1.0f), 2.0f * roadHalf - 1.0f, 2.6f);
			decals.Crosswalk(glm::vec3(0.0f, decalY, crossZ1 + 1.7f), glm::vec3(0.0f, 0.0f, 1.0f), 2.0f * roadHalf - 1.0f, 2.6f);
		}
		else {
			markingsFor(zStart, crossZ0 - 4.0f);
			markingsFor(crossZ1 + 4.0f, zEnd);
			// Stop lines and crosswalks.
			kit.Flat(M_PAINT_WHITE, -roadHalf + 0.3f, 0.0f, crossZ0 - 3.6f, crossZ0 - 3.2f, paintY);
			kit.Flat(M_PAINT_WHITE, 0.0f, roadHalf - 0.3f, crossZ1 + 3.2f, crossZ1 + 3.6f, paintY);
			for (float x = -roadHalf + 0.5f; x < roadHalf - 0.5f; x += 1.0f) {
				kit.Flat(M_PAINT_WHITE, x, x + 0.5f, crossZ0 - 3.0f, crossZ0 - 0.4f, paintY);
				kit.Flat(M_PAINT_WHITE, x, x + 0.5f, crossZ1 + 0.4f, crossZ1 + 3.0f, paintY);
			}
		}

		// --- Curbs and sidewalks.
		auto sidewalkRun = [&](float z0, float z1) {
			for (int side = -1; side <= 1; side += 2) {
				const float s = static_cast<float>(side);
				const float xi = s * roadHalf, xc = s * curbOuter, xo = s * sidewalkOuter;
				kit.Box(M_CURB, glm::vec3(std::min(xi, xc), 0.0f, z0), glm::vec3(std::max(xi, xc), curbH, z1),
					(side < 0 ? 1u : 2u) | 4u);
				kit.Flat(M_SIDEWALK, std::min(xc, xo), std::max(xc, xo), z0, z1, curbH);
			}
		};
		sidewalkRun(zStart, crossZ0 - 4.0f);
		sidewalkRun(crossZ1 + 4.0f, tunnelZ0);
		// Cross-street sidewalks (cover the intersection corners).
		for (int side = -1; side <= 1; side += 2) {
			const float s = static_cast<float>(side);
			const float xa = s * roadHalf, xb = s * 80.0f;
			kit.Flat(M_SIDEWALK, std::min(xa, xb), std::max(xa, xb), crossZ0 - 4.0f, crossZ0 - 0.25f, curbH);
			kit.Flat(M_SIDEWALK, std::min(xa, xb), std::max(xa, xb), crossZ1 + 0.25f, crossZ1 + 4.0f, curbH);
			kit.Box(M_CURB, glm::vec3(std::min(xa, xb), 0.0f, crossZ0 - 0.25f), glm::vec3(std::max(xa, xb), curbH, crossZ0), 16u | 4u);
			kit.Box(M_CURB, glm::vec3(std::min(xa, xb), 0.0f, crossZ1), glm::vec3(std::max(xa, xb), curbH, crossZ1 + 0.25f), 32u | 4u);
		}

		// --- Buildings with windows and signage.
		int neonIndex = 0;
		auto building = [&](float xNear, float xFar, float z0, float z1, float height, int facadeMat) {
			const float xmn = std::min(xNear, xFar), xmx = std::max(xNear, xFar);
			const float facing = xNear < xFar ? -1.0f : 1.0f; // street-facing normal sign along X
			kit.Box(facadeMat, glm::vec3(xmn, 0.0f, z0), glm::vec3(xmx, height, z1), 0x3Fu & ~8u);
			kit.Box(M_ROOF, glm::vec3(xmn - 0.1f, height, z0 - 0.1f), glm::vec3(xmx + 0.1f, height + 0.6f, z1 + 0.1f), 0x3Fu & ~8u);

			// Ground floor: shopfront glazing band or dark glass.
			const float faceX = xNear + facing * 0.03f;
			const glm::vec3 N(facing, 0, 0);
			const glm::vec3 texU(0, 0, facing > 0 ? -1.0f : 1.0f);
			const float zLen = z1 - z0;
			if (chance(0.55f)) {
				const float shopU0 = uniform(0.0f, 0.5f);
				kit.QuadUV(M_SHOPFRONT, glm::vec3(faceX, 1.6f + curbH, (z0 + z1) * 0.5f),
					texU * (zLen * 0.5f - 1.2f), glm::vec3(0, 1.2f, 0), N,
					glm::vec2(shopU0, 0.0f), glm::vec2(shopU0 + std::min((zLen - 2.4f) / 12.0f, 0.5f), 1.0f));
			}
			else {
				kit.Quad(M_WINDOW_DARK, glm::vec3(faceX, 1.6f + curbH, (z0 + z1) * 0.5f),
					glm::vec3(0, 0, zLen * 0.5f - 1.2f), glm::vec3(0, 1.2f, 0), N, texU, glm::vec3(0, -1, 0));
			}

			// Upper floors: window grid; lit fraction drops with height.
			for (float y = 5.0f; y + 2.2f < height - 0.5f; y += 3.4f) {
				for (float z = z0 + 1.8f; z + 1.6f < z1 - 0.6f; z += 3.2f) {
					const float r = uniform(0.0f, 1.0f);
					int mat = M_WINDOW_DARK;
					if (r < 0.14f) mat = M_WINDOW_WARM;
					else if (r < 0.2f) mat = M_WINDOW_COOL;
					else if (r < 0.32f) mat = M_WINDOW_DIM;
					if (mat == M_WINDOW_DARK) {
						kit.Quad(mat, glm::vec3(faceX, y + 1.0f, z + 0.8f), glm::vec3(0, 0, 0.8f), glm::vec3(0, 1.0f, 0),
							N, texU, glm::vec3(0, -1, 0));
					}
					else {
						// 1.6 x 2.0 m pane: the interior-mapping pane contract (interior_mapping.glsl).
						// Random atlas tile, inset slightly to avoid mip bleeding between tiles.
						const int tile = static_cast<int>(rng() % 16u);
						const glm::vec2 t0(static_cast<float>(tile % 4) * 0.25f + 0.004f, static_cast<float>(tile / 4) * 0.25f + 0.004f);
						kit.QuadUV(mat, glm::vec3(faceX, y + 1.0f, z + 0.8f), texU * 0.8f, glm::vec3(0, 1.0f, 0), N,
							t0, t0 + glm::vec2(0.242f));
					}
				}
			}

			// Neon sign + small coloured fill light in front of it.
			if (chance(0.6f)) {
				static const int neon[] = { M_NEON_PINK, M_NEON_CYAN, M_NEON_RED, M_NEON_PURPLE, M_NEON_AMBER };
				static const glm::vec3 neonColor[] = {
					{1.0f, 0.12f, 0.55f}, {0.1f, 0.85f, 1.0f}, {1.0f, 0.08f, 0.04f}, {0.55f, 0.2f, 1.0f}, {1.0f, 0.55f, 0.08f} };
				const int k = static_cast<int>(rng() % 5u);
				const float w = std::min(uniform(1.6f, 3.8f), zLen * 0.4f);
				const float zc = uniform(z0 + w * 0.5f + 0.5f, z1 - w * 0.5f - 0.5f);
				const float y0 = 3.4f + curbH;
				const float box = facing * 0.25f;
				kit.Box(M_SIGN_BOX, glm::vec3(std::min(xNear, xNear + box), y0, zc - w * 0.5f),
					glm::vec3(std::max(xNear, xNear + box), y0 + 1.0f, zc + w * 0.5f), facing > 0 ? 1u | 4u | 8u | 16u | 32u : 2u | 4u | 8u | 16u | 32u);
				// Two tube strokes on the sign face.
				const float fx = xNear + box + facing * 0.02f;
				kit.Box(neon[k], glm::vec3(fx - 0.03f, y0 + 0.22f, zc - w * 0.42f), glm::vec3(fx + 0.03f, y0 + 0.3f, zc + w * 0.42f));
				kit.Box(neon[k], glm::vec3(fx - 0.03f, y0 + 0.62f, zc - w * 0.3f), glm::vec3(fx + 0.03f, y0 + 0.7f, zc + w * 0.3f));
				if (neonLights) {
					result.lightNodes.push_back(MakeLight("neon_fill_" + std::to_string(neonIndex++), "point",
						glm::vec3(xNear + facing * 1.2f, y0 + 0.5f, zc), glm::vec3(0.0f), neonColor[k], 900.0f, 9.0f,
						0, 0, false, 0.4f));
				}
			}
		};

		auto buildingRow = [&](float side, float z0, float z1) {
			float z = z0;
			while (z < z1 - 6.0f) {
				const float w = std::min(uniform(10.0f, 24.0f), z1 - z);
				const float h = uniform(10.0f, 38.0f);
				const float depth = uniform(12.0f, 18.0f);
				const int facade = chance(0.5f) ? M_BRICK : M_CONCRETE_FACADE;
				building(side * sidewalkOuter, side * (sidewalkOuter + depth), z, z + w - 0.4f, h, facade);
				z += w;
			}
		};
		for (float side : { -1.0f, 1.0f }) {
			buildingRow(side, zStart, crossZ0 - 4.0f);
			buildingRow(side, crossZ1 + 4.0f, tunnelZ0);
		}
		// Cross-street frontage buildings (face the cross street along Z).
		for (float side : { -1.0f, 1.0f }) {
			for (float x = 30.0f; x < 78.0f; x += 16.0f) {
				const float xa = side * x, xb = side * (x + 14.0f);
				const float h = uniform(10.0f, 30.0f);
				kit.Box(chance(0.5f) ? M_BRICK : M_CONCRETE_FACADE, glm::vec3(std::min(xa, xb), 0.0f, crossZ0 - 20.0f),
					glm::vec3(std::max(xa, xb), h, crossZ0 - 4.0f), 0x3Fu & ~8u);
				kit.Box(chance(0.5f) ? M_BRICK : M_CONCRETE_FACADE, glm::vec3(std::min(xa, xb), 0.0f, crossZ1 + 4.0f),
					glm::vec3(std::max(xa, xb), h, crossZ1 + 20.0f), 0x3Fu & ~8u);
			}
		}

		// --- Streetlights: pole, arm, head with emissive lens and a downward spot light.
		int lampIndex = 0;
		auto streetlight = [&](float side, float z, bool led) {
			const float poleX = side * (curbOuter + 0.6f);
			const float poleH = 8.5f;
			kit.Cylinder(M_POLE, glm::vec3(poleX, curbH, z), poleH, 0.11f);
			const float armEnd = side * (roadHalf - 2.2f);
			kit.Box(M_POLE, glm::vec3(std::min(poleX, armEnd), poleH + curbH - 0.12f, z - 0.07f),
				glm::vec3(std::max(poleX, armEnd), poleH + curbH, z + 0.07f));
			const float headX0 = armEnd - side * 0.2f, headX1 = armEnd + side * 0.9f;
			const float headY = poleH + curbH - 0.32f;
			kit.Box(M_POLE, glm::vec3(std::min(headX0, headX1), headY, z - 0.28f),
				glm::vec3(std::max(headX0, headX1), headY + 0.25f, z + 0.28f));
			kit.Flat(led ? M_LAMP_LED : M_LAMP_SODIUM, std::min(headX0, headX1) + 0.05f, std::max(headX0, headX1) - 0.05f,
				z - 0.22f, z + 0.22f, headY - 0.01f, true);
			AddFlare(extras, led ? NightStreetPacks::FlareLED : NightStreetPacks::FlareSodium, glm::vec3((headX0 + headX1) * 0.5f, headY - 0.01f, z),
				glm::vec3(0.0f, -1.0f, 0.0f), materials[led ? M_LAMP_LED : M_LAMP_SODIUM].emissiveStrength, 1.0f);
			// Spot aimed down and slightly toward the road centre (-Z forward rotated by pitch/roll).
			const glm::vec3 lightPos((headX0 + headX1) * 0.5f, headY - 0.05f, z);
			const glm::vec3 rot(-90.0f, 0.0f, side * -12.0f);
			const bool shadows = lampIndex < shadowedStreetlights;
			result.lightNodes.push_back(MakeLight("streetlight_" + std::to_string(lampIndex), "spot", lightPos, rot,
				led ? kLED4000 : kSodiumHPS, led ? 14000.0f : 22000.0f, 30.0f, 38.0f, 68.0f, shadows, 0.25f));
			++lampIndex;
		};
		for (float z = 0.0f; z < tunnelZ0 - 4.0f; z += lampSpacing) {
			if (z > crossZ0 - 6.0f && z < crossZ1 + 6.0f) continue;
			const bool led = (z > crossZ0 - 40.0f && z < crossZ1 + 40.0f); // brighter LED block around the junction
			streetlight(1.0f, z, led);
			streetlight(-1.0f, z + lampSpacing * 0.5f, led);
		}

		// --- Tunnel.
		if (withTunnel) {
			const float wallX = 7.5f, ceilY = 6.5f;
			// Service walkways.
			for (int side = -1; side <= 1; side += 2) {
				const float s = static_cast<float>(side);
				kit.Box(M_CURB, glm::vec3(std::min(s * roadHalf, s * wallX), 0.0f, tunnelZ0),
					glm::vec3(std::max(s * roadHalf, s * wallX), curbH, tunnelZ1), (side < 0 ? 1u : 2u) | 4u);
				// Walls: tiles on the lower 3.5 m, concrete above.
				const glm::vec3 N(-s, 0, 0);
				const glm::vec3 texU(0, 0, s);
				const float zc = (tunnelZ0 + tunnelZ1) * 0.5f, zh = (tunnelZ1 - tunnelZ0) * 0.5f;
				kit.Quad(M_TUNNEL_TILES, glm::vec3(s * wallX, curbH + 1.675f, zc), glm::vec3(0, 0, zh), glm::vec3(0, 1.675f, 0), N, texU, glm::vec3(0, -1, 0));
				kit.Quad(M_TUNNEL_CONCRETE, glm::vec3(s * wallX, (3.5f + ceilY) * 0.5f, zc), glm::vec3(0, 0, zh), glm::vec3(0, (ceilY - 3.5f) * 0.5f, 0), N, texU, glm::vec3(0, -1, 0));
				// Solid mass beside the tunnel up to the ceiling.
				kit.Box(M_TUNNEL_CONCRETE, glm::vec3(std::min(s * wallX, s * 40.0f), 0.0f, tunnelZ0),
					glm::vec3(std::max(s * wallX, s * 40.0f), ceilY, tunnelZ1), 4u | 16u | 32u | (side < 0 ? 2u : 1u));
			}
			kit.Flat(M_TUNNEL_CONCRETE, -wallX, wallX, tunnelZ0, tunnelZ1, ceilY, true);
			// Mass above the tunnel (hillside block) with portal faces.
			kit.Box(M_TUNNEL_CONCRETE, glm::vec3(-40.0f, ceilY, tunnelZ0), glm::vec3(40.0f, 18.0f, tunnelZ1), 0x3Fu & ~8u);
			kit.Box(M_TUNNEL_CONCRETE, glm::vec3(-wallX, ceilY, tunnelZ0 - 0.6f), glm::vec3(wallX, ceilY + 1.2f, tunnelZ0), 32u | 8u);
			kit.Box(M_TUNNEL_CONCRETE, glm::vec3(-wallX, ceilY, tunnelZ1), glm::vec3(wallX, ceilY + 1.2f, tunnelZ1 + 0.6f), 16u | 8u);
			// Ceiling light strips and sodium fixtures every 9 m.
			int t = 0;
			for (float z = tunnelZ0 + 4.0f; z < tunnelZ1 - 2.0f; z += 9.0f) {
				for (int side = -1; side <= 1; side += 2) {
					const float x = static_cast<float>(side) * 4.6f;
					kit.Box(M_POLE, glm::vec3(x - 0.2f, ceilY - 0.12f, z - 0.7f), glm::vec3(x + 0.2f, ceilY, z + 0.7f), 0x3Fu & ~4u);
					kit.Flat(M_TUNNEL_STRIP, x - 0.15f, x + 0.15f, z - 0.62f, z + 0.62f, ceilY - 0.125f, true);
					AddFlare(extras, NightStreetPacks::FlareSodium, glm::vec3(x, ceilY - 0.125f, z), glm::vec3(0.0f, -1.0f, 0.0f),
						materials[M_TUNNEL_STRIP].emissiveStrength, 0.6f);
					result.lightNodes.push_back(MakeLight("tunnel_light_" + std::to_string(t++), "spot",
						glm::vec3(x, ceilY - 0.2f, z), glm::vec3(-90.0f, 0.0f, static_cast<float>(side) * -20.0f),
						kSodiumHPS, 6500.0f, 14.0f, 35.0f, 55.0f, false, 0.3f));
				}
			}
		}

		// --- Industrial strip after the tunnel: containers, fence, floodlights.
		if (withIndustrial && zEnd > tunnelZ1 + 20.0f) {
			sidewalkRun(tunnelZ1, zEnd);
			int flood = 0;
			for (int side = -1; side <= 1; side += 2) {
				const float s = static_cast<float>(side);
				// Fence posts and rails along the sidewalk edge.
				for (float z = tunnelZ1 + 2.0f; z < zEnd; z += 3.0f) {
					kit.Cylinder(M_FENCE, glm::vec3(s * (sidewalkOuter - 0.3f), curbH, z), 2.2f, 0.04f, 6);
				}
				kit.Box(M_FENCE, glm::vec3(s * (sidewalkOuter - 0.3f) - 0.03f, 2.2f, tunnelZ1 + 2.0f),
					glm::vec3(s * (sidewalkOuter - 0.3f) + 0.03f, 2.28f, zEnd));
				// Container stacks.
				for (float z = tunnelZ1 + 6.0f; z < zEnd - 8.0f; z += uniform(7.0f, 11.0f)) {
					const float x0 = s * uniform(14.0f, 18.0f);
					const int levels = 1 + static_cast<int>(rng() % 3u);
					for (int l = 0; l < levels; ++l) {
						const int mat = l % 3 == 0 ? M_CONTAINER_RED : (l % 3 == 1 ? M_CONTAINER_BLUE : M_CONTAINER_GREY);
						const float xa = x0, xb = x0 + s * 12.2f;
						kit.Box(mat, glm::vec3(std::min(xa, xb), l * 2.6f, z), glm::vec3(std::max(xa, xb), l * 2.6f + 2.59f, z + 2.44f), 0x3Fu & ~8u);
					}
				}
				// Tall floodlight masts.
				for (float z = tunnelZ1 + 25.0f; z < zEnd; z += 45.0f) {
					const float x = s * 12.5f;
					kit.Cylinder(M_POLE, glm::vec3(x, 0.0f, z), 14.0f, 0.18f);
					kit.Box(M_POLE, glm::vec3(x - 0.6f, 14.0f, z - 0.4f), glm::vec3(x + 0.6f, 14.5f, z + 0.4f));
					kit.Flat(M_LAMP_LED, x - 0.5f, x + 0.5f, z - 0.3f, z + 0.3f, 13.99f, true);
					AddFlare(extras, NightStreetPacks::FlareFlood, glm::vec3(x, 13.99f, z), glm::vec3(0.0f, -1.0f, 0.0f),
						materials[M_LAMP_LED].emissiveStrength, 1.4f, kMetalHalide);
					result.lightNodes.push_back(MakeLight("floodlight_" + std::to_string(flood++), "spot",
						glm::vec3(x, 13.9f, z), glm::vec3(-90.0f, 0.0f, s * -25.0f), kMetalHalide, 40000.0f, 45.0f, 30.0f, 60.0f,
						flood == 1, 0.35f));
				}
			}
		}

		// --- Reflection probes: one per street segment (box = street canyon up to the facades),
		// the junction, the tunnel and the industrial strip. Capture points sit at car-roof height.
		auto probe = [&](const glm::vec3& p, const glm::vec3& mn, const glm::vec3& mx, float blend) {
			result.reflectionProbes.push_back({
				{"position", { p.x, p.y, p.z }},
				{"box_min", { mn.x, mn.y, mn.z }},
				{"box_max", { mx.x, mx.y, mx.z }},
				{"blend_distance", blend} });
		};
		const float canyonHalf = sidewalkOuter + 0.5f;
		for (float z = zStart; z < crossZ0 - 4.0f; z += 30.0f) {
			const float z1 = std::min(z + 34.0f, crossZ0);
			probe(glm::vec3(0.0f, 1.6f, (z + z1) * 0.5f), glm::vec3(-canyonHalf, -1.0f, z - 2.0f), glm::vec3(canyonHalf, 30.0f, z1 + 2.0f), 3.0f);
		}
		probe(glm::vec3(0.0f, 1.6f, (crossZ0 + crossZ1) * 0.5f), glm::vec3(-40.0f, -1.0f, crossZ0 - 6.0f), glm::vec3(40.0f, 30.0f, crossZ1 + 6.0f), 4.0f);
		for (float z = crossZ1 + 4.0f; z < tunnelZ0; z += 30.0f) {
			const float z1 = std::min(z + 34.0f, tunnelZ0 + 2.0f);
			probe(glm::vec3(0.0f, 1.6f, (z + z1) * 0.5f), glm::vec3(-canyonHalf, -1.0f, z - 2.0f), glm::vec3(canyonHalf, 30.0f, z1 + 2.0f), 3.0f);
		}
		if (withTunnel) {
			for (float z = tunnelZ0; z < tunnelZ1; z += 24.0f) {
				const float z1 = std::min(z + 26.0f, tunnelZ1 + 1.0f);
				probe(glm::vec3(0.0f, 1.6f, (z + z1) * 0.5f), glm::vec3(-7.6f, -0.5f, z - 1.0f), glm::vec3(7.6f, 6.6f, z1 + 1.0f), 2.0f);
			}
		}
		if (withIndustrial && zEnd > tunnelZ1 + 20.0f) {
			for (float z = tunnelZ1; z < zEnd; z += 35.0f) {
				const float z1 = std::min(z + 38.0f, zEnd + 5.0f);
				probe(glm::vec3(0.0f, 1.6f, (z + z1) * 0.5f), glm::vec3(-30.0f, -1.0f, z - 2.0f), glm::vec3(30.0f, 30.0f, z1 + 2.0f), 4.0f);
			}
		}

		const fs::path gltfPath = fs::path(output + ".gltf");
		result.ok = WriteGltf(gltfPath, materials, kit, textureRoot, result.triangleCount, result.primitiveCount);
		result.gltfPath = gltfPath.generic_string();
		std::cout << "[ProceduralStreetKit] Generated " << result.gltfPath << " ok=" << result.ok
			<< " primitives=" << result.primitiveCount << " triangles=" << result.triangleCount
			<< " decals=" << DecalCount(extras) << " flares=" << extras.lensFlares.sources.size()
			<< " lights=" << result.lightNodes.size() << " probes=" << result.reflectionProbes.size() << std::endl;
		return result;
	}
}

// ==============================================================================================
// District generator: connected blocks, roads and clutter built from a RoadNetwork.
// ==============================================================================================
namespace {

	enum DMat : int {
		D_FAC_BRICK_RED = M_COUNT, D_FAC_BRICK_BROWN, D_FAC_CONCRETE, D_FAC_STUCCO_CREAM,
		D_FAC_STUCCO_TAN, D_FAC_PAINT_BLUE, D_FAC_PAINT_GREEN,
		D_SHUTTER, D_AWNING_RED, D_AWNING_GREEN, D_AWNING_CREAM, D_DOOR, D_ROOF_UNIT,
		D_ROAD_CLEAN, D_PAINT_WORN, D_STORM_DRAIN,
		D_WOOD, D_WIRE, D_PALM_TRUNK, D_PALM_FROND,
		D_SIGNAL_HOUSING, D_SIGNAL_RED, D_SIGNAL_GREEN, D_SIGNAL_AMBER,
		D_HYDRANT, D_BIN, D_CAR_WHITE, D_CAR_BLACK, D_CAR_SILVER, D_CAR_RED, D_CAR_BLUE,
		D_CAR_GLASS, D_TAIL_LAMP, D_WHEEL, D_SKYLINE, D_SKYLINE_DARK, D_HILL, D_WINDOW_ROOM_DARK,
		D_SHOP_ROOM_24, D_SHOP_ROOM_32, D_SHOP_ROOM_40, D_SHOP_ROOM_48, D_COUNT
	};

	MatDef DMatDef(const char* name, const char* set, float tile, glm::vec3 base, float rough, float metal = 0.0f,
		glm::vec3 emissive = glm::vec3(0.0f), float emStrength = 0.0f, float wet = 0.3f, float puddle = 0.0f,
		float rain = 0.0f, bool doubleSided = false) {
		MatDef m;
		m.name = name; m.texSet = set; m.tile = tile; m.baseColor = base; m.roughness = rough; m.metallic = metal;
		m.emissive = emissive; m.emissiveStrength = emStrength; m.wetness = wet; m.puddle = puddle;
		m.rainBeads = rain; m.doubleSided = doubleSided;
		return m;
	}

	// Street-kit materials plus the district set. Emissive values are cd/m^2; the window and
	// shopfront glow is deliberately low so most of the facade stays dark.
	std::vector<MatDef> BuildDistrictMaterials() {
		std::vector<MatDef> m = BuildMaterials();
		m.resize(D_COUNT);
		m[M_WINDOW_WARM].emissiveStrength = 4.5f;
		m[M_WINDOW_COOL].emissiveStrength = 3.0f;
		m[M_WINDOW_DIM].emissiveStrength = 1.8f;
		m[M_SHOPFRONT].emissiveStrength = 2.2f;
		m[M_SHOPFRONT].emissive = glm::vec3(1.0f, 0.82f, 0.62f);
		// Window interiors (shaders/includes/interior_mapping.glsl): parallax rooms behind the glass.
		// lighting = occupancy bias (0 dark, 1 lit); blinds = chance of blinds or curtains.
		m[M_WINDOW_WARM].interior = json{ {"depth", 4.5}, {"lighting", 0.8}, {"blinds", 0.5} };
		m[M_WINDOW_COOL].interior = json{ {"depth", 4.0}, {"lighting", 0.7}, {"blinds", 0.6}, {"seed", 1.0} };
		m[M_WINDOW_DIM].interior = json{ {"depth", 4.0}, {"lighting", 0.55}, {"blinds", 0.6}, {"seed", 2.0} };
		m[D_WINDOW_ROOM_DARK] = DMatDef("window_dark_room", "", 1.0f, glm::vec3(0.02f, 0.025f, 0.03f), 0.06f, 0.0f,
			glm::vec3(0.9f, 0.85f, 0.75f), 2.0f, 0.3f, 0.0f, 0.6f);
		m[D_WINDOW_ROOM_DARK].interior = json{ {"depth", 3.5}, {"lighting", 0.2}, {"blinds", 0.6}, {"seed", 3.0} };
		const glm::vec3 none(0.0f);
		m[D_FAC_BRICK_RED] = DMatDef("facade_brick_red", "brick_facade", 3.0f, glm::vec3(0.85f, 0.55f, 0.48f), 0.9f, 0.0f, none, 0.0f, 0.55f);
		m[D_FAC_BRICK_BROWN] = DMatDef("facade_brick_brown", "brick_facade", 3.0f, glm::vec3(0.62f, 0.5f, 0.42f), 0.9f, 0.0f, none, 0.0f, 0.55f);
		m[D_FAC_CONCRETE] = DMatDef("facade_concrete_grey", "concrete_wall", 2.3f, glm::vec3(0.6f, 0.6f, 0.62f), 0.95f, 0.0f, none, 0.0f, 0.5f);
		m[D_FAC_STUCCO_CREAM] = DMatDef("facade_stucco_cream", "concrete_wall", 2.3f, glm::vec3(0.82f, 0.76f, 0.62f), 0.9f, 0.0f, none, 0.0f, 0.5f);
		m[D_FAC_STUCCO_TAN] = DMatDef("facade_stucco_tan", "concrete_wall", 2.3f, glm::vec3(0.7f, 0.56f, 0.42f), 0.9f, 0.0f, none, 0.0f, 0.5f);
		m[D_FAC_PAINT_BLUE] = DMatDef("facade_paint_blue", "concrete_wall", 2.3f, glm::vec3(0.25f, 0.36f, 0.5f), 0.8f, 0.0f, none, 0.0f, 0.5f);
		m[D_FAC_PAINT_GREEN] = DMatDef("facade_paint_green", "concrete_wall", 2.3f, glm::vec3(0.22f, 0.38f, 0.3f), 0.8f, 0.0f, none, 0.0f, 0.5f);
		m[D_SHUTTER] = DMatDef("roller_shutter", "", 1.0f, glm::vec3(0.22f, 0.22f, 0.23f), 0.55f, 0.6f, none, 0.0f, 0.4f);
		m[D_AWNING_RED] = DMatDef("awning_red", "", 1.0f, glm::vec3(0.5f, 0.1f, 0.08f), 0.8f, 0.0f, none, 0.0f, 0.3f, 0.0f, 0.0f, true);
		m[D_AWNING_GREEN] = DMatDef("awning_green", "", 1.0f, glm::vec3(0.1f, 0.3f, 0.18f), 0.8f, 0.0f, none, 0.0f, 0.3f, 0.0f, 0.0f, true);
		m[D_AWNING_CREAM] = DMatDef("awning_cream", "", 1.0f, glm::vec3(0.3f, 0.26f, 0.2f), 0.8f, 0.0f, none, 0.0f, 0.3f, 0.0f, 0.0f, true);
		m[D_DOOR] = DMatDef("door_dark", "", 1.0f, glm::vec3(0.07f, 0.06f, 0.05f), 0.5f, 0.0f, none, 0.0f, 0.5f);
		m[D_ROOF_UNIT] = DMatDef("roof_unit", "", 1.0f, glm::vec3(0.25f, 0.26f, 0.27f), 0.6f, 0.5f, none, 0.0f, 0.5f);
		m[D_ROAD_CLEAN] = DMatDef("road_clean", "asphalt_road", 2.5f, glm::vec3(0.55f), 1.0f, 0.0f, none, 0.0f, 1.0f, 0.8f);
		m[D_PAINT_WORN] = DMatDef("paint_worn", "", 1.0f, glm::vec3(0.42f, 0.42f, 0.4f), 0.6f, 0.0f, none, 0.0f, 0.5f, 0.0f, 0.5f);
		m[D_STORM_DRAIN] = DMatDef("storm_drain", "", 1.0f, glm::vec3(0.04f), 0.5f, 0.6f, none, 0.0f, 0.6f, 0.6f);
		m[D_WOOD] = DMatDef("utility_pole_wood", "", 1.0f, glm::vec3(0.12f, 0.085f, 0.065f), 0.85f, 0.0f, none, 0.0f, 0.4f);
		m[D_WIRE] = DMatDef("overhead_wire", "", 1.0f, glm::vec3(0.02f), 0.5f, 0.0f, none, 0.0f, 0.2f);
		m[D_PALM_TRUNK] = DMatDef("palm_trunk", "", 1.0f, glm::vec3(0.22f, 0.17f, 0.11f), 0.85f, 0.0f, none, 0.0f, 0.3f);
		m[D_PALM_FROND] = DMatDef("palm_frond", "", 1.0f, glm::vec3(0.07f, 0.16f, 0.06f), 0.5f, 0.0f, none, 0.0f, 0.4f, 0.0f, 0.0f, true);
		m[D_SIGNAL_HOUSING] = DMatDef("signal_housing", "", 1.0f, glm::vec3(0.03f), 0.5f, 0.0f, none, 0.0f, 0.4f);
		m[D_SIGNAL_RED] = DMatDef("signal_red", "", 1.0f, glm::vec3(0.12f, 0.02f, 0.02f), 0.3f, 0.0f, glm::vec3(1.0f, 0.06f, 0.03f), 2500.0f);
		m[D_SIGNAL_GREEN] = DMatDef("signal_green", "", 1.0f, glm::vec3(0.02f, 0.12f, 0.04f), 0.3f, 0.0f, glm::vec3(0.1f, 1.0f, 0.35f), 2500.0f);
		m[D_SIGNAL_AMBER] = DMatDef("signal_amber", "", 1.0f, glm::vec3(0.12f, 0.08f, 0.01f), 0.3f, 0.0f, glm::vec3(1.0f, 0.6f, 0.05f), 2500.0f);
		m[D_HYDRANT] = DMatDef("hydrant_red", "", 1.0f, glm::vec3(0.6f, 0.08f, 0.04f), 0.5f, 0.0f, none, 0.0f, 0.4f);
		m[D_BIN] = DMatDef("bin_green", "", 1.0f, glm::vec3(0.05f, 0.12f, 0.09f), 0.6f, 0.3f, none, 0.0f, 0.4f);
		m[D_CAR_WHITE] = DMatDef("car_white", "", 1.0f, glm::vec3(0.75f, 0.75f, 0.74f), 0.3f, 0.2f, none, 0.0f, 0.8f, 0.0f, 0.8f);
		m[D_CAR_BLACK] = DMatDef("car_black", "", 1.0f, glm::vec3(0.02f, 0.02f, 0.025f), 0.25f, 0.0f, none, 0.0f, 0.8f, 0.0f, 0.8f);
		m[D_CAR_SILVER] = DMatDef("car_silver", "", 1.0f, glm::vec3(0.45f, 0.46f, 0.48f), 0.3f, 0.8f, none, 0.0f, 0.8f, 0.0f, 0.8f);
		m[D_CAR_RED] = DMatDef("car_red", "", 1.0f, glm::vec3(0.4f, 0.04f, 0.03f), 0.3f, 0.0f, none, 0.0f, 0.8f, 0.0f, 0.8f);
		m[D_CAR_BLUE] = DMatDef("car_blue", "", 1.0f, glm::vec3(0.05f, 0.1f, 0.22f), 0.3f, 0.0f, none, 0.0f, 0.8f, 0.0f, 0.8f);
		m[D_CAR_GLASS] = DMatDef("car_glass", "", 1.0f, glm::vec3(0.01f, 0.012f, 0.014f), 0.05f, 0.0f, none, 0.0f, 0.8f);
		m[D_TAIL_LAMP] = DMatDef("tail_lamp_red", "", 1.0f, glm::vec3(0.05f), 0.2f, 0.0f, glm::vec3(1.0f, 0.04f, 0.02f), 500.0f);
		m[D_WHEEL] = DMatDef("tyre_black", "", 1.0f, glm::vec3(0.02f), 0.8f, 0.0f, none, 0.0f, 0.2f);
		m[D_SKYLINE] = DMatDef("skyline_concrete", "concrete_wall", 6.0f, glm::vec3(0.2f, 0.2f, 0.22f), 1.0f, 0.0f, none, 0.0f, 0.2f);
		m[D_SKYLINE_DARK] = DMatDef("skyline_dark", "", 1.0f, glm::vec3(0.05f, 0.05f, 0.06f), 1.0f, 0.0f, none, 0.0f, 0.1f);
		m[D_HILL] = DMatDef("hill_silhouette", "", 1.0f, glm::vec3(0.04f, 0.06f, 0.04f), 1.0f, 0.0f, none, 0.0f, 0.1f);
		// Shopfront glazing with a parallax shop/cafe room (window_rooms pack). One material per glass
		// width: the interior shader needs the pane size, and every bay is one pane (one room per shop).
		const char* shopNames[4] = { "shop_room_2_4m", "shop_room_3_2m", "shop_room_4_0m", "shop_room_4_8m" };
		for (int k = 0; k < 4; ++k) {
			const float halfW = 1.2f + 0.4f * static_cast<float>(k);
			m[D_SHOP_ROOM_24 + k] = DMatDef(shopNames[k], "", 1.0f, glm::vec3(0.03f), 0.08f, 0.0f,
				glm::vec3(1.0f, 0.96f, 0.9f), 3.2f, 0.3f, 0.0f, 0.6f);
			m[D_SHOP_ROOM_24 + k].interior = json{ {"depth", 6.0}, {"lighting", 0.75}, {"blinds", 0.08}, {"style", NightStreetPacks::RoomShop},
				{"seed", 4.0 + k}, {"pane", json::array({ halfW, 1.1f })}, {"sill", 0.35} };
		}
		return m;
	}

	// Oriented box whose horizontal axes are a (along) and n (across); vertical is +Y.
	// Half extents: hA along a, hY up, hN along n. mask bits as in Kit::Box: 0 +a, 1 -a, 2 +Y, 3 -Y, 4 +n, 5 -n.
	void OrientedBox(Kit& kit, int mat, const glm::vec3& c, const glm::vec3& a, const glm::vec3& n,
		float hA, float hY, float hN, uint32_t mask = 0x37u) {
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const glm::vec3 A = a * hA, Y = up * hY, N = n * hN;
		if (mask & 1u)  kit.Quad(mat, c + A, N, Y, a, n, up);
		if (mask & 2u)  kit.Quad(mat, c - A, N, Y, -a, -n, up);
		if (mask & 4u)  kit.Quad(mat, c + Y, A, N, up, a, n);
		if (mask & 8u)  kit.Quad(mat, c - Y, A, N, -up, a, n);
		if (mask & 16u) kit.Quad(mat, c + N, A, Y, n, a, up);
		if (mask & 32u) kit.Quad(mat, c - N, A, Y, -n, -a, up);
	}

	// Thin four-sided prism between two points (wires, crossarms of any direction).
	void Stick(Kit& kit, int mat, const glm::vec3& A, const glm::vec3& B, float r) {
		const glm::vec3 dv = B - A;
		const float len = glm::length(dv);
		if (len < 1e-3f) return;
		const glm::vec3 dir = dv / len;
		glm::vec3 side = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(side) < 1e-4f) side = glm::vec3(1.0f, 0.0f, 0.0f);
		side = glm::normalize(side);
		const glm::vec3 up2 = glm::normalize(glm::cross(dir, side));
		const glm::vec3 mid = (A + B) * 0.5f;
		const glm::vec3 U = dir * (len * 0.5f);
		const glm::vec3 faces[4] = { side, up2, -side, -up2 };
		for (const glm::vec3& s : faces) {
			const glm::vec3 t = glm::normalize(glm::cross(s, dir));
			kit.Quad(mat, mid + s * r, U, t * r, s, dir, t);
		}
	}

	// Axis-aligned box on a facade plane: u along A, depth along the outward normal N.
	void FaceBox(Kit& kit, int mat, const glm::vec3& faceMid, const glm::vec3& A, const glm::vec3& N,
		float uLo, float uHi, float yLo, float yHi, float depth) {
		glm::vec3 mn(1e30f), mx(-1e30f);
		for (float u : { uLo, uHi }) for (float dpt : { 0.0f, depth }) for (float y : { yLo, yHi }) {
			const glm::vec3 p = faceMid + A * u + glm::vec3(0.0f, y, 0.0f) + N * dpt;
			mn = glm::min(mn, p);
			mx = glm::max(mx, p);
		}
		kit.Box(mat, mn, mx, 0x3Fu & ~8u);
	}

	struct DistrictNode { glm::vec2 p{ 0.0f }; float road = 0.0f; float outer = 0.0f; int degree = 0; };
	struct DistrictEdge {
		int a = 0, b = 0;
		glm::vec3 P{ 0.0f }, Q{ 0.0f }, d{ 0.0f }, n{ 0.0f };
		float L = 0.0f, hw = 7.5f;
		bool avenue = false, ventura = false;
	};

	float SegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
		const glm::vec2 ab = b - a;
		const float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
		return glm::length(p - (a + ab * t));
	}

} // namespace

namespace ProceduralStreetKit {

	Result GenerateDistrict(const json& params, const RoadNetwork& network) {
		Result result;
		if (network.Nodes().empty() || network.Edges().empty()) {
			std::cerr << "[ProceduralStreetKit] district: road network is empty" << std::endl;
			return result;
		}
		const uint32_t seed = params.value("seed", 2015u);
		const std::string output = params.value("output", std::string("generated/nfs_district"));
		const fs::path textureRoot = params.value("texture_root", std::string("textures/cc0"));
		const float avenueLampSpacing = std::clamp(params.value("streetlight_spacing", 30.0f), 8.0f, 100.0f);
		const float crossLampSpacing = std::clamp(params.value("cross_streetlight_spacing", 30.0f), 8.0f, 150.0f);
		const int shadowedLamps = std::max(params.value("shadowed_streetlights", 2), 0);
		const size_t maxProbes = static_cast<size_t>(std::clamp(params.value("max_reflection_probes", 46), 0, 48));

		const float curbH = 0.15f;      // curb height (sidewalk top)
		const float curbT = 0.15f;      // curb width
		const float paintY = 0.004f;    // road paint lift over asphalt
		const float decalY = 0.005f;    // decal overlay lift (DecalPass also applies a depth offset)
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const glm::vec2 spawn(-3.8f, -40.0f);  // player start: keep the start lane clear of parked cars
		const float sidewalkW = network.SidewalkWidth();
		const bool placeProps = params.value("props", true);   // CC0 prop instances (models/cc0)

		std::vector<MatDef> materials = BuildDistrictMaterials();
		{
			std::error_code ec;
			const fs::path outDir = fs::path(output).parent_path();
			fs::create_directories(outDir, ec);
			const std::string stem = fs::path(output).filename().string();
			const fs::path atlasPath = outDir / (stem + "_window_atlas.png");
			const fs::path shopPath = outDir / (stem + "_shop_interior.png");
			if (WriteWindowAtlas(atlasPath, seed)) {
				for (int m : { M_WINDOW_WARM, M_WINDOW_COOL, M_WINDOW_DIM }) {
					materials[m].emissiveTexture = atlasPath.generic_string();
					if (materials[m].interior.is_null()) materials[m].emissive = glm::vec3(1.0f);
				}
			}
			if (WriteShopInterior(shopPath, seed)) {
				materials[M_SHOPFRONT].emissiveTexture = shopPath.generic_string();
				materials[M_SHOPFRONT].emissive = glm::vec3(1.0f, 0.8f, 0.6f); // warm, restrained interiors
				for (int k = D_SHOP_ROOM_24; k <= D_SHOP_ROOM_48; ++k) materials[k].emissiveTexture = shopPath.generic_string();
			}
		}
		SceneRenderExtras& extras = result.renderExtras;
		const PackSetup packs = SetupPacks(params, extras);
		const bool roomAtlas = packs.rooms;
		if (roomAtlas) {
			// The atlases already carry warm/cool room light: keep the emissive colours near-neutral so the
			// tint is not applied twice; offices get the office-weighted variant set (style 2).
			materials[M_WINDOW_WARM].emissive = glm::vec3(1.0f, 0.95f, 0.88f);
			materials[M_WINDOW_COOL].emissive = glm::vec3(0.93f, 0.97f, 1.0f);
			materials[M_WINDOW_DIM].emissive = glm::vec3(1.0f, 0.93f, 0.85f);
			materials[D_WINDOW_ROOM_DARK].emissive = glm::vec3(1.0f, 0.96f, 0.92f);
			materials[M_WINDOW_COOL].interior["style"] = NightStreetPacks::RoomOffice;
		}
		DecalWriter decals(packs, extras, seed);
		Kit kit(materials);
		kit.SetChunkXZ(true);

		std::mt19937 rng(seed ^ 0xA5F00Du);
		auto uniform = [&rng](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
		auto chance = [&](float p) { return uniform(0.0f, 1.0f) < p; };
		auto pickInt = [&rng](int n) { return static_cast<int>(rng() % static_cast<uint32_t>(std::max(n, 1))); };
		auto W3 = [](const glm::vec2& p, float y) { return glm::vec3(p.x, y, p.y); };

		// ---- Obstacles, CC0 prop instances and helpers shared by the blocks below.
		json obstacles = json::array();
		auto circle = [&](const glm::vec3& p, float r) {
			obstacles.push_back({ {"type", "circle"}, {"pos", { p.x, p.z }}, {"radius", r} });
		};
		auto boxObstacle = [&](const glm::vec3& c, const glm::vec3& d, float hA, float hN) {
			obstacles.push_back({ {"type", "box"}, {"center", { c.x, c.z }}, {"half", { hA, hN }},
				{"yaw", std::atan2(d.z, d.x)} });
		};
		// Yaw (radians) that turns model +X onto the horizontal direction d (rotation about +Y).
		auto yawAlong = [](const glm::vec3& d) { return std::atan2(-d.z, d.x); };
		// Scene "model" node for a CC0 prop (models/cc0 is git-ignored; nothing is placed when the file is missing).
		int propIndex = 0;
		auto prop = [&](const char* model, const char* tag, const glm::vec3& base, float yaw) -> bool {
			if (!placeProps) return false;
			const std::string rel = std::string("models/cc0/") + model + ".glb";
			if (!fs::exists(rel)) return false;
			json n;
			n["type"] = "model";
			n["name"] = std::string(tag) + "_" + std::to_string(propIndex++);
			n["path"] = rel;
			n["position"] = { base.x, base.y, base.z };
			n["rotation"] = { 0.0f, glm::degrees(yaw), 0.0f };
			n["scale"] = { 1.0f, 1.0f, 1.0f };
			n["material_overrides"] = json::array({ json{ {"match", "*"}, {"wetness_response", 0.5f} } });
			result.propNodes.push_back(n);
			return true;
		};
		// Parked car: d = length axis (4.4 m), n = width axis (1.8 m), c = ground-contact centre.
		auto parkedCar = [&](const glm::vec3& c, const glm::vec3& d, const glm::vec3& n) {
			const int paint[] = { D_CAR_WHITE, D_CAR_BLACK, D_CAR_SILVER, D_CAR_RED, D_CAR_BLUE };
			const int body = paint[pickInt(5)];
			OrientedBox(kit, body, c + up * 0.6f, d, n, 2.2f, 0.3f, 0.9f, 0x3Fu & ~8u);
			OrientedBox(kit, D_CAR_GLASS, c + up * 1.2f, d, n, 1.2f, 0.3f, 0.82f, 0x33u);
			OrientedBox(kit, body, c + up * 1.5f, d, n, 1.2f, 0.03f, 0.8f, 0x04u);
			for (float sa : { -1.4f, 1.4f }) {
				for (float sn : { -0.8f, 0.8f }) {
					OrientedBox(kit, D_WHEEL, c + d * sa + n * sn + up * 0.16f, d, n, 0.32f, 0.16f, 0.12f);
				}
			}
			for (float sn : { -0.6f, 0.6f }) {
				kit.Quad(D_TAIL_LAMP, c - d * 2.21f + n * sn + up * 0.75f, n * 0.2f, up * 0.07f, -d, n, up);
			}
			boxObstacle(c, d, 2.25f, 0.95f);
		};

		// ---- Graph -> geometry records.
		const auto& netNodes = network.Nodes();
		std::vector<DistrictNode> nodes(netNodes.size());
		for (size_t i = 0; i < netNodes.size(); ++i) nodes[i].p = netNodes[i].pos;
		std::vector<DistrictEdge> edges;
		for (const RoadNetwork::Edge& e : network.Edges()) {
			const int nodeCount = static_cast<int>(netNodes.size());
			if (e.a < 0 || e.b < 0 || e.a >= nodeCount || e.b >= nodeCount) continue;
			DistrictEdge g;
			g.a = e.a;
			g.b = e.b;
			g.hw = e.halfWidth;
			g.P = W3(netNodes[e.a].pos, 0.0f);
			g.Q = W3(netNodes[e.b].pos, 0.0f);
			const glm::vec3 dv = g.Q - g.P;
			g.L = glm::length(dv);
			if (g.L < 2.0f) continue;
			g.d = dv / g.L;
			g.n = glm::vec3(-g.d.z, 0.0f, g.d.x);
			g.ventura = e.name.find("Ventura") != std::string::npos;
			g.avenue = g.ventura || e.name.find("Palm") != std::string::npos;
			nodes[e.a].degree++;
			nodes[e.b].degree++;
			nodes[e.a].road = std::max(nodes[e.a].road, g.hw);
			nodes[e.b].road = std::max(nodes[e.b].road, g.hw);
			edges.push_back(g);
		}
		for (DistrictNode& nd : nodes) nd.outer = nd.road + curbT + sidewalkW;

		// ---- Ground under everything.
		glm::vec2 mn(1e9f), mx(-1e9f);
		for (const DistrictNode& nd : nodes) { mn = glm::min(mn, nd.p); mx = glm::max(mx, nd.p); }
		kit.Flat(M_GROUND, mn.x - 600.0f, mx.x + 600.0f, mn.y - 600.0f, mx.y + 600.0f, -0.02f);

		// Horizontal strip from A to B over offsets [lo, hi] along n, at height y.
		auto strip = [&](int mat, const glm::vec3& A, const glm::vec3& B, const glm::vec3& n, float lo, float hi, float y) {
			const glm::vec3 dv = B - A;
			const float len = glm::length(dv);
			if (len < 0.05f || hi <= lo) return;
			const glm::vec3 d = dv / len;
			const glm::vec3 mid = (A + B) * 0.5f + n * ((lo + hi) * 0.5f) + glm::vec3(0.0f, y, 0.0f);
			kit.Quad(mat, mid, d * (len * 0.5f), n * ((hi - lo) * 0.5f), up, d, n);
		};
		auto rectFlat = [&](int mat, float x0, float x1, float z0, float z1, float y) {
			kit.Flat(mat, std::min(x0, x1), std::max(x0, x1), std::min(z0, z1), std::max(z0, z1), y);
		};

		// ---- Road surface, curbs, sidewalks.
		for (const DistrictEdge& e : edges) {
			const float rA = nodes[e.a].road, rB = nodes[e.b].road;
			const float oA = nodes[e.a].outer, oB = nodes[e.b].outer;
			strip(M_ROAD, e.P + e.d * rA, e.Q - e.d * rB, e.n, -e.hw, e.hw, 0.0f);
			const glm::vec3 SA = e.P + e.d * oA, SB = e.Q - e.d * oB;
			const float clen = glm::length(SB - SA);
			for (float s : { -1.0f, 1.0f }) {
				const float h0 = e.hw, h1 = e.hw + curbT, h2 = e.hw + curbT + sidewalkW;
				// Curb face toward the road.
				const glm::vec3 cm = (SA + SB) * 0.5f + e.n * (s * h0) + up * (curbH * 0.5f);
				if (clen > 0.05f) kit.Quad(M_CURB, cm, e.d * (clen * 0.5f), up * (curbH * 0.5f), -e.n * s, e.d, up);
				if (s > 0.0f) strip(M_SIDEWALK, SA, SB, e.n, h1, h2, curbH);
				else strip(M_SIDEWALK, SA, SB, e.n, -h2, -h1, curbH);
			}
		}
		// Intersections: clean asphalt square plus four sidewalk corners.
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].degree == 0) continue;
			const glm::vec3 c = W3(nodes[i].p, 0.0f);
			const float h = nodes[i].road, o = nodes[i].outer;
			rectFlat(D_ROAD_CLEAN, c.x - h, c.x + h, c.z - h, c.z + h, 0.0f);
			for (float sx : { -1.0f, 1.0f }) for (float sz : { -1.0f, 1.0f }) {
				rectFlat(M_SIDEWALK, c.x + sx * h, c.x + sx * o, c.z + sz * h, c.z + sz * o, curbH);
			}
		}

		// ---- Markings, patches and drains per edge; crosswalks and stop lines per approach.
		for (const DistrictEdge& e : edges) {
			const float rA = nodes[e.a].road, rB = nodes[e.b].road;
			auto M = [&](float t) { return e.P + e.d * t; };
			const float t0 = rA + 4.5f, t1 = e.L - rB - 4.5f;
			if (t1 > t0 + 1.0f) {
				// Decals (road_decals pack) replace the flat paint strips; the layout RNG calls stay the same.
				const glm::vec3 lift = up * decalY;
				if (decals.Active()) {
					decals.Line(RoadDecal::LineDoubleYellow, M(t0) + lift, M(t1) + lift, 0.42f);
				}
				else {
					strip(M_PAINT_YELLOW, M(t0), M(t1), e.n, -0.25f, -0.01f, paintY);
					strip(M_PAINT_YELLOW, M(t0), M(t1), e.n, 0.01f, 0.25f, paintY);
				}
				// Lane dividers (dashed) and edge lines broken into faded runs.
				for (float side : { -1.0f, 1.0f }) {
					const float off = side * e.hw * 0.5f;
					for (float t = t0; t < t1; t += 12.0f) {
						const float te = std::min(t + 3.0f, t1);
						if (chance(0.1f)) continue;
						const int mat = chance(0.25f) ? static_cast<int>(D_PAINT_WORN) : static_cast<int>(M_PAINT_WHITE);
						if (!decals.Active()) strip(mat, M(t), M(te), e.n, off - 0.07f, off + 0.07f, paintY);
						else if (te - t > 1.0f) decals.Painted(RoadDecal::LaneDashWhite, M((t + te) * 0.5f) + e.n * off + lift, e.d, te - t, 0.14f);
					}
					const float edgeOff = side * (e.hw - 0.35f);
					for (float t = t0; t < t1; t += 25.0f) {
						if (chance(0.15f)) continue;
						const float te = std::min(t + uniform(14.0f, 22.0f), t1);
						const int mat = chance(0.3f) ? static_cast<int>(D_PAINT_WORN) : static_cast<int>(M_PAINT_WHITE);
						if (!decals.Active()) strip(mat, M(t), M(te), e.n, edgeOff - 0.08f, edgeOff + 0.08f, paintY);
						else decals.Line(RoadDecal::LineSingleWhite, M(t) + e.n * edgeOff + lift, M(te) + e.n * edgeOff + lift, 0.16f);
					}
				}
			}
			// Worn asphalt patches and storm drains at the curbs.
			const int patches = std::max(1, static_cast<int>(e.L / 70.0f));
			for (int k = 0; k < patches; ++k) {
				const float ta = uniform(rA + 2.0f, std::max(rA + 2.5f, e.L - rB - 2.0f));
				const glm::vec3 c = M(ta) + e.n * uniform(-e.hw + 1.2f, e.hw - 1.2f) + up * 0.003f;
				kit.Quad(M_ROAD_WORN, c, e.d * uniform(1.5f, 4.0f), e.n * uniform(1.0f, 2.0f), up, e.d, e.n);
			}
			for (float t = t0 + 10.0f; t < t1 - 5.0f; t += 35.0f) {
				for (float side : { -1.0f, 1.0f }) {
					const glm::vec3 c = M(t) + e.n * (side * (e.hw - 0.75f)) + up * 0.0045f;
					kit.Quad(D_STORM_DRAIN, c, e.d * 0.35f, e.n * 0.25f, up, e.d, e.n);
				}
			}
		}
		for (const DistrictEdge& e : edges) {
			for (int endIdx = 0; endIdx < 2; ++endIdx) {
				const bool atA = endIdx == 0;
				const int node = atA ? e.a : e.b;
				if (nodes[node].degree < 2) continue;
				const float hN = nodes[node].road;
				const glm::vec3 dirOut = atA ? e.d : -e.d;
				const glm::vec3 base = atA ? e.P : e.Q;
				auto at = [&](float s) { return base + dirOut * s; };
				const float cwA = hN + 1.0f, cwB = hN + 3.4f;
				if (!decals.Active()) {
					for (float o = -e.hw + 0.5f; o < e.hw - 0.4f; o += 1.0f) {
						const glm::vec3 c = at((cwA + cwB) * 0.5f) + e.n * (o + 0.25f) + up * paintY;
						kit.Quad(M_PAINT_WHITE, c, dirOut * ((cwB - cwA) * 0.5f), e.n * 0.25f, up, dirOut, e.n);
					}
					const glm::vec3 sl = at(hN + 4.0f) + up * paintY;
					kit.Quad(M_PAINT_WHITE, sl, e.n * (e.hw - 0.3f), dirOut * 0.2f, up, e.n, dirOut);
					continue;
				}
				// Decals: crossing over the full width, stop bar and lane arrows on the approach lanes only
				// (right-hand traffic: arriving traffic travels -dirOut, its lanes lie on its right).
				const glm::vec3 lift = up * decalY;
				decals.Crosswalk(at((cwA + cwB) * 0.5f) + lift, dirOut, 2.0f * e.hw - 0.9f, cwB - cwA);
				const glm::vec3 travel = -dirOut;
				const glm::vec3 right = DecalWriter::RightOf(travel);
				decals.Painted(RoadDecal::StopBarWhite, at(hN + 4.0f) + right * ((e.hw - 0.15f) * 0.5f) + lift, travel, 0.4f, e.hw - 0.45f);
				const float arrowAt = hN + 7.2f;
				if (nodes[node].degree < 3 || arrowAt + 2.5f > e.L * 0.5f) continue;
				bool left = false, straight = false, rightTurn = false;
				for (const DistrictEdge& g : edges) {
					if (&g == &e || (g.a != node && g.b != node)) continue;
					const glm::vec3 o = g.a == node ? g.d : -g.d;
					if (glm::dot(o, travel) > 0.7f) straight = true;
					else if (glm::dot(o, right) > 0.5f) rightTurn = true;
					else if (glm::dot(o, right) < -0.5f) left = true;
				}
				const RoadDecal inner = left ? (straight ? RoadDecal::ArrowStraightLeft : RoadDecal::ArrowLeft)
					: (straight ? RoadDecal::ArrowStraight : RoadDecal::ArrowRight);
				const RoadDecal outer = rightTurn ? (straight ? RoadDecal::ArrowStraightRight : RoadDecal::ArrowRight)
					: (straight ? RoadDecal::ArrowStraight : RoadDecal::ArrowLeft);
				decals.Arrow(inner, at(arrowAt) + right * (e.hw * 0.25f) + lift, travel, 3.6f);
				decals.Arrow(outer, at(arrowAt) + right * (e.hw * 0.75f) + lift, travel, 3.6f);
			}
		}

		// ---- Block fill: 1 m raster of cells outside the drivable band, merged into lots.
		const float bandOuterEdge = curbT + sidewalkW;
		auto busyAt = [&](const glm::vec2& p) -> bool {
			for (const DistrictEdge& e : edges) {
				if (SegmentDistance(p, glm::vec2(e.P.x, e.P.z), glm::vec2(e.Q.x, e.Q.z)) < e.hw + bandOuterEdge + 0.5f) return true;
			}
			for (const DistrictNode& nd : nodes) {
				if (nd.degree == 0) continue;
				if (std::max(std::abs(p.x - nd.p.x), std::abs(p.y - nd.p.y)) < nd.outer + 0.5f) return true;
			}
			return false;
		};
		const float margin = 70.0f;
		const int gx0 = static_cast<int>(std::floor(mn.x - margin));
		const int gz0 = static_cast<int>(std::floor(mn.y - margin));
		const int nx = static_cast<int>(std::ceil(mx.x + margin)) - gx0;
		const int nz = static_cast<int>(std::ceil(mx.y + margin)) - gz0;
		std::vector<uint8_t> busy(static_cast<size_t>(nx) * static_cast<size_t>(nz), 0);
		for (int j = 0; j < nz; ++j) {
			for (int i = 0; i < nx; ++i) {
				busy[static_cast<size_t>(j) * nx + i] = busyAt(glm::vec2(gx0 + i + 0.5f, gz0 + j + 0.5f)) ? 1 : 0;
			}
		}
		auto cellBusy = [&](int i, int j) -> bool {
			if (i < 0 || j < 0 || i >= nx || j >= nz) return true;   // grid edge counts as street side
			return busy[static_cast<size_t>(j) * nx + i] != 0;
		};
		std::vector<uint8_t> visited(busy.size(), 0);
		auto freeCell = [&](int i, int j) -> bool {
			if (i < 0 || j < 0 || i >= nx || j >= nz) return false;
			const size_t k = static_cast<size_t>(j) * nx + i;
			return busy[k] == 0 && visited[k] == 0;
		};
		struct Lot { float x0, x1, z0, z1; };
		std::vector<Lot> lots;
		auto splitLengths = [&](float total, float lo, float hi) {
			std::vector<float> out;
			float rem = total;
			while (rem > hi + 1e-3f) {
				float s = uniform(lo, hi);
				if (rem - s < lo) s = rem;
				out.push_back(s);
				rem -= s;
			}
			if (rem > 0.5f) out.push_back(rem);
			return out;
		};
		for (int j = 0; j < nz; ++j) {
			for (int i = 0; i < nx; ++i) {
				if (!freeCell(i, j)) continue;
				int w = 1;
				while (freeCell(i + w, j)) ++w;
				int h = 1;
				bool grow = true;
				while (grow && j + h < nz) {
					for (int k = 0; k < w; ++k) {
						if (!freeCell(i + k, j + h)) { grow = false; break; }
					}
					if (grow) ++h;
				}
				for (int jj = j; jj < j + h; ++jj) {
					for (int ii = i; ii < i + w; ++ii) visited[static_cast<size_t>(jj) * nx + ii] = 1;
				}
				if (w < 2 || h < 2) continue;
				const float x0 = static_cast<float>(gx0 + i), z0 = static_cast<float>(gz0 + j);
				const float rw = static_cast<float>(w), rh = static_cast<float>(h);
				if (rw >= rh) {
					float xa = x0;
					for (float lw : splitLengths(rw, 9.0f, 20.0f)) {
						const std::vector<float> rows = rh > 26.0f ? splitLengths(rh, 12.0f, 24.0f) : std::vector<float>{ rh };
						float za = z0;
						for (float lh : rows) { lots.push_back({ xa, xa + lw, za, za + lh }); za += lh; }
						xa += lw;
					}
				}
				else {
					float za = z0;
					for (float lh : splitLengths(rh, 9.0f, 20.0f)) {
						const std::vector<float> cols = rw > 26.0f ? splitLengths(rw, 12.0f, 24.0f) : std::vector<float>{ rw };
						float xa = x0;
						for (float lw : cols) { lots.push_back({ xa, xa + lw, za, za + lh }); xa += lw; }
						za += lh;
					}
				}
			}
		}

		// ---- Buildings.
		const int facadeMats[] = { D_FAC_BRICK_RED, D_FAC_BRICK_BROWN, D_FAC_CONCRETE, D_FAC_STUCCO_CREAM, D_FAC_STUCCO_TAN, D_FAC_PAINT_BLUE, D_FAC_PAINT_GREEN };
		const float facadeWeights[] = { 0.2f, 0.16f, 0.16f, 0.16f, 0.12f, 0.1f, 0.1f };
		const int awningMats[] = { D_AWNING_RED, D_AWNING_GREEN, D_AWNING_CREAM };
		const int neonMats[] = { M_NEON_PINK, M_NEON_CYAN, M_NEON_RED, M_NEON_PURPLE, M_NEON_AMBER };
		const glm::vec3 neonColor[] = { glm::vec3(1.0f, 0.12f, 0.55f), glm::vec3(0.1f, 0.85f, 1.0f), glm::vec3(1.0f, 0.08f, 0.04f), glm::vec3(0.55f, 0.2f, 1.0f), glm::vec3(1.0f, 0.55f, 0.08f) };
		int neonFills = 0;
		int buildingCount = 0;
		for (const Lot& L : lots) {
			const float x0 = L.x0 + 0.15f, z0 = L.z0 + 0.15f, z1 = L.z1 - 0.15f;
			float x1 = L.x1 - 0.15f;
			if (x1 - x0 < 2.0f || z1 - z0 < 2.0f) continue;
			// Surface parking lot instead of a building: asphalt, stall lines and a few parked cars.
			if (x1 - x0 > 14.0f && z1 - z0 > 14.0f && chance(0.1f)) {
				kit.Flat(D_ROAD_CLEAN, x0 + 0.3f, x1 - 0.3f, z0 + 0.3f, z1 - 0.3f, 0.004f);
				for (float x = x0 + 1.5f; x < x1 - 1.0f; x += 2.6f) {
					if (decals.Active()) {
						decals.Line(RoadDecal::LineSingleWhite, glm::vec3(x + 0.05f, 0.009f, z0 + 1.0f), glm::vec3(x + 0.05f, 0.009f, z1 - 1.0f), 0.1f);
					}
					else {
						kit.Flat(M_PAINT_WHITE, x, x + 0.1f, z0 + 1.0f, z1 - 1.0f, 0.008f);
					}
				}
				for (float x = x0 + 1.5f; x + 2.6f < x1 - 0.5f; x += 2.6f) {
					if (!chance(0.45f)) continue;
					const glm::vec3 c(x + 1.3f, 0.0f, z0 + 3.4f);
					if (glm::length(glm::vec2(c.x - spawn.x, c.z - spawn.y)) < 9.0f) continue;
					parkedCar(c, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
				}
				continue;
			}
			++buildingCount;
			// Narrow service lane on the +X side of some lots (dumpster, pipes, fire escape).
			const bool alley = x1 - x0 > 12.0f && chance(0.3f);
			if (alley) x1 -= 2.8f;
			const glm::vec3 cc((x0 + x1) * 0.5f, 0.0f, (z0 + z1) * 0.5f);
			const float dist = glm::length(glm::vec2(cc.x - spawn.x, cc.z - spawn.y));
			int storeys = dist < 170.0f ? 1 + pickInt(3) : 2 + pickInt(4);
			if (dist >= 170.0f && chance(0.12f)) storeys = 4 + pickInt(2);
			const float H = std::min(storeys * 3.4f + 0.6f, 18.0f);

			const float r0 = uniform(0.0f, 1.0f);
			int fm = facadeMats[0];
			{
				float acc = 0.0f;
				for (int k = 0; k < 7; ++k) {
					acc += facadeWeights[k];
					if (r0 <= acc) { fm = facadeMats[k]; break; }
				}
			}

			// Street-facing side: the neighbour cell just outside a face is band (busy).
			const int cgx = static_cast<int>(std::floor(cc.x)) - gx0;
			const int cgz = static_cast<int>(std::floor(cc.z)) - gz0;
			const int xl = static_cast<int>(std::floor(x0 - 0.6f)) - gx0;
			const int xr = static_cast<int>(std::floor(x1 + 0.6f)) - gx0;
			const int zl = static_cast<int>(std::floor(z0 - 0.6f)) - gz0;
			const int zr = static_cast<int>(std::floor(z1 + 0.6f)) - gz0;
			int street = -1;   // 0 -X, 1 +X, 2 -Z, 3 +Z
			if (cellBusy(xl, cgz)) street = 0;
			else if (cellBusy(xr, cgz)) street = 1;
			else if (cellBusy(cgx, zl)) street = 2;
			else if (cellBusy(cgx, zr)) street = 3;

			// Walls on all four sides.
			struct Face { glm::vec3 mid, A, N; float len; };
			auto faceSet = [](float fx0, float fx1, float fz0, float fz1) {
				return std::vector<Face>{
					{ glm::vec3(fx0, 0.0f, (fz0 + fz1) * 0.5f), glm::vec3(0, 0, 1), glm::vec3(-1, 0, 0), fz1 - fz0 },
					{ glm::vec3(fx1, 0.0f, (fz0 + fz1) * 0.5f), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), fz1 - fz0 },
					{ glm::vec3((fx0 + fx1) * 0.5f, 0.0f, fz0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), fx1 - fx0 },
					{ glm::vec3((fx0 + fx1) * 0.5f, 0.0f, fz1), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), fx1 - fx0 },
				};
			};
			const std::vector<Face> faces = faceSet(x0, x1, z0, z1);
			// Window grid on one face between yFrom and yTo; most are dark, a few lit at low luminance.
			auto windowRows = [&](const Face& F, float yFrom, float yTo) {
				for (float y0 = yFrom; y0 + 2.2f < yTo - 0.4f; y0 += 3.4f) {
					for (float u = -F.len * 0.5f + 1.6f; u + 1.6f < F.len * 0.5f - 0.6f; u += 3.2f) {
						const glm::vec3 c = F.mid + F.A * u + up * (y0 + 1.0f) + F.N * 0.03f;
						const float q = uniform(0.0f, 1.0f);
						int wm = M_WINDOW_DARK;
						if (q < 0.09f) wm = M_WINDOW_WARM;
						else if (q < 0.13f) wm = M_WINDOW_COOL;
						else if (q < 0.21f) wm = M_WINDOW_DIM;
						if (wm == M_WINDOW_DARK) {
							// Dark panes are rooms too (faint). Tile from position, not rng, so the layout stream is unchanged.
							const int dTile = static_cast<int>(std::fmod(std::fabs(c.x * 0.37f + c.y * 0.71f + c.z * 1.13f) * 16.0f, 16.0f));
							const glm::vec2 d0(static_cast<float>(dTile % 4) * 0.25f + 0.004f, static_cast<float>(dTile / 4) * 0.25f + 0.004f);
							kit.QuadUV(D_WINDOW_ROOM_DARK, c, F.A * 0.8f, up * 1.0f, F.N, d0, d0 + glm::vec2(0.242f));
						}
						else {
							const int tile = pickInt(16);
							const glm::vec2 t0(static_cast<float>(tile % 4) * 0.25f + 0.004f, static_cast<float>(tile / 4) * 0.25f + 0.004f);
							kit.QuadUV(wm, c, F.A * 0.8f, up * 1.0f, F.N, t0, t0 + glm::vec2(0.242f));
						}
					}
				}
			};
			for (int f = 0; f < 4; ++f) {
				const Face& F = faces[f];
				kit.Quad(fm, F.mid + up * (H * 0.5f), F.A * (F.len * 0.5f), up * (H * 0.5f), F.N, F.A, glm::vec3(0, -1, 0));
				windowRows(F, 3.5f, H);
			}

			// Setback tower on part of the lot: a taller inset mass with its own window grid.
			float topH = H;
			float ax0 = x0, ax1 = x1, az0 = z0, az1 = z1;
			const bool tower = x1 - x0 > 12.0f && z1 - z0 > 12.0f && storeys >= 2 && chance(0.3f);
			if (tower) {
				ax0 = x0 + (x1 - x0) * uniform(0.0f, 0.3f);
				ax1 = x1 - (x1 - x0) * uniform(0.0f, 0.3f);
				az0 = z0 + (z1 - z0) * uniform(0.0f, 0.3f);
				az1 = z1 - (z1 - z0) * uniform(0.0f, 0.3f);
				topH = std::min(H + uniform(6.0f, 12.0f), 30.0f);
				kit.Box(fm, glm::vec3(ax0, H, az0), glm::vec3(ax1, topH, az1), 0x3Fu & ~8u);
				for (const Face& F : faceSet(ax0, ax1, az0, az1)) windowRows(F, H + 1.0f, topH);
			}

			// Ground floor shopfronts on the street-facing side.
			if (street >= 0) {
				const Face& F = faces[street];
				float u = -F.len * 0.5f + 0.2f;
				while (u < F.len * 0.5f - 2.4f) {
					// With room atlases the glass is snapped to one of the shop-room pane widths
					// (2.4 / 3.2 / 4.0 / 4.8 m, door strip 1.85 m) so every bay is exactly one pane.
					const float bayDraw = uniform(4.0f, 7.0f);
					const int shopSize = std::clamp(static_cast<int>((bayDraw - 4.0f) / 3.0f * 4.0f), 0, 3);
					const float shopGlass = 2.4f + 0.8f * static_cast<float>(shopSize);
					const float bay = std::min(roomAtlas ? shopGlass + 1.85f : bayDraw, F.len * 0.5f - u);
					if (bay < 2.6f) break;
					const float ua = u, ub = u + bay;
					u = ub;
					// Door at the bay start, glass for the rest.
					const float doorU0 = ua + 0.25f, doorU1 = ua + 1.35f;
					kit.Quad(D_DOOR, F.mid + F.A * ((doorU0 + doorU1) * 0.5f) + up * 1.15f + F.N * 0.02f,
						F.A * 0.55f, up * 1.15f, F.N, F.A, glm::vec3(0, -1, 0));
					const float gu0 = doorU1 + 0.25f, gu1 = ub - 0.25f;
					if (gu1 - gu0 > 1.0f) {
						const glm::vec3 gc = F.mid + F.A * ((gu0 + gu1) * 0.5f) + up * 1.5f + F.N * 0.03f;
						const glm::vec3 gh = F.A * ((gu1 - gu0) * 0.5f);
						if (chance(0.4f)) {
							const glm::vec2 shopUv0(uniform(0.0f, 0.4f), 0.0f), shopUv1(uniform(0.5f, 0.9f), 1.0f);
							if (roomAtlas && std::abs((gu1 - gu0) - shopGlass) < 0.01f) {
								// Shop/cafe room behind the glass (interior-mapping tile UV contract).
								kit.QuadUV(D_SHOP_ROOM_24 + shopSize, gc, gh, up * 1.1f, F.N, glm::vec2(0.004f), glm::vec2(0.246f));
							}
							else {
								kit.QuadUV(M_SHOPFRONT, gc, gh, up * 1.1f, F.N, shopUv0, shopUv1);
							}
						}
						else {
							kit.Quad(M_WINDOW_DARK, gc, gh, up * 1.1f, F.N, F.A, glm::vec3(0, -1, 0));
						}
						if (chance(0.15f)) {
							kit.Quad(D_SHUTTER, gc + F.N * 0.02f, gh, up * 1.1f, F.N, F.A, glm::vec3(0, -1, 0));
						}
					}
					// Awning and signage.
					if (chance(0.6f)) {
						FaceBox(kit, awningMats[pickInt(3)], F.mid, F.A, F.N, ua - 0.05f, ub + 0.05f, 2.85f, 3.25f, 1.0f);
					}
					if (chance(0.35f)) {
						FaceBox(kit, M_SIGN_BOX, F.mid, F.A, F.N, ua + 0.6f, ub - 0.6f, 3.35f, 3.85f, 0.22f);
						if (chance(0.5f)) {
							const int k = pickInt(5);
							kit.Quad(neonMats[k], F.mid + F.A * ((ua + ub) * 0.5f) + up * 3.6f + F.N * 0.24f,
								F.A * ((ub - ua) * 0.22f), up * 0.05f, F.N, F.A, up);
							if (neonFills < 10 && chance(0.4f)) {
								++neonFills;
								result.lightNodes.push_back(MakeLight("district_neon_fill_" + std::to_string(neonFills), "point",
									F.mid + F.A * ((ua + ub) * 0.5f) + up * 3.4f + F.N * 1.2f, glm::vec3(0.0f),
									neonColor[k], 500.0f, 8.0f, 0.0f, 0.0f, false, 0.4f));
							}
						}
					}
					// Pillar between bays.
					FaceBox(kit, fm, F.mid, F.A, F.N, ub - 0.15f, ub + 0.15f, 0.0f, 3.25f, 0.12f);
				}
			}

			// Service lane on the +X side: dumpster, wall pipes.
			if (alley) {
				const float ax = x1 + 1.45f, zc = (z0 + z1) * 0.5f;
				if (z1 - z0 > 6.0f && chance(0.7f)) {
					kit.Box(D_BIN, glm::vec3(ax - 0.55f, 0.0f, zc - 1.0f), glm::vec3(ax + 0.55f, 1.2f, zc + 1.0f), 0x3Fu & ~8u);
					boxObstacle(glm::vec3(ax, 0.0f, zc), glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 0.55f);
				}
				for (float z = z0 + 2.0f; z < z1 - 2.0f; z += 9.0f) {
					if (chance(0.5f)) kit.Cylinder(M_POLE, glm::vec3(x1 + 0.1f, 0.0f, z), H, 0.06f, 6);
				}
			}
			// Fire escape on the alley wall, or on the back face opposite the street.
			{
				const int fe = alley ? 1 : (street >= 0 ? (street ^ 1) : pickInt(4));
				if (H > 7.0f && (alley || chance(0.3f))) {
					const Face& F = faces[fe];
					for (float y = 3.4f; y + 1.0f < H; y += 3.4f) {
						FaceBox(kit, M_POLE, F.mid, F.A, F.N, -1.6f, 1.6f, y, y + 0.08f, 0.9f);
						Stick(kit, M_POLE, F.mid + F.A * -1.6f + F.N * 0.9f + up * (y + 0.95f),
							F.mid + F.A * 1.6f + F.N * 0.9f + up * (y + 0.95f), 0.025f);
						Stick(kit, M_POLE, F.mid + F.A * 1.2f + up * y, F.mid + F.A * 1.2f + up * (y + 3.4f), 0.03f);
					}
				}
			}

			// Roof: parapet slab; rooftop units on the setback tower when present.
			kit.Box(M_ROOF, glm::vec3(x0 - 0.1f, H, z0 - 0.1f), glm::vec3(x1 + 0.1f, H + 0.5f, z1 + 0.1f), 0x3Fu & ~8u);
			if (tower) {
				kit.Box(M_ROOF, glm::vec3(ax0 - 0.1f, topH, az0 - 0.1f), glm::vec3(ax1 + 0.1f, topH + 0.5f, az1 + 0.1f), 0x3Fu & ~8u);
			}
			const float rx0 = tower ? ax0 : x0, rx1 = tower ? ax1 : x1;
			const float rz0 = tower ? az0 : z0, rz1 = tower ? az1 : z1;
			const int units = (rx1 - rx0 > 3.5f && rz1 - rz0 > 3.5f) ? pickInt(3) : 0;
			for (int k = 0; k < units; ++k) {
				const float ux = uniform(rx0 + 1.0f, rx1 - 2.2f), uz = uniform(rz0 + 1.0f, rz1 - 2.2f);
				if (chance(0.35f) && prop("exterior_aircon_unit", "aircon", glm::vec3(ux + 0.9f, topH + 0.5f, uz + 0.46f), pickInt(2) * 1.5707963f)) continue;
				kit.Box(D_ROOF_UNIT, glm::vec3(ux, topH + 0.5f, uz),
					glm::vec3(ux + uniform(1.2f, 2.0f), topH + uniform(1.2f, 1.8f), uz + uniform(1.0f, 1.8f)), 0x3Fu & ~8u);
			}
		}

		// ---- Streetlights, junction signals, utility poles and wires, palms, clutter.
		int lampIndex = 0;
		auto streetlight = [&](const DistrictEdge& e, float t, float s, bool led) {
			const glm::vec3 base = e.P + e.d * t + e.n * (s * (e.hw + 0.9f)) + up * curbH;
			const glm::vec3 rn = -e.n * s;     // toward the road
			const float poleH = 8.5f;
			kit.Cylinder(M_POLE, base, poleH, 0.11f);
			OrientedBox(kit, M_POLE, base + up * (poleH - 0.12f) + rn * 1.45f, rn, e.d, 1.45f, 0.07f, 0.07f);
			const glm::vec3 headC = base + up * (poleH - 0.3f) + rn * 2.9f;
			OrientedBox(kit, M_POLE, headC, rn, e.d, 0.45f, 0.13f, 0.25f);
			kit.Quad(led ? M_LAMP_LED : M_LAMP_SODIUM, headC - up * 0.135f, rn * 0.42f, e.d * 0.2f, -up, rn, e.d);
			AddFlare(extras, led ? NightStreetPacks::FlareLED : NightStreetPacks::FlareSodium, headC - up * 0.135f, -up,
				materials[led ? M_LAMP_LED : M_LAMP_SODIUM].emissiveStrength, 0.9f);
			const float tilt = std::abs(e.d.z) > 0.5f ? 12.0f * rn.x : 0.0f;
			const bool shadows = lampIndex < shadowedLamps;
			result.lightNodes.push_back(MakeLight("streetlight_" + std::to_string(lampIndex), "spot", headC - up * 0.2f,
				glm::vec3(-90.0f, 0.0f, tilt), led ? kLED4000 : kSodiumHPS, led ? 20000.0f : 30000.0f,
				34.0f, 40.0f, 74.0f, shadows, 0.25f));
			++lampIndex;
		};
		for (const DistrictEdge& e : edges) {
			const float spacing = e.avenue ? avenueLampSpacing : crossLampSpacing;
			const float t0 = nodes[e.a].outer + 2.0f, t1 = e.L - nodes[e.b].outer - 2.0f;
			int k = 0;
			for (float t = t0 + spacing * 0.5f; t <= t1; t += spacing, ++k) {
				const float s = (k % 2 == 0) ? 1.0f : -1.0f;   // alternate sides for a staggered row
				streetlight(e, t, s, chance(0.15f));
				circle(e.P + e.d * t + e.n * (s * (e.hw + 0.9f)), 0.3f);
			}
		}

		// Traffic signals on two diagonal corners of junctions with three or more approaches.
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].degree < 3) continue;
			const glm::vec3 c = W3(nodes[i].p, 0.0f);
			const float o = nodes[i].road + 0.6f;
			for (float sx : { -1.0f, 1.0f }) {
				const float sz = sx;   // one diagonal pair: (+,+) and (-,-)
				const glm::vec3 p = c + glm::vec3(sx * o, 0.0f, sz * o) + up * curbH;
				kit.Cylinder(D_SIGNAL_HOUSING, p, 5.4f, 0.12f, 8);
				// Mast arm reaching over the junction; head hangs at its end.
				const glm::vec3 armEnd = c + glm::vec3(-sx * 1.5f, 0.0f, -sz * 1.5f) + up * 5.1f;
				Stick(kit, D_SIGNAL_HOUSING, p + up * 5.2f, armEnd, 0.07f);
				const glm::vec3 headC = armEnd - up * 0.5f;
				// Head faces the junction centre along the arm (x for the arm direction).
				OrientedBox(kit, D_SIGNAL_HOUSING, headC, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.2f, 0.5f, 0.2f);
				const glm::vec3 faceN(-sx, 0.0f, 0.0f);
				const int active = pickInt(3);
				const int lensMats[3] = { D_SIGNAL_RED, D_SIGNAL_AMBER, D_SIGNAL_GREEN };
				for (int k = 0; k < 3; ++k) {
					const int mat = (k == active) ? lensMats[k] : static_cast<int>(D_SIGNAL_HOUSING);
					const glm::vec3 lc = headC + faceN * 0.21f + up * (0.35f - 0.35f * static_cast<float>(k));
					kit.Quad(mat, lc, glm::vec3(0.0f, 0.0f, 0.15f), glm::vec3(0.0f, 0.1f, 0.0f), faceN, glm::vec3(0, 0, 1), up);
					if (k == active) AddFlare(extras, NightStreetPacks::FlareSignal, lc, faceN, materials[mat].emissiveStrength, 0.35f, materials[mat].emissive);
				}
				circle(p, 0.3f);
			}
		}

		// Span gantries over four-way junctions (on the diagonal signal poles) and corner street-name signs.
		for (size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].degree < 3) continue;
			const glm::vec3 c = W3(nodes[i].p, 0.0f);
			const float o = nodes[i].road + 0.6f;
			if (nodes[i].degree >= 4) {
				const glm::vec3 A = c + glm::vec3(o, 0.0f, o) + up * 5.9f;
				const glm::vec3 B = c + glm::vec3(-o, 0.0f, -o) + up * 5.9f;
				Stick(kit, D_SIGNAL_HOUSING, A, B, 0.07f);
				const glm::vec3 mid = (A + B) * 0.5f;
				OrientedBox(kit, D_SIGNAL_HOUSING, mid - up * 0.45f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 0.25f, 0.45f, 0.2f);
				kit.Quad(D_SIGNAL_RED, mid - up * 0.45f + glm::vec3(0.0f, 0.0f, 0.21f), glm::vec3(0.15f, 0.0f, 0.0f), glm::vec3(0.0f, 0.1f, 0.0f),
					glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f), up);
				AddFlare(extras, NightStreetPacks::FlareSignal, mid - up * 0.45f + glm::vec3(0.0f, 0.0f, 0.21f), glm::vec3(0.0f, 0.0f, 1.0f),
					materials[D_SIGNAL_RED].emissiveStrength, 0.35f, materials[D_SIGNAL_RED].emissive);
			}
			for (float sx : { -1.0f, 1.0f }) {
				const float sz = -sx;   // the two diagonal corners without a signal pole
				const glm::vec3 p = c + glm::vec3(sx * o, 0.0f, sz * o) + up * curbH;
				kit.Cylinder(M_POLE, p, 3.4f, 0.06f, 6);
				OrientedBox(kit, M_SIGN_BOX, p + up * 3.1f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 0.5f, 0.2f, 0.03f);
				circle(p, 0.15f);
			}
		}

		// Utility poles on both sides at a fixed spacing, with overhead wires along and across.
		for (const DistrictEdge& e : edges) {
			const float sp = 48.0f;
			const float t0 = nodes[e.a].outer + 6.0f, t1 = e.L - nodes[e.b].outer - 6.0f;
			glm::vec3 prevL(0.0f), prevR(0.0f);
			bool hasPrev = false;
			for (float t = t0 + 10.0f; t <= t1; t += sp) {
				const glm::vec3 bl = e.P + e.d * t + e.n * (e.hw + 3.4f);
				const glm::vec3 br = e.P + e.d * t - e.n * (e.hw + 3.4f);
				for (const glm::vec3 bp : { bl, br }) {
					const glm::vec3 base(bp.x, curbH, bp.z);
					kit.Cylinder(D_WOOD, base, 10.5f, 0.14f, 8);
					OrientedBox(kit, D_WOOD, base + up * 9.9f, e.n, e.d, 0.9f, 0.06f, 0.06f);
					circle(base, 0.3f);
					// Pole-mounted transformer on roughly one pole in three.
					if (chance(0.33f)) kit.Cylinder(D_ROOF_UNIT, base + up * 6.9f, 1.6f, 0.28f, 10);
				}
				const glm::vec3 tl(bl.x, curbH + 9.0f, bl.z), tr(br.x, curbH + 9.0f, br.z);
				auto sagged = [&](const glm::vec3& A, const glm::vec3& B, float sag) {
					const int segs = 4;
					auto pt = [&](float u) { return glm::mix(A, B, u) - up * (sag * 4.0f * u * (1.0f - u)); };
					for (int q = 0; q < segs; ++q) {
						const float u0 = static_cast<float>(q) / segs, u1 = static_cast<float>(q + 1) / segs;
						Stick(kit, D_WIRE, pt(u0), pt(u1), 0.025f);
					}
				};
				sagged(tl, tr, 0.3f);   // across the road
				if (hasPrev) {
					sagged(prevL, tl, 0.5f);
					sagged(prevR, tr, 0.5f);
					sagged(prevL + up * 0.25f, tl + up * 0.25f, 0.5f);
				}
				prevL = tl;
				prevR = tr;
				hasPrev = true;
			}
		}

		// Palms: dense along Ventura, sparse on the other avenue and cross streets.
		for (const DistrictEdge& e : edges) {
			if (!e.avenue) continue;
			const float sp = e.ventura ? 13.0f : 26.0f;
			const float t0 = nodes[e.a].outer + 3.0f, t1 = e.L - nodes[e.b].outer - 3.0f;
			for (float t = t0; t <= t1; t += sp) {
				for (float s : { -1.0f, 1.0f }) {
					if (!e.ventura && chance(0.5f)) continue;
					const glm::vec3 base = e.P + e.d * (t + uniform(-1.0f, 1.0f)) + e.n * (s * (e.hw + 2.1f));
					const glm::vec3 b0(base.x, curbH, base.z);
					if (glm::length(glm::vec2(b0.x - spawn.x, b0.z - spawn.y)) < 3.0f) continue;
					const float H = uniform(8.5f, 11.5f);
					kit.Cylinder(D_PALM_TRUNK, b0, H, 0.16f, 8);
					const glm::vec3 T = b0 + up * H;
					const int fronds = 7;
					const float a0 = uniform(0.0f, 6.2831f);
					for (int q = 0; q < fronds; ++q) {
						const float a = a0 + 6.2831853f * static_cast<float>(q) / fronds + uniform(-0.2f, 0.2f);
						const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
						const glm::vec3 tip = T + dir * uniform(2.0f, 2.6f) - up * uniform(0.8f, 1.4f);
						const glm::vec3 Uv = (tip - T) * 0.5f;
						const glm::vec3 side2 = glm::normalize(glm::cross(Uv, up));
						const glm::vec3 Vv = side2 * 0.4f;
						kit.Quad(D_PALM_FROND, (T + tip) * 0.5f, Uv, Vv, glm::normalize(glm::cross(Uv, Vv)), glm::normalize(Uv), glm::normalize(Vv));
					}
					circle(b0, 0.35f);
				}
			}
		}

		// Sidewalk clutter: CC0 hydrants, bins, planters, potted plants, shrubs, benches and utility boxes
		// (procedural fallbacks where a model is absent); parked cars along the outer lanes.
		for (const DistrictEdge& e : edges) {
			const float t0 = nodes[e.a].outer + 4.0f, t1 = e.L - nodes[e.b].outer - 4.0f;
			const float yawD = yawAlong(e.d);
			auto sideOf = [&]() { return chance(0.5f) ? 1.0f : -1.0f; };
			for (float t = t0; t <= t1; t += 40.0f) {
				if (chance(0.35f)) {
					const float s = sideOf();
					const glm::vec3 hb = e.P + e.d * t + e.n * (s * (e.hw + 1.4f)) + up * curbH;
					if (!prop("fire_hydrant", "hydrant", hb, uniform(0.0f, 6.2831853f))) {
						kit.Cylinder(D_HYDRANT, hb, 0.7f, 0.12f, 8);
						kit.Cylinder(D_HYDRANT, hb + up * 0.6f, 0.15f, 0.17f, 8);
					}
					circle(hb, 0.25f);
				}
				if (chance(0.25f)) {
					const float s = sideOf();
					const glm::vec3 bc = e.P + e.d * (t + 8.0f) + e.n * (s * (e.hw + 3.4f)) + up * (curbH + 0.45f);
					const glm::vec3 base(bc.x, curbH, bc.z);
					if (prop("metal_trash_can", "bin", base, yawD)) {
						boxObstacle(base, e.d, 0.9f, 0.3f);
					}
					else {
						OrientedBox(kit, D_BIN, bc, e.d, e.n, 0.33f, 0.45f, 0.33f);
						obstacles.push_back({ {"type", "box"}, {"center", { bc.x, bc.z }}, {"half", { 0.35f, 0.35f }}, {"yaw", 0.0f} });
					}
				}
				// Planters sit between the lamp row and the shopfronts; offsets keep clear of hydrants and bins.
				if (chance(0.3f)) {
					const float s = sideOf();
					const glm::vec3 pc = e.P + e.d * (t + 11.0f) + e.n * (s * (e.hw + curbT + 1.2f));
					const glm::vec3 base(pc.x, curbH, pc.z);
					if (prop("planter_box_01", "planter", base, yawD)) boxObstacle(base, e.d, 0.46f, 0.22f);
				}
				if (chance(0.4f)) {
					const float s = sideOf();
					const glm::vec3 pp = e.P + e.d * (t + 19.0f) + e.n * (s * (e.hw + curbT + sidewalkW - 0.7f));
					const glm::vec3 base(pp.x, curbH, pp.z);
					if (prop("potted_plant_01", "pot", base, uniform(0.0f, 6.2831853f))) circle(base, 0.35f);
				}
				if (chance(0.2f)) {
					const float s = sideOf();
					const glm::vec3 sc = e.P + e.d * (t + 27.0f) + e.n * (s * (e.hw + curbT + sidewalkW - 0.3f));
					const glm::vec3 base(sc.x, curbH, sc.z);
					if (prop("shrub_01", "shrub", base, yawD)) boxObstacle(base, e.d, 1.3f, 0.2f);
				}
				if (chance(0.2f)) {
					const float s = sideOf();
					const glm::vec3 bp = e.P + e.d * (t + 33.0f) + e.n * (s * (e.hw + curbT + 2.2f));
					const glm::vec3 base(bp.x, curbH, bp.z);
					if (prop("modular_street_seating", "bench", base, yawD)) boxObstacle(base, e.d, 2.17f, 0.4f);
				}
				if (chance(0.12f)) {
					const float s = sideOf();
					const glm::vec3 pb = e.P + e.d * (t + 14.0f) + e.n * (s * (e.hw + curbT + sidewalkW - 0.6f));
					const glm::vec3 base(pb.x, curbH, pb.z);
					const bool utility = chance(0.5f);
					if (prop(utility ? "utility_box_01" : "power_box_01", "utilbox", base, yawD)) boxObstacle(base, e.d, 0.3f, 0.3f);
				}
			}
			const float carSpacing = 11.0f;
			for (float t = t0 + 8.0f; t <= t1 - 4.0f; t += carSpacing) {
				if (!chance(e.ventura ? 0.4f : 0.22f)) continue;
				const float s = sideOf();
				const glm::vec3 c = e.P + e.d * t + e.n * (s * (e.hw - 1.2f));
				if (glm::length(glm::vec2(c.x - spawn.x, c.z - spawn.y)) < 9.0f) continue;
				parkedCar(c, e.d, e.n);
			}
		}

		// Distant skyline ring and hills; a few far lights for urban depth.
		{
			const glm::vec2 cen = (mn + mx) * 0.5f;
			const glm::vec2 ext = (mx - mn) * 0.5f + glm::vec2(margin + 40.0f);
			for (int k = 0; k < 40; ++k) {
				const float a = 6.2831853f * static_cast<float>(k) / 40.0f + uniform(-0.05f, 0.05f);
				const float R = uniform(1.0f, 1.25f);
				const glm::vec2 p = cen + glm::vec2(std::cos(a) * ext.x * R, std::sin(a) * ext.y * R);
				const float w = uniform(25.0f, 60.0f), dpt = uniform(25.0f, 60.0f), h = uniform(25.0f, 110.0f);
				kit.Box(chance(0.7f) ? D_SKYLINE : D_SKYLINE_DARK, glm::vec3(p.x - w * 0.5f, 0.0f, p.y - dpt * 0.5f),
					glm::vec3(p.x + w * 0.5f, h, p.y + dpt * 0.5f), 0x3Fu & ~8u);
			}
			for (int k = 0; k < 16; ++k) {
				const float a = 6.2831853f * static_cast<float>(k) / 16.0f;
				const glm::vec2 p = cen + glm::vec2(std::cos(a) * ext.x * 1.6f, std::sin(a) * ext.y * 1.6f);
				const float w = uniform(160.0f, 260.0f), dpt = uniform(120.0f, 180.0f), h = uniform(25.0f, 60.0f);
				kit.Box(D_HILL, glm::vec3(p.x - w * 0.5f, -1.0f, p.y - dpt * 0.5f), glm::vec3(p.x + w * 0.5f, h, p.y + dpt * 0.5f), 0x3Fu & ~8u);
			}
			for (int k = 0; k < 6; ++k) {
				const float a = 6.2831853f * (static_cast<float>(k) + 0.5f) / 6.0f;
				const glm::vec2 p = cen + glm::vec2(std::cos(a) * ext.x * 1.12f, std::sin(a) * ext.y * 1.12f);
				result.lightNodes.push_back(MakeLight("district_far_light_" + std::to_string(k), "point",
					glm::vec3(p.x, 22.0f, p.y), glm::vec3(0.0f), kSodiumHPS, 2500.0f, 120.0f, 0.0f, 0.0f, false, 1.0f));
			}
		}

		// ---- Reflection probes: one per junction plus one per ~48 m of street, capped.
		auto probe = [&](const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi, float blend) {
			result.reflectionProbes.push_back({
				{"position", { p.x, p.y, p.z }},
				{"box_min", { lo.x, lo.y, lo.z }},
				{"box_max", { hi.x, hi.y, hi.z }},
				{"blend_distance", blend} });
		};
		size_t probeCount = 0;
		for (size_t i = 0; i < nodes.size() && probeCount < maxProbes; ++i) {
			if (nodes[i].degree < 2) continue;
			const glm::vec3 c = W3(nodes[i].p, 1.6f);
			const float o = nodes[i].outer + 1.0f;
			probe(c, glm::vec3(c.x - o, -1.0f, c.z - o), glm::vec3(c.x + o, 30.0f, c.z + o), 4.0f);
			++probeCount;
		}
		for (const DistrictEdge& e : edges) {
			const int sections = std::max(1, static_cast<int>(std::ceil(e.L / 48.0f)));
			const float step = e.L / static_cast<float>(sections);
			for (int k = 0; k < sections && probeCount < maxProbes; ++k) {
				const glm::vec3 A = e.P + e.d * (step * static_cast<float>(k));
				const glm::vec3 B = e.P + e.d * (step * static_cast<float>(k + 1));
				const glm::vec3 c = (A + B) * 0.5f + up * 1.6f;
				const float o = e.hw + bandOuterEdge + 1.0f;
				const glm::vec3 lo(std::min(A.x, B.x) - o, -1.0f, std::min(A.z, B.z) - o);
				const glm::vec3 hi(std::max(A.x, B.x) + o, 30.0f, std::max(A.z, B.z) + o);
				probe(c, lo, hi, 3.0f);
				++probeCount;
			}
		}

		// ---- glTF and layout output.
		const fs::path gltfPath = fs::path(output + ".gltf");
		result.ok = WriteGltf(gltfPath, materials, kit, textureRoot, result.triangleCount, result.primitiveCount);
		result.gltfPath = gltfPath.generic_string();
		const fs::path layoutPath = fs::path(output + "_layout.json");
		{
			json layout;
			layout["obstacles"] = obstacles;
			std::ofstream out(layoutPath);
			if (out) out << layout.dump(1);
			result.layoutPath = layoutPath.generic_string();
		}
		std::cout << "[ProceduralStreetKit] District " << result.gltfPath << " ok=" << result.ok
			<< " lots=" << buildingCount << " primitives=" << result.primitiveCount
			<< " triangles=" << result.triangleCount << " lights=" << result.lightNodes.size()
			<< " probes=" << result.reflectionProbes.size() << " obstacles=" << obstacles.size()
			<< " decals=" << DecalCount(extras) << " flares=" << extras.lensFlares.sources.size() << std::endl;
		return result;
	}

	void BuildDefaultDistrictNetwork(RoadNetwork& network) {
		// Two avenues (x = 0 and x = 150) joined by four cross streets: three city blocks per side,
		// several 4-way intersections, a long loop and two dead-end-free turnarounds.
		network.Clear();
		network.SetDefaultHalfWidth(7.5f);
		network.SetSidewalkWidth(4.25f);
		const float halfWidth = network.DefaultHalfWidth();
		const float rows[] = { -80.0f, 60.0f, 200.0f, 340.0f };
		for (int r = 0; r < 4; ++r) {
			network.AddNode({ "A" + std::to_string(r), glm::vec2(0.0f, rows[r]) });
			network.AddNode({ "B" + std::to_string(r), glm::vec2(150.0f, rows[r]) });
		}
		auto add = [&](const std::string& a, const std::string& b, const std::string& name) {
			RoadNetwork::Edge e;
			e.a = network.FindNode(a);
			e.b = network.FindNode(b);
			e.halfWidth = halfWidth;
			e.name = name;
			network.AddEdge(e);
		};
		for (int r = 0; r < 3; ++r) {
			add("A" + std::to_string(r), "A" + std::to_string(r + 1), "Ventura Ave");
			add("B" + std::to_string(r), "B" + std::to_string(r + 1), "Palm Blvd");
		}
		const char* cross[] = { "1st St", "2nd St", "3rd St", "4th St" };
		for (int r = 0; r < 4; ++r) add("A" + std::to_string(r), "B" + std::to_string(r), cross[r]);
		network.Reindex();
	}

} // namespace ProceduralStreetKit
