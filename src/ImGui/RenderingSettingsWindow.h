#pragma once

#include "BaseWindow.h"
#include "../RenderContext.h"
#include <glm/glm.hpp>

// Forward declare ModularRenderer to avoid circular dependency
class ModularRenderer;

/**
 * @brief Rendering settings and pipeline controls window
 * Now integrated with ModularRenderer for real-time rendering control
 */
class RenderingSettingsWindow : public BaseWindow {
public:
	RenderingSettingsWindow();

	void Render() override;

	//Set the modular renderer to control
	void SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer);

	//Sync settings FROM renderer context (read current values)
	void SyncFromRenderer();

	//Sync settings TO renderer context (apply changes)
	void SyncToRenderer();

	// Getters for MainWindow to read settings
	float GetExposure() const { return m_exposure; }
	float GetGamma() const { return m_gamma; }
	bool GetEnableShadows() const { return m_enableShadows; }
	float GetShadowBias() const { return m_shadowBias; }
	float GetShadowNear() const { return m_shadowNear; }
	float GetShadowFar() const { return m_shadowFar; }
	bool GetEnableBloom() const { return m_enableBloom; }
	bool GetEnableTAA() const { return m_enableTAA; }
	bool GetEnableSSAO() const { return m_enableSSAO; }
	glm::vec3 GetEnvColor() const { return m_envColor; }

private:
	// Engine resolution
	glm::ivec2 m_windowResolution = glm::ivec2(1920, 1080);
	int m_resolutionPreset = 0;// 0=1920x1080,1=3840x1440
	// Post-processing settings
	float m_exposure = 0.96f;
	float m_gamma = 2.16f;
	bool m_enableHDR = true;
	bool m_enableBloom = true;
	float m_bloomStrength = 0.8f;
	float m_bloomKnee = 0.5f;
	float m_bloomThreshold = 1.0f;
	glm::vec3 m_envColor = glm::vec3(13.0f / 255.0f);

	// IBL intensity controls
	float m_iblIntensity = 0.1f;
	float m_skyboxExposure = 1.0f;
	float m_diffuseIBLScale = 0.3f;
	float m_specularIBLScale = 0.45f;

	// Tonemapping UI state
	int   m_tonemapType = 2; // 0=None,1=ACES,2=GT,3=GT7
	float m_tm_P = 1.0f, m_tm_a = 1.0f, m_tm_m = 0.22f, m_tm_l = 0.4f, m_tm_c = 2.022f, m_tm_b = 0.0f;
	// GT7 params
	float m_tm7_peakNits = 1000.0f;
	float m_tm7_blend = 0.6f;
	float m_tm7_fadeStart = 0.98f;
	float m_tm7_fadeEnd = 1.16f;
	bool  m_tm7_useJzazbz = false;
	bool  m_outputSRGB = true;

	// Shadow settings
	bool m_enableShadows = true;
	float m_shadowBias = 0.0008f;
	float m_shadowNear = 1.0f;
	float m_shadowFar = 100.0f;
	bool m_enablePCSS = false;
	float m_lightSize = 0.02f;
	int m_shadowDebugVisualization = 0;

	// Anti-aliasing settings
	bool m_enableTAA = true;
	float m_taaBlendFactor = 0.15f;
	float m_taaVarianceThreshold = 0.8f;
	float m_taaLumaWeight = 0.2f;
	bool m_taaUseYCoCg = false;
	bool m_enableSSAO = false;
	bool m_ssaoHalfRes = true;
	float m_ssaoResolutionScale = 0.5f;
	float m_ssaoRadius = 1.0f;
	float m_ssaoIntensity = 1.0f;
	float m_ssaoTemporalAlpha = 0.12f;

	// Indirect diffuse settings
	bool m_enableIndirectDiffuse = RenderContext::IndirectDiffuseDefaults::Enable;
	float m_indirectDiffuseStrength = RenderContext::IndirectDiffuseDefaults::Strength;
	float m_indirectDiffuseBounceFeedback = RenderContext::IndirectDiffuseDefaults::BounceFeedback;
	int m_indirectDiffuseSliceCount = RenderContext::IndirectDiffuseDefaults::SliceCount;
	int m_indirectDiffuseSamplesPerSlice = RenderContext::IndirectDiffuseDefaults::SamplesPerSlice;
	float m_indirectDiffuseRadiusVS = RenderContext::IndirectDiffuseDefaults::RadiusVS;
	float m_indirectDiffuseThicknessVS = RenderContext::IndirectDiffuseDefaults::ThicknessVS;
	float m_indirectDiffuseTemporalAlpha = RenderContext::IndirectDiffuseDefaults::TemporalAlpha;
	float m_indirectDiffuseNormalReject = RenderContext::IndirectDiffuseDefaults::NormalReject;
	float m_indirectDiffuseDepthReject = RenderContext::IndirectDiffuseDefaults::DepthReject;
	bool m_indirectDiffuseHistoryReset = false;
	float m_indirectDiffuseDenoiseStrength = RenderContext::IndirectDiffuseDefaults::DenoiseStrength;
	float m_indirectDiffuseUpscaleSharpness = RenderContext::IndirectDiffuseDefaults::UpscaleSharpness;
	int m_indirectDiffuseDebugStage = RenderContext::IndirectDiffuseDefaults::DebugMode;
	int m_indirectDiffuseCompositeMode = RenderContext::IndirectDiffuseDefaults::CompositeMode;
	bool m_indirectDiffuseValidationShowLegend = true;
	bool m_indirectDiffuseValidationEnableCursorProbe = false;
	bool m_indirectDiffuseValidationDisableReinjection = false;
	bool m_indirectDiffuseValidationDisableTemporal = false;
	bool m_indirectDiffuseValidationDisableDenoise = false;

	// Surfel GI persistent irradiance cache
	bool m_enableSurfelGI = false;
	int m_surfelGIQualityTier = 1;
	int m_surfelGIDebugView = 0;
	int m_surfelGIMaxSurfels = 65536;
	int m_surfelGIMaxRayBudget = 192;
	int m_surfelGISpawnTileSize = 8;
	int m_surfelGIMaxSurfelsPerCell = 128;
	int m_surfelGIMaxGatherSurfelsPerPixel = 32;
	int m_surfelGIMaxSpawnsPerFrame = 64;
	int m_surfelGIMaxProjectedSurfelsPerFrame = 24576;
	int m_surfelGIMaxRecycleCountPerFrame = 2048;
	float m_surfelGITargetRadiusPixels = 8.0f;
	float m_surfelGIMinRadius = 0.03f;
	float m_surfelGIMaxRadius = 5.0f;
	float m_surfelGICoverageThreshold = 0.85f;
	float m_surfelGIRecyclePressure = 0.85f;
	float m_surfelGINormalReject = 0.25f;
	float m_surfelGICoverageNormalReject = 0.86f;
	float m_surfelGICoverageDepthTolerance = 0.0035f;
	float m_surfelGIFinalGatherNormalReject = 0.15f;
	float m_surfelGIRadialDepthVariance = 1.0e-4f;
	float m_surfelGIIntensity = 1.0f;
	float m_surfelGICellAverageFallbackStrength = 0.20f;
	bool m_surfelGIUseRadialDepth = true;
	bool m_surfelGIUseRayGuiding = true;
	bool m_surfelGIUseRayBinning = true;
	bool m_surfelGIUseScreenTrace = true;
	bool m_surfelGIUseSurfelFallbackTrace = true;
	bool m_surfelGIPlacementValidation = false;
	int m_surfelGIRTMaxTriangles = 2000000;
	float m_surfelGIRTBuildBudgetMs = 0.75f;
	int m_surfelGIRTMaxBLASTrianglesPerFrame = 12000;
	int m_surfelGIRTMaxResidentMB = 512;
	bool m_surfelGIRTIncludeSkinnedMeshes = false;
	int m_surfelGITLASHeatmapColorLimit = 50;
	bool m_surfelGITLASDisplayMultipleBVHLayers = false;
	int m_surfelGITLASBVHLayerToDisplay = 0;
	int m_lightingCompositeDebugMode = 0;

	// Screen-space contact shadow settings
	float m_sssResolutionScale = 0.5f;
	float m_sssTemporalAlpha = 0.1f;

	//LPV Global Illumination settings
	bool m_enableLPV = false;
	float m_lpvGIStrength = 1.0f;
	int m_lpvGridResolution = 128;
	float m_lpvVoxelSize = 0.5f;
	int m_lpvRSMResolution = 512;
	int m_lpvVPLSampleCount = 32000;
	int m_lpvPropagationIterations = 5;
	float m_lpvPropagationAttenuation = 0.9f;
	float m_lpvPropagationBias = 0.1f;
	bool m_lpvEnableOcclusion = true;
	int m_lpvUpdateFrequency = 1;
	bool m_lpvDebugVisualization = false;  //Debug visualization toggle
	float m_lpvDebugBoost = 5.0f;          //Debug energy amplification

	// Path Tracing settings
	int m_rendererMode = 0;  // 0 = Deferred Real-time, 1 = Path Traced
	bool m_enablePathTracing = false;
	int m_rtSamplesPerPixel = 4;
	int m_rtMaxBounces = 4;
	float m_rtResolutionScale = 1.0f;
	bool m_rtAccumulate = true;
	bool m_rtDenoise = false;

	// Advanced path tracing - lighting
	bool m_rtEnableNEE = true;
	bool m_rtEnableMIS = true;
	
	// SVGF Denoising settings
	float m_svgfTemporalAlpha = 0.15f;
	float m_svgfVarianceClipGamma = 1.5f;
	float m_svgfDepthThreshold = 0.05f;
	float m_svgfNormalThreshold = 0.9f;
	int m_svgfAtrousIterations = 4;
	float m_svgfPhiColor = 5.0f;
	float m_svgfPhiNormal = 32.0f;
	float m_svgfPhiDepth = 0.01f;
	
	// BVH Debug Visualization
	bool m_rtDisplayBVH = false;
	bool m_rtDisplayMultipleBVHLayers = false;
	int m_rtBVHLayerToDisplay = 0;
	int m_rtHeatmapColorLimit = 50;
	
	// IBL Environment
	bool m_rtEnableIBL = true;
	float m_rtIBLIntensity = 1.0f;

	// Debug settings
	int m_debugMode = 0;
	bool m_wireframeMode = false;
	bool m_showBoundingBoxes = false;
	bool m_showBBoxLegend = true;  // Show color legend when bounding boxes are enabled
	bool m_showLightGizmos = false;
	
	// Rendering overrides
	bool m_forceBackfaceCulling = false;  // Force backface culling on all meshes

	// Quality settings
	int m_textureFiltering = 4;
	float m_lodBias = 0.0f;
	float m_maxDrawDistance = 1000.0f;

	// ModularRenderer reference for real-time control
	std::shared_ptr<ModularRenderer> m_modularRenderer;

	// Helper methods
	void ApplyQualityPreset(int quality);
	void ResetToDefaults();
};
