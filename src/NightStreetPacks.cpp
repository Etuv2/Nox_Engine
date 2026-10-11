#include "NightStreetPacks.h"
#include "json.hpp"
#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

	const char* kDecalFiles[static_cast<int>(NightStreetPacks::RoadDecal::Count)] = {
		"arrow_straight_white.png", "arrow_left_white.png", "arrow_right_white.png",
		"arrow_straight_left_white.png", "arrow_straight_right_white.png",
		"line_double_yellow.png", "line_single_yellow.png", "lane_dash_white.png", "line_single_white.png",
		"stop_bar_white.png", "crosswalk_white.png" };

	// Sprite layers of the lens flare pack.
	enum FlareTexture : int { TexGlowSodium = 0, TexGlowLED, TexStarburst4, TexStarburst6, TexStreak, TexHalo, TexCount };
	const char* kFlareFiles[TexCount] = {
		"glow_sodium.png", "glow_led.png", "starburst_4.png", "starburst_6.png", "streak_horizontal.png", "halo_ring.png" };

	// Window room pack: everything (rooms, face slots, style weights, room light and shape) comes from
	// atlas_manifest.json ("assets" and the "nox_interior" block); see textures/window_rooms/README.md.
	// Style rows in the manifest, in the order of NightStreetPacks::RoomStyle.
	const char* kRoomStyleNames[] = { "apartment", "shop", "office" };

	bool ReadJson(const fs::path& path, json& out) {
		std::ifstream in(path);
		if (!in) return false;
		out = json::parse(in, nullptr, false);
		return !out.is_discarded();
	}

	bool AllFilesExist(const fs::path& root, const char* const* files, int count) {
		std::error_code ec;
		for (int i = 0; i < count; ++i) {
			if (!fs::exists(root / files[i], ec)) return false;
		}
		return true;
	}

} // namespace

namespace NightStreetPacks {

	const DecalPaintInfo* LoadDecalPaintInfo(const std::string& root) {
		static std::map<std::string, std::unique_ptr<DecalPaintInfo>> cache;
		if (root.empty()) return nullptr;
		auto it = cache.find(root);
		if (it != cache.end()) return it->second.get();
		std::unique_ptr<DecalPaintInfo>& slot = cache[root];

		json meta;
		if (!ReadJson(fs::path(root) / "generation_prompts.json", meta) || !meta.contains("assets")) {
			std::cerr << "[NightStreetPacks] road decals: no generation_prompts.json under '" << root << "'" << std::endl;
			return nullptr;
		}
		auto info = std::make_unique<DecalPaintInfo>();
		constexpr int count = static_cast<int>(RoadDecal::Count);
		for (int i = 0; i < count; ++i) {
			bool found = false;
			for (const json& a : meta["assets"]) {
				if (a.value("file", std::string()) != kDecalFiles[i] || !a.contains("paint_bounds_uv_top_left")) continue;
				const json& b = a["paint_bounds_uv_top_left"];
				if (!b.is_array() || b.size() < 4) continue;
				info->bounds[i] = glm::vec4(b[0].get<float>(), b[1].get<float>(), b[2].get<float>(), b[3].get<float>());
				found = true;
			}
			std::error_code ec;
			if (!found || !fs::exists(fs::path(root) / kDecalFiles[i], ec)) {
				std::cerr << "[NightStreetPacks] road decals: missing " << kDecalFiles[i] << " or its paint bounds" << std::endl;
				return nullptr;
			}
			info->tailU[i] = (info->bounds[i].x + info->bounds[i].z) * 0.5f;
		}

		// Arrow stems: alpha-weighted centre of the painted pixels just above the bottom of the paint
		// bounds. Turn arrows do not have their tail at u = 0.5 (pack README "Placement and scale").
		for (int i = 0; i <= static_cast<int>(RoadDecal::ArrowStraightRight); ++i) {
			stbi_set_flip_vertically_on_load_thread(0);
			int w = 0, h = 0, n = 0;
			const fs::path file = fs::path(root) / kDecalFiles[i];
			unsigned char* data = stbi_load(file.string().c_str(), &w, &h, &n, 4);
			if (!data) continue;
			const glm::vec4 b = info->bounds[i];
			const int y0 = static_cast<int>((b.w - 0.08f * (b.w - b.y)) * h);
			const int y1 = static_cast<int>((b.w - 0.02f * (b.w - b.y)) * h);
			double sum = 0.0, sumX = 0.0;
			for (int y = std::max(y0, 0); y < std::min(y1, h); ++y) {
				for (int x = 0; x < w; ++x) {
					const unsigned char a = data[(static_cast<size_t>(y) * w + x) * 4 + 3];
					if (a < 128) continue;
					sum += a;
					sumX += a * (x + 0.5);
				}
			}
			stbi_image_free(data);
			if (sum > 0.0) info->tailU[i] = static_cast<float>(sumX / sum / w);
		}
		slot = std::move(info);
		return slot.get();
	}

	bool BuildRoadDecalSet(const std::string& root, DecalSet& out) {
		if (!LoadDecalPaintInfo(root)) return false;
		DecalSet set;
		set.name = "road_paint";
		for (const char* f : kDecalFiles) set.layers.push_back({ (fs::path(root) / f).generic_string() });
		// The landscape stop bar is stored square: decal quads carry the true aspect.
		set.textureOptions.size = 1024;
		set.textureOptions.debugName = "road decals";
		// Road paint (pack README "Material setup"): texture white ~0.75 linear -> ~0.7 albedo, dry
		// roughness 0.55, the street-kit paint wetness and the asphalt's standing-water coverage.
		set.material.albedoScale = 0.95f;
		set.material.roughness = 0.55f;
		set.material.wetnessResponse = 0.75f;
		set.material.puddleResponse = 1.0f;
		out = std::move(set);
		return true;
	}

	bool BuildLensFlareSet(const std::string& root, LensFlareSet& out) {
		if (root.empty() || !AllFilesExist(root, kFlareFiles, TexCount)) return false;
		LensFlareSet set;
		for (const char* f : kFlareFiles) set.layers.push_back({ (fs::path(root) / f).generic_string() });
		set.textureOptions.size = 512;
		set.textureOptions.anisotropic = false;
		set.textureOptions.debugName = "lens flares";

		// Pack README "Suggested combinations": coloured glows untinted, one starburst per source with a
		// mild tint, streak/halo kept faint and reserved for strong glare. Widths/gains from the README.
		const glm::vec3 warm(1.0f, 0.82f, 0.6f), cool(0.88f, 0.94f, 1.0f);
		auto layer = [](int tex, float gain, float width, glm::vec3 tint, float glareWeight = 0.0f, bool sourceTint = false) {
			LensFlareLayer l;
			l.textureLayer = tex; l.gain = gain; l.width = width; l.tint = tint;
			l.glareWeight = glareWeight; l.useSourceTint = sourceTint;
			return l;
		};
		set.styles.resize(FlareStyleCount);
		set.styles[FlareSodium].layers = { layer(TexGlowSodium, 1.0f, 1.0f, glm::vec3(1.0f)), layer(TexStarburst4, 0.15f, 2.0f, warm),
			layer(TexStreak, 0.04f, 3.0f, warm, 1.0f) };
		set.styles[FlareLED].layers = { layer(TexGlowLED, 1.0f, 1.0f, glm::vec3(1.0f)), layer(TexStarburst6, 0.12f, 2.0f, cool),
			layer(TexStreak, 0.04f, 3.0f, cool, 1.0f) };
		set.styles[FlareFlood].layers = { layer(TexGlowLED, 1.0f, 1.0f, glm::vec3(1.0f)), layer(TexStarburst6, 0.12f, 2.0f, glm::vec3(1.0f), 0.0f, true),
			layer(TexStreak, 0.04f, 3.0f, cool), layer(TexHalo, 0.02f, 3.0f, cool, 1.0f) };
		set.styles[FlareSignal].layers = { layer(TexGlowLED, 1.0f, 1.0f, glm::vec3(1.0f), 0.0f, true) };
		out = std::move(set);
		return true;
	}

	bool BuildInteriorAtlas(const std::string& root, InteriorAtlas& out) {
		const char* tag = "[NightStreetPacks] window rooms: ";
		json manifest;
		if (root.empty() || !ReadJson(fs::path(root) / "atlas_manifest.json", manifest) ||
			!manifest.contains("assets") || !manifest["assets"].is_array() || manifest["assets"].empty()) {
			std::cerr << tag << "cannot read atlas_manifest.json under '" << root << "'" << std::endl;
			return false;
		}
		const json nox = manifest.value("nox_interior", json::object());
		auto readVec2 = [](const json& j, const char* key, glm::vec2& v) {
			if (j.contains(key) && j[key].is_array() && j[key].size() >= 2) v = glm::vec2(j[key][0].get<float>(), j[key][1].get<float>());
		};
		auto readVec3 = [](const json& j, glm::vec3& v) {
			if (j.is_array() && j.size() >= 3) v = glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
		};

		InteriorAtlas atlas;
		// Face slots: nox_interior.face_order, else the manifest's face names.
		std::vector<std::string> faceNames;
		if (nox.contains("face_order") && nox["face_order"].is_array()) {
			for (const json& f : nox["face_order"]) if (f.is_string()) faceNames.push_back(f.get<std::string>());
		}
		else if (manifest.contains("faces") && manifest["faces"].is_object()) {
			for (auto it = manifest["faces"].begin(); it != manifest["faces"].end(); ++it) faceNames.push_back(it.key());
		}
		for (const std::string& name : faceNames) {
			InteriorAtlas::FaceRole role;
			if (!InteriorAtlas::FaceRoleFromName(name, role)) {
				std::cerr << tag << "unknown face '" << name << "'" << std::endl;
				return false;
			}
			atlas.faceRoles.push_back(role);
		}
		if (atlas.faceRoles.empty()) {
			std::cerr << tag << "no faces listed" << std::endl;
			return false;
		}

		std::error_code ec;
		std::vector<std::string> roomFiles;
		for (const json& asset : manifest["assets"]) {
			const std::string file = asset.value("file", std::string());
			if (file.empty() || !fs::exists(fs::path(root) / file, ec) || !asset.contains("faces")) {
				std::cerr << tag << "asset '" << file << "' is missing" << std::endl;
				return false;
			}
			const json& faces = asset["faces"];
			for (const std::string& faceName : faceNames) {
				if (!faces.contains(faceName) || !faces[faceName].contains("sample_uv_rect") || faces[faceName]["sample_uv_rect"].size() < 4) {
					std::cerr << tag << file << " lacks face " << faceName << std::endl;
					return false;
				}
				const json& face = faces[faceName];
				// sample_uv_rect: pixel-centre bounds with a 2-pixel inset, image top-left origin.
				const json& uv = face["sample_uv_rect"];
				atlas.faceLayers.push_back({ (fs::path(root) / file).generic_string(),
					glm::vec4(uv[0].get<float>(), uv[1].get<float>(), uv[2].get<float>(), uv[3].get<float>()) });
				glm::vec3 mean(0.1f);
				if (face.contains("mean_linear_rgb")) readVec3(face["mean_linear_rgb"], mean);
				atlas.faceMean.push_back(mean);
			}
			roomFiles.push_back(file);
		}
		const int rooms = static_cast<int>(roomFiles.size());
		auto roomIndex = [&](const std::string& file) {
			for (int r = 0; r < rooms; ++r) if (roomFiles[r] == file) return r;
			return -1;
		};

		atlas.textureOptions.size = nox.value("face_size", 512);
		atlas.textureOptions.premultiply = false;   // opaque RGB rooms
		atlas.textureOptions.debugName = "window rooms";

		// Room light: (occupied brightness, chance on when unoccupied, that brightness).
		glm::vec3 lightDefault(1.0f, 0.0f, 0.0f);
		if (nox.contains("room_light_default")) readVec3(nox["room_light_default"], lightDefault);
		atlas.roomLight.assign(rooms, lightDefault);
		if (nox.contains("room_light") && nox["room_light"].is_object()) {
			for (auto it = nox["room_light"].begin(); it != nox["room_light"].end(); ++it) {
				const int r = roomIndex(it.key());
				if (r < 0) { std::cerr << tag << "room_light names unknown room " << it.key() << std::endl; continue; }
				readVec3(it.value(), atlas.roomLight[r]);
			}
		}

		// Variant weights per material style; the game's RoomStyle indices must match the row order.
		if (nox.contains("styles") && nox["styles"].is_array()) {
			for (const json& style : nox["styles"]) {
				std::vector<float> row(rooms, 0.0f);
				if (style.contains("weights") && style["weights"].is_object()) {
					for (auto it = style["weights"].begin(); it != style["weights"].end(); ++it) {
						const int r = roomIndex(it.key());
						if (r < 0) { std::cerr << tag << "style weights name unknown room " << it.key() << std::endl; continue; }
						row[r] = it.value().get<float>();
					}
				}
				const size_t index = atlas.styleWeights.size();
				const std::string name = style.value("name", std::string());
				if (index < std::size(kRoomStyleNames) && name != kRoomStyleNames[index]) {
					std::cerr << tag << "style row " << index << " is '" << name << "', expected '" << kRoomStyleNames[index] << "'" << std::endl;
				}
				atlas.styleWeights.push_back(std::move(row));
			}
		}

		if (nox.contains("room_shape") && nox["room_shape"].is_object()) {
			const json& shape = nox["room_shape"];
			readVec2(shape, "side_margin", atlas.roomShape.sideMargin);
			readVec2(shape, "height", atlas.roomShape.height);
			readVec2(shape, "sill_scale", atlas.roomShape.sillScale);
			atlas.roomShape.depthJitter = shape.value("depth_jitter", atlas.roomShape.depthJitter);
		}
		if (nox.contains("pane_uv") && nox["pane_uv"].is_array() && nox["pane_uv"].size() >= 3) {
			atlas.paneUV.grid = nox["pane_uv"][0].get<float>();
			atlas.paneUV.inset = nox["pane_uv"][1].get<float>();
			atlas.paneUV.extent = nox["pane_uv"][2].get<float>();
		}
		atlas.farDistance = nox.value("far_distance", atlas.farDistance);
		out = std::move(atlas);
		return true;
	}

} // namespace NightStreetPacks
