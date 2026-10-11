#pragma once
#include <string>
#include <glm/glm.hpp>
#include "json.hpp"

// Game-agnostic CPU post-processing of an equirectangular HDR environment (linear float RGB(A)).
// Applied once at load, before the cubemap conversion, so diffuse irradiance and the specular
// prefilter derive from the processed data.
struct EnvironmentMapOptions {
	float rotationDeg = 0.0f;   // azimuth rotation about +Y (column shift = rotationDeg / 360 * width)
	float intensity = 1.0f;     // linear radiance multiplier
	float maxRadiance = 0.0f;   // > 0: clamp per-texel luminance to this value (hue preserved)

	bool groundEnabled = false;
	glm::vec3 groundColor{ 0.0f };
	float groundBlendStartDeg = 2.0f;   // elevation where the blend starts (weight 0)
	float groundBlendEndDeg = -6.0f;    // elevation where the ground colour is fully applied (weight 1)
};

struct EnvironmentProcessStats {
	int clampedTexels = 0;
};

// Parses the object form of the top-level "skybox" key. The "path" and "fallback" fields are
// read by the caller (they need scene-relative path resolution). Invalid fields fall back to
// defaults and append a message to *warnings (may be null).
EnvironmentMapOptions ParseEnvironmentMapOptions(const nlohmann::json& skyboxObject, std::string* warnings);

// Processes interleaved float pixels in place: rotation, intensity, luminance clamp, ground blend.
// channels: 1..4; only the first min(channels, 3) channels are treated as colour (alpha is untouched).
EnvironmentProcessStats ProcessEquirectRadiance(float* pixels, int width, int height, int channels,
	const EnvironmentMapOptions& options);

// True when the options would change the pixel data (used to skip the pass for plain skyboxes).
bool EnvironmentMapOptionsAreIdentity(const EnvironmentMapOptions& options);
