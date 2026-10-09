#include "ProceduralStreetKit.h"
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
			MeshBuilder& b = Builder(mat, center.z);
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
			MeshBuilder& b = Builder(mat, center.z);
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
			MeshBuilder& b = Builder(mat, base.z);
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

	private:
		MeshBuilder& Builder(int mat, float z) {
			// 40 m chunks along the street so frustum and shadow culling stay effective.
			const int chunk = static_cast<int>(std::floor(z / 40.0f));
			return m_builders[{ mat, chunk }];
		}

		const std::vector<MatDef>& m_materials;
		std::map<std::pair<int, int>, MeshBuilder> m_builders;
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
					materials[m].emissive = glm::vec3(1.0f);
				}
			}
			if (WriteShopInterior(shopPath, seed)) {
				materials[M_SHOPFRONT].emissiveTexture = shopPath.generic_string();
				materials[M_SHOPFRONT].emissive = glm::vec3(1.0f);
			}
		}
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
		markingsFor(zStart, crossZ0 - 4.0f);
		markingsFor(crossZ1 + 4.0f, zEnd);
		// Stop lines and crosswalks.
		kit.Flat(M_PAINT_WHITE, -roadHalf + 0.3f, 0.0f, crossZ0 - 3.6f, crossZ0 - 3.2f, paintY);
		kit.Flat(M_PAINT_WHITE, 0.0f, roadHalf - 0.3f, crossZ1 + 3.2f, crossZ1 + 3.6f, paintY);
		for (float x = -roadHalf + 0.5f; x < roadHalf - 0.5f; x += 1.0f) {
			kit.Flat(M_PAINT_WHITE, x, x + 0.5f, crossZ0 - 3.0f, crossZ0 - 0.4f, paintY);
			kit.Flat(M_PAINT_WHITE, x, x + 0.5f, crossZ1 + 0.4f, crossZ1 + 3.0f, paintY);
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
						kit.Quad(mat, glm::vec3(faceX, y + 1.0f, z + 0.75f), glm::vec3(0, 0, 0.75f), glm::vec3(0, 1.0f, 0),
							N, texU, glm::vec3(0, -1, 0));
					}
					else {
						// Random atlas tile, inset slightly to avoid mip bleeding between tiles.
						const int tile = static_cast<int>(rng() % 16u);
						const glm::vec2 t0(static_cast<float>(tile % 4) * 0.25f + 0.004f, static_cast<float>(tile / 4) * 0.25f + 0.004f);
						kit.QuadUV(mat, glm::vec3(faceX, y + 1.0f, z + 0.75f), texU * 0.75f, glm::vec3(0, 1.0f, 0), N,
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
			<< " lights=" << result.lightNodes.size() << " probes=" << result.reflectionProbes.size() << std::endl;
		return result;
	}
}
