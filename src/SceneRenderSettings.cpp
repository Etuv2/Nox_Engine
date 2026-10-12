#include "SceneRenderSettings.h"
#include "RenderContext.h"
#include "FrameBuffer.h"
#include "ScreenQuad.h"
#include "assets/AssetIO.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

using nlohmann::json;

namespace {
	template <typename T>
	void Read(const json& j, const char* key, T& target) {
		if (j.contains(key) && !j[key].is_null()) {
			try {
				target = j[key].get<T>();
			}
			catch (const std::exception& e) {
				std::cerr << "[SceneRenderSettings] Invalid value for '" << key << "': " << e.what() << std::endl;
			}
		}
	}

	void ReadVec3(const json& j, const char* key, glm::vec3& target) {
		if (j.contains(key) && j[key].is_array() && j[key].size() >= 3) {
			target = glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
		}
	}

	// Resets every field this module can set to the RenderContext defaults.
	void ResetControlledFields(RenderContext& ctx) {
		const RenderContext d;
		ctx.physicalCamera = d.physicalCamera;
		ctx.exposureMode = d.exposureMode;
		ctx.exposureEV100 = d.exposureEV100;
		ctx.exposureCompensationEV = d.exposureCompensationEV;
		ctx.autoExposureMinEV100 = d.autoExposureMinEV100;
		ctx.autoExposureMaxEV100 = d.autoExposureMaxEV100;
		ctx.autoExposureSpeedUp = d.autoExposureSpeedUp;
		ctx.autoExposureSpeedDown = d.autoExposureSpeedDown;
		ctx.autoExposureLowPercentile = d.autoExposureLowPercentile;
		ctx.autoExposureHighPercentile = d.autoExposureHighPercentile;
		ctx.enableBloom = d.enableBloom;
		ctx.bloomMode = d.bloomMode;
		ctx.bloomStrength = d.bloomStrength;
		ctx.bloomThreshold = d.bloomThreshold;
		ctx.bloomKnee = d.bloomKnee;
		ctx.bloomScatterThreshold = d.bloomScatterThreshold;
		ctx.bloomHalfResChain = d.bloomHalfResChain;
		ctx.whiteBalanceTemperature = d.whiteBalanceTemperature;
		ctx.whiteBalanceTint = d.whiteBalanceTint;
		ctx.gradeContrast = d.gradeContrast;
		ctx.gradeSaturation = d.gradeSaturation;
		ctx.gradeShadowTint = d.gradeShadowTint;
		ctx.gradeHighlightTint = d.gradeHighlightTint;
		ctx.gradeSplitToneStrength = d.gradeSplitToneStrength;
		ctx.gradeLift = d.gradeLift;
		ctx.gradeGamma = d.gradeGamma;
		ctx.gradeGain = d.gradeGain;
		ctx.vignetteIntensity = d.vignetteIntensity;
		ctx.filmGrainIntensity = d.filmGrainIntensity;
		ctx.chromaticAberration = d.chromaticAberration;
		ctx.enableOutputDither = d.enableOutputDither;
		ctx.tonemapType = d.tonemapType;
		ctx.iblIntensity = d.iblIntensity;
		ctx.diffuseIBLScale = d.diffuseIBLScale;
		ctx.specularIBLScale = d.specularIBLScale;
		ctx.skyboxExposure = d.skyboxExposure;
		ctx.enableSSAO = d.enableSSAO;
		ctx.ssaoIntensity = d.ssaoIntensity;
		ctx.enableIndirectDiffuse = d.enableIndirectDiffuse;
		ctx.indirectDiffuseStrength = d.indirectDiffuseStrength;
		ctx.enableScreenSpaceShadows = d.enableScreenSpaceShadows;
		ctx.enableTAA = d.enableTAA;
		ctx.taaBlendFactor = d.taaBlendFactor;
		ctx.taaRejectionMode = d.taaRejectionMode;
		ctx.taaDisocclusionTolerance = d.taaDisocclusionTolerance;
		ctx.taaMotionCurrentWeight = d.taaMotionCurrentWeight;
		ctx.postDebugView = d.postDebugView;
		ctx.velocityFromDepth = d.velocityFromDepth;
		ctx.enableClusteredLighting = d.enableClusteredLighting;
		ctx.enableFrustumCulling = d.enableFrustumCulling;
		ctx.ssrResolveSamples = d.ssrResolveSamples;
		ctx.weatherWetness = d.weatherWetness;
		ctx.weatherPuddles = d.weatherPuddles;
		ctx.weatherRain = d.weatherRain;
		ctx.weatherPuddleScale = d.weatherPuddleScale;
		ctx.enableSSR = d.enableSSR;
		ctx.ssrMaxRoughness = d.ssrMaxRoughness;
		ctx.ssrMaxDistance = d.ssrMaxDistance;
		ctx.ssrMaxSteps = d.ssrMaxSteps;
		ctx.ssrThickness = d.ssrThickness;
		ctx.ssrTemporalBlend = d.ssrTemporalBlend;
		ctx.enableReflectionProbes = d.enableReflectionProbes;
		ctx.enableMotionBlur = d.enableMotionBlur;
		ctx.rainDropCount = d.rainDropCount;
		ctx.rainBrightness = d.rainBrightness;
		ctx.motionBlurExposureTime = d.motionBlurExposureTime;
		ctx.motionBlurMaxPixels = d.motionBlurMaxPixels;
		ctx.motionBlurSamples = d.motionBlurSamples;
		ctx.enableVolumetricFog = d.enableVolumetricFog;
		ctx.volumetricTileSize = d.volumetricTileSize;
		ctx.volumetricSlices = d.volumetricSlices;
		ctx.volumetricFar = d.volumetricFar;
		ctx.fogDensity = d.fogDensity;
		ctx.fogBaseHeight = d.fogBaseHeight;
		ctx.fogHeightFalloff = d.fogHeightFalloff;
		ctx.fogAnisotropy = d.fogAnisotropy;
		ctx.fogNoiseStrength = d.fogNoiseStrength;
		ctx.fogNoiseScale = d.fogNoiseScale;
		ctx.fogTemporalBlend = d.fogTemporalBlend;
		ctx.fogDistantDensity = d.fogDistantDensity;
		ctx.fogDistantGlow = d.fogDistantGlow;
		ctx.fogAlbedo = d.fogAlbedo;
		ctx.fogWind = d.fogWind;
		ctx.fogAmbientRadiance = d.fogAmbientRadiance;
	}
}

namespace SceneRenderSettings {

	static std::string g_activeTier;
	const std::string& ActiveTier() { return g_activeTier; }

	void Apply(RenderContext& ctx, const json& s, bool resetFirst) {
		if (resetFirst) {
			ResetControlledFields(ctx);
		}
		if (!s.is_object()) {
			return;
		}
		// Camera / exposure
		Read(s, "physical_camera", ctx.physicalCamera);
		if (s.contains("exposure_mode") && s["exposure_mode"].is_string()) {
			ctx.exposureMode = s["exposure_mode"].get<std::string>() == "auto" ? 1 : 0;
		}
		Read(s, "exposure_ev100", ctx.exposureEV100);
		Read(s, "exposure_compensation", ctx.exposureCompensationEV);
		Read(s, "auto_exposure_min_ev100", ctx.autoExposureMinEV100);
		Read(s, "auto_exposure_max_ev100", ctx.autoExposureMaxEV100);
		Read(s, "auto_exposure_speed_up", ctx.autoExposureSpeedUp);
		Read(s, "auto_exposure_speed_down", ctx.autoExposureSpeedDown);
		Read(s, "auto_exposure_low_percentile", ctx.autoExposureLowPercentile);
		Read(s, "auto_exposure_high_percentile", ctx.autoExposureHighPercentile);

		// Bloom
		Read(s, "bloom", ctx.enableBloom);
		if (s.contains("bloom_mode") && s["bloom_mode"].is_string()) {
			ctx.bloomMode = s["bloom_mode"].get<std::string>() == "energy_conserving" ? 1 : 0;
		}
		Read(s, "bloom_strength", ctx.bloomStrength);
		Read(s, "bloom_threshold", ctx.bloomThreshold);
		Read(s, "bloom_knee", ctx.bloomKnee);
		Read(s, "bloom_scatter_threshold", ctx.bloomScatterThreshold);
		Read(s, "bloom_half_res", ctx.bloomHalfResChain);

		// Colour
		Read(s, "white_balance_temperature", ctx.whiteBalanceTemperature);
		Read(s, "white_balance_tint", ctx.whiteBalanceTint);
		Read(s, "contrast", ctx.gradeContrast);
		Read(s, "saturation", ctx.gradeSaturation);
		ReadVec3(s, "shadow_tint", ctx.gradeShadowTint);
		ReadVec3(s, "highlight_tint", ctx.gradeHighlightTint);
		Read(s, "split_tone_strength", ctx.gradeSplitToneStrength);
		ReadVec3(s, "lift", ctx.gradeLift);
		ReadVec3(s, "gamma", ctx.gradeGamma);
		ReadVec3(s, "gain", ctx.gradeGain);
		Read(s, "vignette", ctx.vignetteIntensity);
		Read(s, "film_grain", ctx.filmGrainIntensity);
		Read(s, "chromatic_aberration", ctx.chromaticAberration);
		Read(s, "dither", ctx.enableOutputDither);
		if (s.contains("tonemap") && s["tonemap"].is_string()) {
			const std::string t = s["tonemap"].get<std::string>();
			ctx.tonemapType = t == "none" ? RenderContext::TonemapType::None
				: t == "aces" ? RenderContext::TonemapType::ACES
				: t == "gt7" ? RenderContext::TonemapType::GT7
				: RenderContext::TonemapType::GT;
		}

		// Environment lighting
		Read(s, "ibl_intensity", ctx.iblIntensity);
		Read(s, "diffuse_ibl_scale", ctx.diffuseIBLScale);
		Read(s, "specular_ibl_scale", ctx.specularIBLScale);
		Read(s, "skybox_exposure", ctx.skyboxExposure);

		// Feature toggles
		Read(s, "ssao", ctx.enableSSAO);
		Read(s, "ssao_intensity", ctx.ssaoIntensity);
		Read(s, "ssgi", ctx.enableIndirectDiffuse);
		Read(s, "ssgi_strength", ctx.indirectDiffuseStrength);
		Read(s, "contact_shadows", ctx.enableScreenSpaceShadows);
		Read(s, "taa", ctx.enableTAA);
		Read(s, "taa_blend", ctx.taaBlendFactor);
		Read(s, "taa_rejection_mode", ctx.taaRejectionMode);
		Read(s, "taa_disocclusion_tolerance", ctx.taaDisocclusionTolerance);
		Read(s, "taa_motion_weight", ctx.taaMotionCurrentWeight);
		Read(s, "velocity_from_depth", ctx.velocityFromDepth);
		if (s.contains("debug_view") && s["debug_view"].is_string()) {
			static const char* kViews[] = { "none", "pre_taa", "taa", "ssr", "bloom", "velocity", "motion_blur", "taa_clean" };
			const std::string v = s["debug_view"].get<std::string>();
			for (int i = 0; i < 8; ++i) {
				if (v == kViews[i]) ctx.postDebugView = i;
			}
		}
		Read(s, "clustered_lighting", ctx.enableClusteredLighting);
		Read(s, "frustum_culling", ctx.enableFrustumCulling);
		Read(s, "ssr_resolve_samples", ctx.ssrResolveSamples);

		// Weather
		Read(s, "wetness", ctx.weatherWetness);
		Read(s, "puddles", ctx.weatherPuddles);
		Read(s, "rain", ctx.weatherRain);
		Read(s, "puddle_scale", ctx.weatherPuddleScale);

		// Reflections
		Read(s, "ssr", ctx.enableSSR);
		Read(s, "ssr_max_roughness", ctx.ssrMaxRoughness);
		Read(s, "ssr_max_distance", ctx.ssrMaxDistance);
		Read(s, "ssr_max_steps", ctx.ssrMaxSteps);
		Read(s, "ssr_thickness", ctx.ssrThickness);
		Read(s, "ssr_temporal_blend", ctx.ssrTemporalBlend);
		Read(s, "reflection_probes", ctx.enableReflectionProbes);
		Read(s, "motion_blur", ctx.enableMotionBlur);
		Read(s, "rain_drop_count", ctx.rainDropCount);
		Read(s, "rain_brightness", ctx.rainBrightness);
		Read(s, "motion_blur_exposure_time", ctx.motionBlurExposureTime);
		Read(s, "motion_blur_max_pixels", ctx.motionBlurMaxPixels);
		Read(s, "motion_blur_samples", ctx.motionBlurSamples);

		// Atmosphere
		Read(s, "volumetric_fog", ctx.enableVolumetricFog);
		Read(s, "volumetric_tile_size", ctx.volumetricTileSize);
		Read(s, "volumetric_slices", ctx.volumetricSlices);
		Read(s, "volumetric_far", ctx.volumetricFar);
		Read(s, "fog_density", ctx.fogDensity);
		Read(s, "fog_base_height", ctx.fogBaseHeight);
		Read(s, "fog_height_falloff", ctx.fogHeightFalloff);
		Read(s, "fog_anisotropy", ctx.fogAnisotropy);
		Read(s, "fog_noise_strength", ctx.fogNoiseStrength);
		Read(s, "fog_noise_scale", ctx.fogNoiseScale);
		Read(s, "fog_temporal_blend", ctx.fogTemporalBlend);
		Read(s, "fog_distant_density", ctx.fogDistantDensity);
		Read(s, "fog_distant_glow", ctx.fogDistantGlow);
		ReadVec3(s, "fog_albedo", ctx.fogAlbedo);
		ReadVec3(s, "fog_wind", ctx.fogWind);
		ReadVec3(s, "fog_ambient", ctx.fogAmbientRadiance);

	}

	bool ApplyFromSceneFile(RenderContext& ctx, const std::string& sceneFilePath, bool previousSceneHadSettings) {
		const std::optional<std::string> sceneText = AssetIO::ReadText(sceneFilePath);
		json scene = sceneText ? json::parse(*sceneText, nullptr, false) : json();
		const bool has = scene.is_object() && scene.contains("render_settings") && scene["render_settings"].is_object();
		g_activeTier.clear();
		if (has || previousSceneHadSettings) {
			Apply(ctx, has ? scene["render_settings"] : json::object());
		}
		if (has && scene.contains("quality_tiers") && scene["quality_tiers"].is_object()) {
			std::string tier = scene["render_settings"].value("quality_tier", std::string());
			if (const char* env = std::getenv("NOX_QUALITY_TIER"); env && env[0] != '\0') {
				tier = env;
			}
			if (!tier.empty() && scene["quality_tiers"].contains(tier)) {
				Apply(ctx, scene["quality_tiers"][tier], false);
				g_activeTier = tier;
				std::cout << "[SceneRenderSettings] Quality tier: " << tier << std::endl;
			}
			else if (!tier.empty()) {
				std::cerr << "[SceneRenderSettings] Unknown quality tier '" << tier << "'" << std::endl;
			}
		}
		if (has) {
			std::cout << "[SceneRenderSettings] Applied render_settings from " << sceneFilePath << std::endl;
		}
		return has;
	}
}
