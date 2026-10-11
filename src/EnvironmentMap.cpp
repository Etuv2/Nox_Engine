#include "EnvironmentMap.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace {
	float Smoothstep01(float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// Reads an optional [r,g,b] array; returns false (and leaves out untouched) when absent or malformed.
	bool ReadColor(const nlohmann::json& node, glm::vec3& out, std::string& error)
	{
		if (!node.is_array() || node.size() != 3) {
			error = "ground.color must be an array of 3 numbers";
			return false;
		}
		for (size_t i = 0; i < 3; ++i) {
			if (!node[i].is_number()) {
				error = "ground.color must be an array of 3 numbers";
				return false;
			}
			out[static_cast<int>(i)] = std::max(0.0f, node[i].get<float>());
		}
		return true;
	}
}

EnvironmentMapOptions ParseEnvironmentMapOptions(const nlohmann::json& skyboxObject, std::string* warnings)
{
	EnvironmentMapOptions opts;
	std::ostringstream log;
	auto note = [&](const std::string& msg) { log << "\n  - " << msg; };

	if (skyboxObject.contains("rotation_deg")) {
		if (skyboxObject["rotation_deg"].is_number()) {
			opts.rotationDeg = skyboxObject["rotation_deg"].get<float>();
		}
		else {
			note("rotation_deg must be a number; using 0");
		}
	}
	if (skyboxObject.contains("intensity")) {
		if (skyboxObject["intensity"].is_number() && skyboxObject["intensity"].get<float>() >= 0.0f) {
			opts.intensity = skyboxObject["intensity"].get<float>();
		}
		else {
			note("intensity must be a non-negative number; using 1");
		}
	}
	if (skyboxObject.contains("max_radiance")) {
		if (skyboxObject["max_radiance"].is_number() && skyboxObject["max_radiance"].get<float>() >= 0.0f) {
			opts.maxRadiance = skyboxObject["max_radiance"].get<float>();
		}
		else {
			note("max_radiance must be a non-negative number; using 0 (disabled)");
		}
	}

	if (skyboxObject.contains("ground")) {
		const nlohmann::json& ground = skyboxObject["ground"];
		std::string error;
		if (!ground.is_object() || !ground.contains("color")) {
			note("ground must be an object with a color; ground disabled");
		}
		else if (!ReadColor(ground["color"], opts.groundColor, error)) {
			note(error + "; ground disabled");
		}
		else {
			opts.groundEnabled = true;
			if (ground.contains("blend_start_deg") && ground["blend_start_deg"].is_number()) {
				opts.groundBlendStartDeg = ground["blend_start_deg"].get<float>();
			}
			if (ground.contains("blend_end_deg") && ground["blend_end_deg"].is_number()) {
				opts.groundBlendEndDeg = ground["blend_end_deg"].get<float>();
			}
			if (opts.groundBlendStartDeg < opts.groundBlendEndDeg) {
				note("ground.blend_start_deg is below blend_end_deg; using a hard edge at blend_end_deg");
				opts.groundBlendStartDeg = opts.groundBlendEndDeg;
			}
		}
	}

	if (warnings) {
		*warnings = log.str();
	}
	return opts;
}

bool EnvironmentMapOptionsAreIdentity(const EnvironmentMapOptions& options)
{
	return std::fabs(std::fmod(options.rotationDeg, 360.0f)) < 1e-6f
		&& options.intensity == 1.0f
		&& options.maxRadiance <= 0.0f
		&& !options.groundEnabled;
}

EnvironmentProcessStats ProcessEquirectRadiance(float* pixels, int width, int height, int channels,
	const EnvironmentMapOptions& options)
{
	EnvironmentProcessStats stats;
	if (!pixels || width <= 0 || height <= 0 || channels <= 0) {
		return stats;
	}
	const int colorChannels = std::min(channels, 3);
	const size_t rowFloats = static_cast<size_t>(width) * channels;

	// 1) Azimuth rotation: dst[x] = src[(x + shift) mod width], per row.
	double rot = std::fmod(static_cast<double>(options.rotationDeg), 360.0);
	if (rot < 0.0) rot += 360.0;
	const int shift = static_cast<int>(std::lround(rot / 360.0 * width)) % width;
	if (shift != 0) {
		std::vector<float> tmp(rowFloats);
		for (int y = 0; y < height; ++y) {
			float* row = pixels + static_cast<size_t>(y) * rowFloats;
			std::copy(row, row + rowFloats, tmp.begin());
			for (int x = 0; x < width; ++x) {
				const int srcX = (x + shift) % width;
				std::copy(tmp.begin() + static_cast<size_t>(srcX) * channels,
					tmp.begin() + static_cast<size_t>(srcX + 1) * channels,
					row + static_cast<size_t>(x) * channels);
			}
		}
	}

	// 2) Intensity and 3) luminance clamp (hue preserving), per texel.
	const bool scaleIntensity = options.intensity != 1.0f;
	const bool clampLum = options.maxRadiance > 0.0f;
	if (scaleIntensity || clampLum) {
		for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
			float* px = pixels + i * channels;
			if (scaleIntensity) {
				for (int c = 0; c < colorChannels; ++c) px[c] *= options.intensity;
			}
			if (clampLum && colorChannels >= 3) {
				const float lum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
				if (lum > options.maxRadiance) {
					const float s = options.maxRadiance / lum;
					for (int c = 0; c < 3; ++c) px[c] *= s;
					++stats.clampedTexels;
				}
			}
			else if (clampLum) {
				// Single-channel data: clamp the value directly.
				if (px[0] > options.maxRadiance) {
					px[0] = options.maxRadiance;
					++stats.clampedTexels;
				}
			}
		}
	}

	// 4) Ground replacement / blend by texel elevation (row centre; 0 = top = +90 deg).
	if (options.groundEnabled && colorChannels >= 3) {
		const float start = options.groundBlendStartDeg;
		const float end = options.groundBlendEndDeg;
		for (int y = 0; y < height; ++y) {
			const float elevation = 90.0f - 180.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
			float w = 0.0f;
			if (elevation <= end) {
				w = 1.0f;
			}
			else if (elevation < start && start > end) {
				// s = 0 at blend_start (weight 0), 1 at blend_end (weight 1).
				w = Smoothstep01((start - elevation) / (start - end));
			}
			if (w <= 0.0f) continue;
			float* row = pixels + static_cast<size_t>(y) * rowFloats;
			for (int x = 0; x < width; ++x) {
				float* px = row + static_cast<size_t>(x) * channels;
				for (int c = 0; c < 3; ++c) {
					px[c] += (options.groundColor[c] - px[c]) * w;
				}
			}
		}
	}

	return stats;
}
