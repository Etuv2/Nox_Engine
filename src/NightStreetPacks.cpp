#include "NightStreetPacks.h"
#include "json.hpp"
#include "stb_image.h"
#include "assets/AssetIO.h"
#include "assets/AssetIOImage.h"
#include "assets/AssetIOJson.h"
#include "InteriorAtlasManifest.h"

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

	bool ReadJson(const fs::path& path, json& out) {
		const std::optional<std::string> text = AssetIO::ReadText(path.generic_string());
		if (!text) return false;
		out = json::parse(*text, nullptr, false);
		return !out.is_discarded();
	}

	bool AllFilesExist(const fs::path& root, const char* const* files, int count) {
		std::error_code ec;
		for (int i = 0; i < count; ++i) {
			if (!AssetIO::Exists((root / files[i]).generic_string())) return false;
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
			if (!found || !AssetIO::Exists((fs::path(root) / kDecalFiles[i]).generic_string())) {
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
			unsigned char* data = AssetIO::LoadImageFile(file.generic_string(), &w, &h, &n, 4);
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
		return InteriorAtlasManifest::Load(root, out);
	}

} // namespace NightStreetPacks
