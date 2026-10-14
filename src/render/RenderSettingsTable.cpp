#include "RenderSettingsTable.h"

#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../ScreenQuad.h"

#include <cmath>
#include <cstdint>

namespace nox::render {

namespace {

using PV = nox::reflect::PropValue;
using PT = nox::reflect::PropType;

RenderSetting Base(const char* key, const char* group, const char* name, PT type, const char* tooltip) {
	RenderSetting s;
	s.key = key;
	s.group = group;
	s.label = std::string(group) + " / " + name;
	s.tooltip = tooltip;
	s.type = type;
	return s;
}

RenderSetting FloatSetting(const char* key, const char* group, const char* name, float RenderContext::*m,
	double lo, double hi, double step, const char* tooltip = "") {
	RenderSetting s = Base(key, group, name, PT::Float, tooltip);
	s.minValue = lo;
	s.maxValue = hi;
	s.step = step;
	if (hi - lo <= 10.0) s.flags |= nox::reflect::PropSlider;
	s.get = [m](const RenderContext& c) { return PV(static_cast<double>(c.*m)); };
	s.set = [m](RenderContext& c, const PV& v) { c.*m = static_cast<float>(std::get<double>(v)); return true; };
	return s;
}

RenderSetting IntSetting(const char* key, const char* group, const char* name, int RenderContext::*m,
	double lo, double hi, double step, const char* tooltip = "") {
	RenderSetting s = Base(key, group, name, PT::Int, tooltip);
	s.minValue = lo;
	s.maxValue = hi;
	s.step = step;
	s.get = [m](const RenderContext& c) { return PV(static_cast<int64_t>(c.*m)); };
	s.set = [m](RenderContext& c, const PV& v) { c.*m = static_cast<int>(std::get<int64_t>(v)); return true; };
	return s;
}

RenderSetting BoolSetting(const char* key, const char* group, const char* name, bool RenderContext::*m,
	const char* tooltip = "") {
	RenderSetting s = Base(key, group, name, PT::Bool, tooltip);
	s.get = [m](const RenderContext& c) { return PV(static_cast<bool>(c.*m)); };
	s.set = [m](RenderContext& c, const PV& v) { c.*m = std::get<bool>(v); return true; };
	return s;
}

RenderSetting Vec3Setting(const char* key, const char* group, const char* name, glm::vec3 RenderContext::*m,
	double lo, double hi, double step, const char* tooltip = "") {
	RenderSetting s = Base(key, group, name, PT::Vec3, tooltip);
	s.minValue = lo;
	s.maxValue = hi;
	s.step = step;
	s.get = [m](const RenderContext& c) { return PV(c.*m); };
	s.set = [m](RenderContext& c, const PV& v) { c.*m = std::get<glm::vec3>(v); return true; };
	return s;
}

// Enum stored as an int index into names; the scene file uses the name.
RenderSetting EnumSetting(const char* key, const char* group, const char* name, int RenderContext::*m,
	std::vector<std::string> names, const char* tooltip = "") {
	RenderSetting s = Base(key, group, name, PT::Enum, tooltip);
	s.enumNames = std::move(names);
	s.get = [m](const RenderContext& c) { return PV(static_cast<int64_t>(c.*m)); };
	s.set = [m](RenderContext& c, const PV& v) { c.*m = static_cast<int>(std::get<int64_t>(v)); return true; };
	return s;
}

// Tone mapper: the scene names map to RenderContext::TonemapType in the order below.
RenderSetting TonemapSetting() {
	RenderSetting s = Base("tonemap", "Tone mapping", "Operator", PT::Enum, "Tone mapping operator");
	s.enumNames = { "gt", "aces", "gt7", "none" };
	s.get = [](const RenderContext& c) {
		int64_t index = 0;
		switch (c.tonemapType) {
		case RenderContext::TonemapType::GT: index = 0; break;
		case RenderContext::TonemapType::ACES: index = 1; break;
		case RenderContext::TonemapType::GT7: index = 2; break;
		case RenderContext::TonemapType::None: index = 3; break;
		}
		return PV(index);
	};
	s.set = [](RenderContext& c, const PV& v) {
		switch (std::get<int64_t>(v)) {
		case 0: c.tonemapType = RenderContext::TonemapType::GT; break;
		case 1: c.tonemapType = RenderContext::TonemapType::ACES; break;
		case 2: c.tonemapType = RenderContext::TonemapType::GT7; break;
		case 3: c.tonemapType = RenderContext::TonemapType::None; break;
		default: return false;
		}
		return true;
	};
	return s;
}

std::vector<RenderSetting> BuildTable() {
	std::vector<RenderSetting> t;
	const char* kExposure = "Exposure";
	t.push_back(BoolSetting("physical_camera", kExposure, "Physical camera", &RenderContext::physicalCamera,
		"Photometric units and pre-exposed lighting (needed for the night look)"));
	t.push_back(EnumSetting("exposure_mode", kExposure, "Mode", &RenderContext::exposureMode, { "manual", "auto" },
		"Manual EV100 or automatic histogram metering"));
	t.push_back(FloatSetting("exposure_ev100", kExposure, "Manual EV100", &RenderContext::exposureEV100, 0.0, 16.0, 0.05,
		"Exposure value at ISO 100; also the starting value for auto exposure"));
	t.push_back(FloatSetting("exposure_compensation", kExposure, "Compensation (EV)", &RenderContext::exposureCompensationEV,
		-6.0, 6.0, 0.05, "Applied in both modes; +1 is brighter"));
	t.push_back(FloatSetting("auto_exposure_min_ev100", kExposure, "Auto min EV100", &RenderContext::autoExposureMinEV100, -10.0, 20.0, 0.1));
	t.push_back(FloatSetting("auto_exposure_max_ev100", kExposure, "Auto max EV100", &RenderContext::autoExposureMaxEV100, -10.0, 20.0, 0.1));
	t.push_back(FloatSetting("auto_exposure_speed_up", kExposure, "Adapt speed up", &RenderContext::autoExposureSpeedUp, 0.0, 10.0, 0.1,
		"Adaptation rate toward brighter scenes (1/s)"));
	t.push_back(FloatSetting("auto_exposure_speed_down", kExposure, "Adapt speed down", &RenderContext::autoExposureSpeedDown, 0.0, 10.0, 0.1,
		"Adaptation rate toward darker scenes (1/s)"));
	t.push_back(FloatSetting("auto_exposure_low_percentile", kExposure, "Metering low percentile", &RenderContext::autoExposureLowPercentile, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("auto_exposure_high_percentile", kExposure, "Metering high percentile", &RenderContext::autoExposureHighPercentile, 0.0, 1.0, 0.01));

	const char* kBloom = "Bloom";
	t.push_back(BoolSetting("bloom", kBloom, "Enabled", &RenderContext::enableBloom));
	t.push_back(EnumSetting("bloom_mode", kBloom, "Mode", &RenderContext::bloomMode, { "additive", "energy_conserving" },
		"Additive thresholded (legacy) or energy-conserving mix"));
	t.push_back(FloatSetting("bloom_strength", kBloom, "Strength", &RenderContext::bloomStrength, 0.0, 4.0, 0.01));
	t.push_back(FloatSetting("bloom_threshold", kBloom, "Threshold", &RenderContext::bloomThreshold, 0.0, 10.0, 0.01,
		"In exposed units"));
	t.push_back(FloatSetting("bloom_knee", kBloom, "Knee", &RenderContext::bloomKnee, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("bloom_scatter_threshold", kBloom, "Scatter threshold", &RenderContext::bloomScatterThreshold, 0.0, 10.0, 0.01));
	t.push_back(BoolSetting("bloom_half_res", kBloom, "Half-resolution chain", &RenderContext::bloomHalfResChain));

	const char* kGrade = "Colour grade";
	t.push_back(FloatSetting("white_balance_temperature", kGrade, "White balance (K)", &RenderContext::whiteBalanceTemperature, 1000.0, 20000.0, 50.0,
		"Kelvin of the illuminant to neutralise"));
	t.push_back(FloatSetting("white_balance_tint", kGrade, "White balance tint", &RenderContext::whiteBalanceTint, -1.0, 1.0, 0.01,
		"Green (-) to magenta (+)"));
	t.push_back(FloatSetting("contrast", kGrade, "Contrast", &RenderContext::gradeContrast, 0.0, 3.0, 0.01));
	t.push_back(FloatSetting("saturation", kGrade, "Saturation", &RenderContext::gradeSaturation, 0.0, 3.0, 0.01));
	t.push_back(Vec3Setting("shadow_tint", kGrade, "Shadow tint", &RenderContext::gradeShadowTint, 0.0, 1.0, 0.01,
		"Additive split-tone colour for shadows"));
	t.push_back(Vec3Setting("highlight_tint", kGrade, "Highlight tint", &RenderContext::gradeHighlightTint, 0.0, 1.0, 0.01,
		"Additive split-tone colour for highlights"));
	t.push_back(FloatSetting("split_tone_strength", kGrade, "Split tone strength", &RenderContext::gradeSplitToneStrength, 0.0, 1.0, 0.01));
	t.push_back(Vec3Setting("lift", kGrade, "Lift", &RenderContext::gradeLift, -1.0, 1.0, 0.01,
		"Display-space lift after tone mapping"));
	t.push_back(Vec3Setting("gamma", kGrade, "Gamma", &RenderContext::gradeGamma, 0.1, 3.0, 0.01,
		"Display-space gamma after tone mapping"));
	t.push_back(Vec3Setting("gain", kGrade, "Gain", &RenderContext::gradeGain, 0.0, 4.0, 0.01,
		"Display-space gain after tone mapping"));

	const char* kOutput = "Lens and output";
	t.push_back(FloatSetting("vignette", kOutput, "Vignette", &RenderContext::vignetteIntensity, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("film_grain", kOutput, "Film grain", &RenderContext::filmGrainIntensity, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("chromatic_aberration", kOutput, "Chromatic aberration (px)", &RenderContext::chromaticAberration, 0.0, 20.0, 0.1));
	t.push_back(BoolSetting("dither", kOutput, "Output dither", &RenderContext::enableOutputDither,
		"Breaks 8-bit banding in dark gradients"));
	t.push_back(TonemapSetting());

	const char* kEnv = "Environment light";
	t.push_back(FloatSetting("ibl_intensity", kEnv, "IBL intensity", &RenderContext::iblIntensity, 0.0, 10.0, 0.01));
	t.push_back(FloatSetting("diffuse_ibl_scale", kEnv, "Diffuse IBL scale", &RenderContext::diffuseIBLScale, 0.0, 4.0, 0.01));
	t.push_back(FloatSetting("specular_ibl_scale", kEnv, "Specular IBL scale", &RenderContext::specularIBLScale, 0.0, 4.0, 0.01));
	t.push_back(FloatSetting("skybox_exposure", kEnv, "Skybox exposure", &RenderContext::skyboxExposure, 0.0, 16.0, 0.01,
		"Exposure of the skybox background"));

	const char* kAo = "Occlusion and GI";
	t.push_back(BoolSetting("ssao", kAo, "SSAO", &RenderContext::enableSSAO));
	t.push_back(FloatSetting("ssao_intensity", kAo, "SSAO intensity", &RenderContext::ssaoIntensity, 0.0, 2.0, 0.01));
	t.push_back(BoolSetting("ssgi", kAo, "Indirect diffuse (SSGI)", &RenderContext::enableIndirectDiffuse));
	t.push_back(FloatSetting("ssgi_strength", kAo, "SSGI strength", &RenderContext::indirectDiffuseStrength, 0.0, 4.0, 0.01));
	t.push_back(FloatSetting("ssgi_resolution", kAo, "SSGI trace resolution", &RenderContext::indirectDiffuseResolutionScale, 0.25, 0.5, 0.25,
		"Fraction of the frame the GI is traced at: 0.25 = quarter (default), 0.5 = half (about 4x the trace cost)"));
	t.push_back(IntSetting("ssgi_history_length", kAo, "SSGI history length", &RenderContext::indirectDiffuseHistoryLength, 1.0, 64.0, 1.0,
		"Max accumulated frames per pixel in the temporal resolve; higher = less noise, slower response"));
	t.push_back(IntSetting("ssgi_denoise_passes", kAo, "SSGI denoise passes", &RenderContext::indirectDiffuseDenoiseIterations, 1.0, 3.0, 1.0,
		"Edge-aware denoise passes after the temporal resolve (the first is wide, later ones are 3x3)"));
	t.push_back(BoolSetting("contact_shadows", kAo, "Contact shadows", &RenderContext::enableScreenSpaceShadows));

	const char* kTaa = "Anti-aliasing";
	t.push_back(BoolSetting("taa", kTaa, "TAA", &RenderContext::enableTAA));
	t.push_back(FloatSetting("taa_blend", kTaa, "Blend factor", &RenderContext::taaBlendFactor, 0.0, 1.0, 0.01));
	t.push_back(IntSetting("taa_rejection_mode", kTaa, "History rejection", &RenderContext::taaRejectionMode, 0.0, 1.0, 1.0,
		"0 = legacy depth/normal difference, 1 = disocclusion (reprojected depth)"));
	t.push_back(FloatSetting("taa_disocclusion_tolerance", kTaa, "Disocclusion tolerance", &RenderContext::taaDisocclusionTolerance, 0.001, 0.5, 0.001));
	t.push_back(FloatSetting("taa_motion_weight", kTaa, "Motion weight", &RenderContext::taaMotionCurrentWeight, 0.0, 1.0, 0.01));
	t.push_back(BoolSetting("velocity_from_depth", kTaa, "Velocity from depth", &RenderContext::velocityFromDepth));

	const char* kLight = "Lighting and debug";
	t.push_back(BoolSetting("clustered_lighting", kLight, "Clustered lighting", &RenderContext::enableClusteredLighting));
	t.push_back(BoolSetting("frustum_culling", kLight, "Frustum culling", &RenderContext::enableFrustumCulling));
	t.push_back(EnumSetting("debug_view", kLight, "Post debug view", &RenderContext::postDebugView,
		{ "none", "pre_taa", "taa", "ssr", "bloom", "velocity", "motion_blur", "taa_clean" }));

	const char* kWeather = "Weather";
	t.push_back(FloatSetting("wetness", kWeather, "Wetness", &RenderContext::weatherWetness, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("puddles", kWeather, "Puddles", &RenderContext::weatherPuddles, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("rain", kWeather, "Rain", &RenderContext::weatherRain, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("puddle_scale", kWeather, "Puddle scale (m)", &RenderContext::weatherPuddleScale, 0.5, 50.0, 0.1));

	const char* kSsr = "Reflections";
	t.push_back(BoolSetting("ssr", kSsr, "Screen-space reflections", &RenderContext::enableSSR));
	t.push_back(FloatSetting("ssr_max_roughness", kSsr, "Max roughness", &RenderContext::ssrMaxRoughness, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("ssr_max_distance", kSsr, "Max distance (m)", &RenderContext::ssrMaxDistance, 1.0, 500.0, 1.0));
	t.push_back(IntSetting("ssr_max_steps", kSsr, "Max steps", &RenderContext::ssrMaxSteps, 4.0, 256.0, 1.0));
	t.push_back(FloatSetting("ssr_thickness", kSsr, "Thickness (m)", &RenderContext::ssrThickness, 0.01, 2.0, 0.01));
	t.push_back(FloatSetting("ssr_temporal_blend", kSsr, "Temporal blend", &RenderContext::ssrTemporalBlend, 0.0, 1.0, 0.01));
	t.push_back(IntSetting("ssr_resolve_samples", kSsr, "Resolve samples", &RenderContext::ssrResolveSamples, 1.0, 16.0, 1.0));
	t.push_back(BoolSetting("reflection_probes", kSsr, "Reflection probes", &RenderContext::enableReflectionProbes));

	const char* kFog = "Volumetric fog";
	t.push_back(BoolSetting("volumetric_fog", kFog, "Enabled", &RenderContext::enableVolumetricFog));
	t.push_back(IntSetting("volumetric_tile_size", kFog, "Tile size (px)", &RenderContext::volumetricTileSize, 4.0, 32.0, 1.0));
	t.push_back(IntSetting("volumetric_slices", kFog, "Depth slices", &RenderContext::volumetricSlices, 8.0, 256.0, 1.0));
	t.push_back(FloatSetting("volumetric_far", kFog, "Far distance (m)", &RenderContext::volumetricFar, 10.0, 1000.0, 1.0));
	t.push_back(FloatSetting("fog_density", kFog, "Density", &RenderContext::fogDensity, 0.0, 0.5, 0.001,
		"Extinction (1/m) at the base height"));
	t.push_back(FloatSetting("fog_base_height", kFog, "Base height (m)", &RenderContext::fogBaseHeight, -100.0, 100.0, 0.5));
	t.push_back(FloatSetting("fog_height_falloff", kFog, "Height falloff (m)", &RenderContext::fogHeightFalloff, 0.1, 100.0, 0.1));
	t.push_back(FloatSetting("fog_anisotropy", kFog, "Anisotropy", &RenderContext::fogAnisotropy, -0.99, 0.99, 0.01,
		"Henyey-Greenstein g: forward scattering halos"));
	t.push_back(FloatSetting("fog_noise_strength", kFog, "Noise strength", &RenderContext::fogNoiseStrength, 0.0, 2.0, 0.01));
	t.push_back(FloatSetting("fog_noise_scale", kFog, "Noise scale (m)", &RenderContext::fogNoiseScale, 0.5, 200.0, 0.1));
	t.push_back(FloatSetting("fog_temporal_blend", kFog, "Temporal blend", &RenderContext::fogTemporalBlend, 0.0, 1.0, 0.01));
	t.push_back(FloatSetting("fog_distant_density", kFog, "Distant density", &RenderContext::fogDistantDensity, 0.0, 0.1, 0.0005,
		"Analytic haze beyond the far distance"));
	t.push_back(FloatSetting("fog_distant_glow", kFog, "Distant glow", &RenderContext::fogDistantGlow, 0.0, 10.0, 0.01));
	t.push_back(Vec3Setting("fog_albedo", kFog, "Albedo", &RenderContext::fogAlbedo, 0.0, 1.0, 0.01));
	t.push_back(Vec3Setting("fog_wind", kFog, "Wind", &RenderContext::fogWind, -10.0, 10.0, 0.01));
	t.push_back(Vec3Setting("fog_ambient", kFog, "Ambient radiance", &RenderContext::fogAmbientRadiance, 0.0, 10.0, 0.01,
		"Sky light inside the medium (cd/m^2)"));

	const char* kMb = "Motion blur";
	t.push_back(BoolSetting("motion_blur", kMb, "Enabled", &RenderContext::enableMotionBlur));
	t.push_back(FloatSetting("motion_blur_exposure_time", kMb, "Shutter time (s)", &RenderContext::motionBlurExposureTime, 0.001, 0.5, 0.001));
	t.push_back(FloatSetting("motion_blur_max_pixels", kMb, "Max blur (px at 1440p)", &RenderContext::motionBlurMaxPixels, 0.0, 256.0, 1.0));
	t.push_back(IntSetting("motion_blur_samples", kMb, "Samples", &RenderContext::motionBlurSamples, 1.0, 64.0, 1.0));

	const char* kRain = "Rain streaks";
	t.push_back(IntSetting("rain_drop_count", kRain, "Drop count", &RenderContext::rainDropCount, 0.0, 200000.0, 1000.0));
	t.push_back(FloatSetting("rain_brightness", kRain, "Brightness", &RenderContext::rainBrightness, 0.0, 4.0, 0.01));
	return t;
}

} // namespace

const std::vector<RenderSetting>& RenderSettingTable() {
	static const std::vector<RenderSetting> table = BuildTable();
	return table;
}

const RenderSetting* FindRenderSetting(const std::string& key) {
	for (const RenderSetting& s : RenderSettingTable()) {
		if (s.key == key) return &s;
	}
	return nullptr;
}

} // namespace nox::render
