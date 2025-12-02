#include "RenderingSettingsWindow.h"
#include "../ModularRenderer.h"
#include "../RenderContext.h"
#include "../LightManager.h"
#include <IMGUI/imgui.h>
#include <iostream>

RenderingSettingsWindow::RenderingSettingsWindow()
	: BaseWindow("Rendering Settings", "F6")
{
	m_position = ImVec2(940, 10);
	m_size = ImVec2(320, 400);
	// Engine Resolution
	m_windowResolution = glm::ivec2(1920, 1080);

	// Initialize settings with RenderContext defaults
	m_exposure = 1.0f;
	m_gamma = 2.2f;
	m_enableHDR = true;
	m_envColor = glm::vec3(0.3f, 0.3f, 0.3f);

	// IBL intensity controls
	m_iblIntensity = 0.4f;
	m_skyboxExposure = 1.0f;
	m_diffuseIBLScale = 0.5f;
	m_specularIBLScale = 0.6f;

	// Shadow settings - match RenderContext defaults
	m_enableShadows = true;
	m_shadowBias = 0.005f;
	m_shadowNear = 0.1f;
	m_shadowFar = 1000.0f;
	m_enablePCSS = false;
	m_lightSize = 0.02f;

	// Bloom settings
	m_enableBloom = true;
	m_bloomStrength = 0.8f;
	m_bloomKnee = 0.5f;
	m_bloomThreshold = 1.0f;

	// TAA settings
	m_enableTAA = true;
	m_taaBlendFactor = 0.15f;
	m_taaVarianceThreshold = 0.8f;
	m_taaLumaWeight = 0.2f;
	m_taaUseYCoCg = true;

	// SSAO settings
	m_enableSSAO = false;
	m_ssaoRadius = 0.5f; // Match RenderContext default
	m_ssaoIntensity = 1.0f;

	// SSGI settings
	m_enableSSGI = false;
	m_ssgiStrength = 1.0f;
	m_ssgiRadius = 5.0f;
	m_ssgiSampleCount = 16;
	m_ssgiHalfRes = true;
	m_ssgiTemporalAlpha = 0.15f;
	m_ssgiNormalReject = 0.15f;
	m_ssgiDepthReject = 0.2f;
	m_ssgiThickness = 0.02f;

	//LPV GI settings - match RenderContext defaults
	m_enableLPV = true;
	m_lpvGIStrength = 1.0f;
	m_lpvGridResolution = 128;
	m_lpvVoxelSize = 0.5f;
	m_lpvRSMResolution = 512;
	m_lpvVPLSampleCount = 32000;
	m_lpvPropagationIterations = 5;
	m_lpvPropagationAttenuation = 0.9f;
	m_lpvPropagationBias = 0.1f;
	m_lpvEnableOcclusion = true;
	m_lpvUpdateFrequency = 1;
	m_lpvDebugVisualization = false;
	m_lpvDebugBoost = 5.0f;

	//Path Tracing settings - match RenderContext defaults
	m_rendererMode = 0;  // Deferred Real-time by default
	m_enablePathTracing = false;
	m_rtSamplesPerPixel = 4;
	m_rtMaxBounces = 4;
	m_rtResolutionScale = 1.0f;
	m_rtAccumulate = true;
	m_rtDenoise = false;
	m_rtEnableNEE = true;
	m_rtEnableMIS = true;

	// SVGF Denoising settings
	m_svgfTemporalAlpha = 0.15f;
	m_svgfVarianceClipGamma = 1.5f;
	m_svgfDepthThreshold = 0.05f;
	m_svgfNormalThreshold = 0.9f;
	m_svgfAtrousIterations = 4;
	m_svgfPhiColor = 5.0f;
	m_svgfPhiNormal = 32.0f;
	m_svgfPhiDepth = 0.01f;

	// BVH Debug Visualization
	m_rtDisplayBVH = false;
	m_rtDisplayMultipleBVHLayers = false;
	m_rtBVHLayerToDisplay = 0;
	m_rtHeatmapColorLimit = 50;

	// IBL Environment
	m_rtEnableIBL = true;
	m_rtIBLIntensity = 1.0f;
}

void RenderingSettingsWindow::SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer) {
	m_modularRenderer = renderer;

	// Sync settings from renderer on first connection
	if (m_modularRenderer) {
		SyncFromRenderer();
	}
}

void RenderingSettingsWindow::SyncFromRenderer() {
	if (!m_modularRenderer) return;

	auto& ctx = m_modularRenderer->GetContext();
	// Post-processing settings
	m_exposure = ctx.exposure;
	m_gamma = ctx.gamma;
	m_enableHDR = ctx.enableHDR;
	m_envColor = ctx.envColor;

	// IBL intensity controls
	m_iblIntensity = ctx.iblIntensity;
	m_skyboxExposure = ctx.skyboxExposure;
	m_diffuseIBLScale = ctx.diffuseIBLScale;
	m_specularIBLScale = ctx.specularIBLScale;

	// Tonemapper
	m_tonemapType = static_cast<int>(ctx.tonemapType);
	m_tm_P = ctx.tm_P; m_tm_a = ctx.tm_a; m_tm_m = ctx.tm_m; m_tm_l = ctx.tm_l; m_tm_c = ctx.tm_c; m_tm_b = ctx.tm_b;
	m_outputSRGB = ctx.outputSRGB;

	// Shadow settings
	m_enableShadows = ctx.enableShadows;
	m_shadowBias = ctx.shadowBias;
	m_shadowNear = ctx.shadowNear;
	m_shadowFar = ctx.shadowFar;
	m_enablePCSS = ctx.enablePCSS;
	m_lightSize = ctx.lightSize;

	// Bloom settings
	m_enableBloom = ctx.enableBloom;
	m_bloomStrength = ctx.bloomStrength;

	// TAA settings
	m_enableTAA = ctx.enableTAA;
	m_taaBlendFactor = ctx.taaBlendFactor;
	m_taaVarianceThreshold = ctx.taaVarianceThreshold;
	m_taaLumaWeight = ctx.taaLumaWeight;
	m_taaUseYCoCg = ctx.taaUseYCoCg;

	// SSAO settings
	m_enableSSAO = ctx.enableSSAO;
	m_ssaoRadius = ctx.ssaoRadius;
	m_ssaoIntensity = ctx.ssaoIntensity;

	// SSGI settings
	m_enableSSGI = ctx.enableSSGI;
	m_ssgiStrength = ctx.ssgiStrength;
	m_ssgiRadius = ctx.ssgiRadius;
	m_ssgiSampleCount = ctx.ssgiSampleCount;
	m_ssgiHalfRes = ctx.ssgiHalfRes;
	m_ssgiTemporalAlpha = ctx.ssgiTemporalAlpha;
	m_ssgiNormalReject = ctx.ssgiNormalReject;
	m_ssgiDepthReject = ctx.ssgiDepthReject;
	m_ssgiThickness = ctx.ssgiThickness;

	//LPV settings
	m_enableLPV = ctx.enableLPV;
	m_lpvGIStrength = ctx.lpvGIStrength;
	m_lpvGridResolution = ctx.lpvGridResolution;
	m_lpvVoxelSize = ctx.lpvVoxelSize;
	m_lpvRSMResolution = ctx.lpvRSMResolution;
	m_lpvVPLSampleCount = ctx.lpvVPLSampleCount;
	m_lpvPropagationIterations = ctx.lpvPropagationIterations;
	m_lpvPropagationAttenuation = ctx.lpvPropagationAttenuation;
	m_lpvPropagationBias = ctx.lpvPropagationBias;
	m_lpvEnableOcclusion = ctx.lpvEnableOcclusion;
	m_lpvUpdateFrequency = ctx.lpvUpdateFrequency;
	m_lpvDebugVisualization = ctx.lpvDebugVisualization;
	m_lpvDebugBoost = ctx.lpvDebugBoost;

	//Path Tracing settings
	m_rendererMode = static_cast<int>(ctx.rendererMode);
	m_enablePathTracing = ctx.enablePathTracing;
	m_rtSamplesPerPixel = ctx.rtSamplesPerPixel;
	m_rtMaxBounces = ctx.rtMaxBounces;
	m_rtResolutionScale = ctx.rtResolutionScale;
	m_rtAccumulate = ctx.rtAccumulate;
	m_rtDenoise = ctx.rtDenoise;
	m_rtEnableNEE = ctx.rtEnableNEE;
	m_rtEnableMIS = ctx.rtEnableMIS;

	// SVGF Denoising settings
	m_svgfTemporalAlpha = ctx.svgfTemporalAlpha;
	m_svgfVarianceClipGamma = ctx.svgfVarianceClipGamma;
	m_svgfDepthThreshold = ctx.svgfDepthThreshold;
	m_svgfNormalThreshold = ctx.svgfNormalThreshold;
	m_svgfAtrousIterations = ctx.svgfAtrousIterations;
	m_svgfPhiColor = ctx.svgfPhiColor;
	m_svgfPhiNormal = ctx.svgfPhiNormal;
	m_svgfPhiDepth = ctx.svgfPhiDepth;

	// BVH Debug Visualization
	m_rtDisplayBVH = ctx.rtDisplayBVH;
	m_rtDisplayMultipleBVHLayers = ctx.rtDisplayMultipleBVHLayers;
	m_rtBVHLayerToDisplay = ctx.rtBVHLayerToDisplay;
	m_rtHeatmapColorLimit = ctx.rtHeatmapColorLimit;

	// IBL Environment
	m_rtEnableIBL = ctx.rtEnableIBL;
	m_rtIBLIntensity = ctx.rtIBLIntensity;

	// Debug visualization settings
	m_debugMode = static_cast<int>(ctx.debugMode);
	m_wireframeMode = ctx.wireframeMode;
	m_showBoundingBoxes = ctx.showBoundingBoxes;
	m_showLightGizmos = ctx.showLightGizmos;
}

void RenderingSettingsWindow::SyncToRenderer() {
	if (!m_modularRenderer) return;

	auto& ctx = m_modularRenderer->GetContext();
	// Resolution adjustment based on current resolution preset
	switch (m_resolutionPreset) {
	case 0: // 1920x1080
		ctx.height = 1080;
		ctx.width = 1920;
		break;
	case 1: // 3840x1440
		ctx.height = 1440;
		ctx.width = 3840;
		break;
	};

	// Post-processing settings
	ctx.exposure = m_exposure;
	ctx.gamma = m_gamma;
	ctx.enableHDR = m_enableHDR;
	ctx.envColor = m_envColor;

	// IBL intensity controls
	ctx.iblIntensity = m_iblIntensity;
	ctx.skyboxExposure = m_skyboxExposure;
	ctx.diffuseIBLScale = m_diffuseIBLScale;
	ctx.specularIBLScale = m_specularIBLScale;

	// Tonemapper
	ctx.tonemapType = static_cast<RenderContext::TonemapType>(m_tonemapType);
	ctx.tm_P = m_tm_P; ctx.tm_a = m_tm_a; ctx.tm_m = m_tm_m; ctx.tm_l = m_tm_l; ctx.tm_c = m_tm_c; ctx.tm_b = m_tm_b;
	ctx.outputSRGB = m_outputSRGB;

	// Shadow settings
	ctx.enableShadows = m_enableShadows;
	ctx.shadowBias = m_shadowBias;
	ctx.shadowNear = m_shadowNear;
	ctx.shadowFar = m_shadowFar;
	ctx.enablePCSS = m_enablePCSS;
	ctx.lightSize = m_lightSize;

	// Bloom settings
	ctx.bloomStrength = m_bloomStrength;
	ctx.bloomKnee = m_bloomKnee;
	ctx.bloomThreshold = m_bloomThreshold;

	// TAA settings
	ctx.enableTAA = m_enableTAA;
	ctx.taaBlendFactor = m_taaBlendFactor;
	ctx.taaVarianceThreshold = m_taaVarianceThreshold;
	ctx.taaLumaWeight = m_taaLumaWeight;
	ctx.taaUseYCoCg = m_taaUseYCoCg;

	// SSAO settings
	ctx.enableSSAO = m_enableSSAO;
	ctx.ssaoRadius = m_ssaoRadius;
	ctx.ssaoBias = 0.025f; // Keep default bias
	ctx.ssaoIntensity = m_ssaoIntensity;
	ctx.ssaoBlurDepthThreshold = 0.01f; // Keep default

	// SSGI settings
	ctx.enableSSGI = m_enableSSGI;
	ctx.ssgiStrength = m_ssgiStrength;
	ctx.ssgiRadius = m_ssgiRadius;
	ctx.ssgiSampleCount = m_ssgiSampleCount;
	ctx.ssgiHalfRes = m_ssgiHalfRes;
	ctx.ssgiTemporalAlpha = m_ssgiTemporalAlpha;
	ctx.ssgiNormalReject = m_ssgiNormalReject;
	ctx.ssgiDepthReject = m_ssgiDepthReject;
	ctx.ssgiThickness = m_ssgiThickness;

	//LPV settings
	ctx.enableLPV = m_enableLPV;
	ctx.lpvGIStrength = m_lpvGIStrength;
	ctx.lpvGridResolution = m_lpvGridResolution;
	ctx.lpvVoxelSize = m_lpvVoxelSize;
	ctx.lpvRSMResolution = m_lpvRSMResolution;
	ctx.lpvVPLSampleCount = m_lpvVPLSampleCount;
	ctx.lpvPropagationIterations = m_lpvPropagationIterations;
	ctx.lpvPropagationAttenuation = m_lpvPropagationAttenuation;
	ctx.lpvPropagationBias = m_lpvPropagationBias;
	ctx.lpvEnableOcclusion = m_lpvEnableOcclusion;
	ctx.lpvUpdateFrequency = m_lpvUpdateFrequency;
	ctx.lpvDebugVisualization = m_lpvDebugVisualization;
	ctx.lpvDebugBoost = m_lpvDebugBoost;

	//Path Tracing settings
	ctx.rendererMode = static_cast<RenderContext::RendererMode>(m_rendererMode);
	ctx.enablePathTracing = m_enablePathTracing;
	ctx.rtSamplesPerPixel = m_rtSamplesPerPixel;
	ctx.rtMaxBounces = m_rtMaxBounces;
	ctx.rtResolutionScale = m_rtResolutionScale;
	ctx.rtAccumulate = m_rtAccumulate;
	ctx.rtDenoise = m_rtDenoise;
	ctx.rtEnableNEE = m_rtEnableNEE;
	ctx.rtEnableMIS = m_rtEnableMIS;

	// SVGF Denoising settings
	ctx.svgfTemporalAlpha = m_svgfTemporalAlpha;
	ctx.svgfVarianceClipGamma = m_svgfVarianceClipGamma;
	ctx.svgfDepthThreshold = m_svgfDepthThreshold;
	ctx.svgfNormalThreshold = m_svgfNormalThreshold;
	ctx.svgfAtrousIterations = m_svgfAtrousIterations;
	ctx.svgfPhiColor = m_svgfPhiColor;
	ctx.svgfPhiNormal = m_svgfPhiNormal;
	ctx.svgfPhiDepth = m_svgfPhiDepth;

	// BVH Debug Visualization
	ctx.rtDisplayBVH = m_rtDisplayBVH;
	ctx.rtDisplayMultipleBVHLayers = m_rtDisplayMultipleBVHLayers;
	ctx.rtBVHLayerToDisplay = m_rtBVHLayerToDisplay;
	ctx.rtHeatmapColorLimit = m_rtHeatmapColorLimit;

	// IBL Environment
	ctx.rtEnableIBL = m_rtEnableIBL;
	ctx.rtIBLIntensity = m_rtIBLIntensity;

	// Debug visualization settings
	ctx.debugMode = static_cast<RenderContext::DebugMode>(m_debugMode);
	ctx.wireframeMode = m_wireframeMode;
	ctx.showBoundingBoxes = m_showBoundingBoxes;
	ctx.showLightGizmos = m_showLightGizmos;

	std::cout << "[RenderingSettings] Synced to renderer - Exposure: " << m_exposure
		<< ", Gamma: " << m_gamma << ", TM: " << m_tonemapType
		<< ", Bloom: " << (m_enableBloom ? "ON" : "OFF")
		<< ", SSAO: " << (m_enableSSAO ? "ON" : "OFF") << std::endl;
}

void RenderingSettingsWindow::Render() {
	BeginWindow();
	if (!IsVisible()) return;

	ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "Rendering Pipeline");
	ImGui::Separator();

	// Show connection status
	if (m_modularRenderer) {
		ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Connected to ModularRenderer");
	}
	else {
		ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "✗ Not connected to renderer");
	}
	ImGui::Separator();

	if (ImGui::BeginTabBar("RenderingTabs")) {
		//General Settings Tab
		if (ImGui::BeginTabItem("General")) {
			// Future general settings can be added here
			ImGui::Text("Engine Resolution:");
			ImGui::Text("%d x %d", m_windowResolution.x, m_windowResolution.y);
			//show multiple predefined resolutions
			const char* resolutions[] = { "1920x1080","3840x1440" };
			if (ImGui::Combo("Resolution", &m_resolutionPreset, resolutions, IM_ARRAYSIZE(resolutions))) {
				SyncToRenderer();
			}
			ImGui::EndTabItem();
		}
		// Post-Processing Tab
		if (ImGui::BeginTabItem("Post-Process")) {
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Tone Mapping & Exposure:");

			if (ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 5.0f, "%.2f")) {
				SyncToRenderer();
			}

			if (ImGui::SliderFloat("Gamma", &m_gamma, 1.0f, 3.0f, "%.2f")) {
				SyncToRenderer();
			}

			// Tonemapper selection and parameters
			const char* tmItems[] = { "None", "ACES", "Filmic (GT)" , "GT7" };
			if (ImGui::Combo("Tonemapper", &m_tonemapType, tmItems, IM_ARRAYSIZE(tmItems))) {
				SyncToRenderer();
			}
			if (m_tonemapType == 2) {
				if (ImGui::SliderFloat("P", &m_tm_P, 0.5f, 2.0f)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("a", &m_tm_a, 0.5f, 2.0f)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("m", &m_tm_m, 0.0f, 0.5f)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("l", &m_tm_l, 0.0f, 1.0f)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("c", &m_tm_c, 0.5f, 3.0f)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("b", &m_tm_b, 0.0f, 0.1f)) { SyncToRenderer(); }
			}
			else if (m_tonemapType == 3) {
				if (ImGui::SliderFloat("Peak Nits", &m_tm7_peakNits, 250.0f, 10000.0f, "%.0f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Blend", &m_tm7_blend, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Fade Start", &m_tm7_fadeStart, 0.8f, 1.2f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Fade End", &m_tm7_fadeEnd, 0.9f, 1.3f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::Checkbox("Use Jzazbz UCS", &m_tm7_useJzazbz)) { SyncToRenderer(); }
			}
			if (ImGui::Checkbox("Output sRGB", &m_outputSRGB)) { SyncToRenderer(); }

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Environment:");

			if (ImGui::ColorEdit3("Sky Color", &m_envColor.x)) {
				SyncToRenderer();
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "IBL (Image-Based Lighting):");
			ImGui::Text("Control skybox and environment lighting intensity");

			if (ImGui::SliderFloat("IBL Overall Intensity", &m_iblIntensity, 0.0f, 2.0f, "%.2f")) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##ibl_intensity")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Overall multiplier for IBL contribution\nReduces over-bright HDR environment lighting");
			}

			if (ImGui::SliderFloat("Skybox Background Exposure", &m_skyboxExposure, 0.1f, 5.0f, "%.2f")) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##skybox_exposure")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Exposure for skybox background only\nDoes not affect lighting, only visual brightness");
			}

			if (ImGui::SliderFloat("Diffuse IBL Scale", &m_diffuseIBLScale, 0.0f, 2.0f, "%.2f")) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##diffuse_ibl")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Scale for diffuse irradiance contribution\nControls ambient/indirect diffuse lighting");
			}

			if (ImGui::SliderFloat("Specular IBL Scale", &m_specularIBLScale, 0.0f, 2.0f, "%.2f")) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##specular_ibl")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Scale for specular prefiltered contribution\nControls environment reflections");
			}

			if (ImGui::Button("Reset IBL to Defaults")) {
				m_iblIntensity = 0.4f;
				m_skyboxExposure = 1.0f;
				m_diffuseIBLScale = 0.5f;
				m_specularIBLScale = 0.6f;
				SyncToRenderer();
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Bloom Effect:");

			if (ImGui::Checkbox("Enable Bloom", &m_enableBloom)) {
				SyncToRenderer();
			}

			if (m_enableBloom) {
				if (ImGui::SliderFloat("Bloom Strength", &m_bloomStrength, 0.0f, 2.0f, "%.2f")) {
					SyncToRenderer();
				}
				if (ImGui::SliderFloat("Bloom Knee", &m_bloomKnee, 0.0f, 1.0f, "%.2f")) {
					SyncToRenderer();
				}
				if (ImGui::SliderFloat("Bloom Threshold", &m_bloomThreshold, 0.0f, 5.0f, "%.2f")) {
					SyncToRenderer();
				}
			}

			ImGui::EndTabItem();
		}

		// Shadows Tab
		if (ImGui::BeginTabItem("Shadows")) {
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Shadow Mapping:");

			if (ImGui::Checkbox("Enable Shadows", &m_enableShadows)) {
				SyncToRenderer();
			}

			if (m_enableShadows) {
				if (ImGui::SliderFloat("Shadow Bias", &m_shadowBias, 0.0001f, 0.01f, "%.5f")) {
					SyncToRenderer(); // Apply immediately
				}

				if (ImGui::SliderFloat("Shadow Near", &m_shadowNear, 0.1f, 10.0f, "%.2f")) {
					SyncToRenderer(); // Apply immediately
				}
				if (ImGui::SliderFloat("Shadow Far", &m_shadowFar, 10.0f, 500.0f, "%.1f")) {
					SyncToRenderer(); // Apply immediately
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Shadow Quality:");

				const char* shadowSizes[] = { "512", "1024", "2048", "4096" };
				int currentShadowSize = 1; // Default to 1024
				if (ImGui::Combo("Shadow Resolution", &currentShadowSize, shadowSizes, 4)) {
					// Shadow resolution change
				}

				if (ImGui::Checkbox("Enable PCSS", &m_enablePCSS)) {
					SyncToRenderer(); // Apply immediately
				}

				if (m_enablePCSS) {
					if (ImGui::SliderFloat("Light Size", &m_lightSize, 0.01f, 1.0f, "%.3f")) {
						SyncToRenderer(); // Apply immediately
					}
				}
			}

			ImGui::EndTabItem();
		}

		// Anti-Aliasing Tab
		if (ImGui::BeginTabItem("Anti-Aliasing")) {
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Temporal Anti-Aliasing:");

			if (ImGui::Checkbox("Enable TAA", &m_enableTAA)) {
				SyncToRenderer(); // Apply immediately

				// Reset TAA history when toggling
				if (m_modularRenderer) {
					m_modularRenderer->ResetTAA();
				}
			}

			if (m_enableTAA) {
				if (ImGui::SliderFloat("TAA Blend Factor", &m_taaBlendFactor, 0.05f, 0.3f, "%.3f")) {
					SyncToRenderer(); // Apply immediately
				}

				if (m_modularRenderer) {
					int taaFrame = m_modularRenderer->GetTAAFrameIndex();
					ImGui::Text("TAA Frame Index: %d", taaFrame);
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Advanced TAA Settings:");

				if (ImGui::SliderFloat("Variance Threshold", &m_taaVarianceThreshold, 0.1f, 2.0f, "%.2f")) {
					SyncToRenderer();
				}

				if (ImGui::SliderFloat("Luma Weight", &m_taaLumaWeight, 0.0f, 1.0f, "%.2f")) {
					SyncToRenderer();
				}

				if (ImGui::Checkbox("Use YCoCg Color Space", &m_taaUseYCoCg)) {
					SyncToRenderer();
				}

				if (ImGui::Button("Reset TAA History")) {
					if (m_modularRenderer) {
						m_modularRenderer->ResetTAA();
					}
				}
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "SSAO (Screen Space Ambient Occlusion):");

			if (ImGui::Checkbox("Enable SSAO", &m_enableSSAO)) {
				SyncToRenderer(); // Apply immediately
			}

			if (m_enableSSAO) {
				if (ImGui::SliderFloat("SSAO Radius", &m_ssaoRadius, 0.1f, 2.0f, "%.2f")) {
					SyncToRenderer(); // Apply immediately
				}

				if (ImGui::SliderFloat("SSAO Intensity", &m_ssaoIntensity, 0.0f, 2.0f, "%.2f")) {
					SyncToRenderer(); // Apply immediately
				}
			}

			ImGui::EndTabItem();
		}

		//Global Illumination Tab
		if (ImGui::BeginTabItem("Global Illumination")) {
			ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "Screen Space Global Illumination (SSGI):");
			if (ImGui::Checkbox("Enable SSGI", &m_enableSSGI)) { SyncToRenderer(); }
			if (m_enableSSGI) {
				if (ImGui::SliderFloat("SSGI Strength", &m_ssgiStrength, 0.0f, 3.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("SSGI Radius (VS)", &m_ssgiRadius, 0.1f, 5.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderInt("SSGI Samples", &m_ssgiSampleCount, 8, 256)) { SyncToRenderer(); }
				if (ImGui::Checkbox("Half Resolution", &m_ssgiHalfRes)) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Temporal Alpha", &m_ssgiTemporalAlpha, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Normal Reject", &m_ssgiNormalReject, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Depth Reject", &m_ssgiDepthReject, 0.0f, 2.0f, "%.2f")) { SyncToRenderer(); }
				if (ImGui::SliderFloat("Thickness", &m_ssgiThickness, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "Light Propagation Volumes (LPV):");
			ImGui::Text("Real-time dynamic global illumination");
			ImGui::Separator();

			if (ImGui::Checkbox("Enable LPV GI", &m_enableLPV)) {
				SyncToRenderer();
			}

			if (m_enableLPV) {
				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Quality Settings:");

				if (ImGui::SliderFloat("GI Strength", &m_lpvGIStrength, 0.0f, 3.0f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_gi_strength")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Multiplier for global illumination contribution");
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Grid Configuration:");

				const char* gridSizes[] = { "64^3 (Fast)", "128^3 (Balanced)", "256^3 (Quality)" };
				int currentGridSize = (m_lpvGridResolution == 64) ? 0 : (m_lpvGridResolution == 128) ? 1 : 2;
				if (ImGui::Combo("Grid Resolution", &currentGridSize, gridSizes, 3)) {
					m_lpvGridResolution = (currentGridSize == 0) ? 64 : (currentGridSize == 1) ? 128 : 256;
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_grid_res")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Higher resolution = better quality but slower\n64^3 = 262K voxels\n128^3 = 2M voxels\n256^3 = 16M voxels");
				}

				if (ImGui::SliderFloat("Voxel Size (m)", &m_lpvVoxelSize, 0.1f, 2.0f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_voxel_size")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("World-space size of each voxel\nSmaller = more detail, smaller coverage");
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Reflective Shadow Map:");

				const char* rsmSizes[] = { "256x256 (Fast)", "512x512 (Balanced)", "1024x1024 (Quality)" };
				int currentRSMSize = (m_lpvRSMResolution == 256) ? 0 : (m_lpvRSMResolution == 512) ? 1 : 2;
				if (ImGui::Combo("RSM Resolution", &currentRSMSize, rsmSizes, 3)) {
					m_lpvRSMResolution = (currentRSMSize == 0) ? 256 : (currentRSMSize == 1) ? 512 : 1024;
					SyncToRenderer();
				}

				if (ImGui::SliderInt("VPL Samples", &m_lpvVPLSampleCount, 8000, 64000, "%d")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_vpl_samples")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Number of Virtual Point Lights injected into grid\nMore samples = better quality but slower");
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Light Propagation:");

				if (ImGui::SliderInt("Propagation Iterations", &m_lpvPropagationIterations, 1, 10)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_prop_iter")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Number of light bounce iterations\n4-6 iterations recommended for realistic GI");
				}

				if (ImGui::SliderFloat("Attenuation", &m_lpvPropagationAttenuation, 0.5f, 1.0f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_atten")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Energy preserved per propagation step\n0.9 = 10% loss per bounce");
				}

				if (ImGui::SliderFloat("Directional Bias", &m_lpvPropagationBias, 0.0f, 0.5f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_bias")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Bias light propagation along initial light direction\n0.0 = omnidirectional, 0.5 = strong directional");
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Advanced Options:");

				if (ImGui::Checkbox("Enable Occlusion", &m_lpvEnableOcclusion)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_occlusion")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Use geometry volume to block light propagation\nMore realistic but slightly slower");
				}

				if (ImGui::SliderInt("Update Frequency", &m_lpvUpdateFrequency, 1, 10)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_update_freq")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Update LPV every N frames\n1 = every frame (most dynamic)\n5 = every 5 frames (better performance)");
				}

				//Debug visualization toggle
				if (ImGui::Checkbox("Debug Visualization", &m_lpvDebugVisualization)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##lpv_debug_viz")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Show LPV voxel grid as colored points\nRed = geometry, Colored = light energy, Gray = empty");
				}

				if (m_lpvDebugVisualization) {
					if (ImGui::SliderFloat("Debug Boost", &m_lpvDebugBoost, 1.0f, 10.0f, "%.1fx")) {
						SyncToRenderer();
					}
					ImGui::SameLine();
					if (ImGui::Button("?##lpv_debug_boost")) {}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Amplify LPV energy visualization for debugging\nIncrease if voxels are hard to see");
					}
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Quality Presets:");

				if (ImGui::Button("Performance")) {
					m_lpvGridResolution = 64;
					m_lpvVoxelSize = 1.0f;
					m_lpvRSMResolution = 256;
					m_lpvVPLSampleCount = 16000;
					m_lpvPropagationIterations = 3;
					m_lpvUpdateFrequency = 3;
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("Balanced")) {
					m_lpvGridResolution = 128;
					m_lpvVoxelSize = 0.5f;
					m_lpvRSMResolution = 512;
					m_lpvVPLSampleCount = 32000;
					m_lpvPropagationIterations = 5;
					m_lpvUpdateFrequency = 1;
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("Quality")) {
					m_lpvGridResolution = 256;
					m_lpvVoxelSize = 0.25f;
					m_lpvRSMResolution = 1024;
					m_lpvVPLSampleCount = 64000;
					m_lpvPropagationIterations = 8;
					m_lpvUpdateFrequency = 1;
					SyncToRenderer();
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Performance Info:");
				ImGui::BulletText("Grid: %d^3 = %d voxels", m_lpvGridResolution,
					m_lpvGridResolution * m_lpvGridResolution * m_lpvGridResolution);
				ImGui::BulletText("Coverage: %.1f x %.1f x %.1f meters",
					m_lpvGridResolution * m_lpvVoxelSize,
					m_lpvGridResolution * m_lpvVoxelSize,
					m_lpvGridResolution * m_lpvVoxelSize);
				ImGui::BulletText("VPL Count: %d samples", m_lpvVPLSampleCount);
				ImGui::BulletText("Compute Dispatches: %d per frame", m_lpvPropagationIterations + 1);
			}

			ImGui::EndTabItem();
		}

		//Path Tracing Tab
		if (ImGui::BeginTabItem("Path Tracing")) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f), "Path Traced Rendering:");
			ImGui::Text("High-quality offline-style rendering mode");
			ImGui::Separator();

			// Renderer mode selection
			const char* rendererModes[] = { "Deferred Real-Time", "Path Traced (Offline Quality)" };
			if (ImGui::Combo("Renderer Mode", &m_rendererMode, rendererModes, 2)) {
				SyncToRenderer();
				std::cout << "[RenderingSettings] Switched to " << rendererModes[m_rendererMode] << std::endl;
			}
			ImGui::SameLine();
			if (ImGui::Button("?##renderer_mode")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Deferred Real-Time: Standard pipeline with SSAO, SSGI, shadows\n"
					"Path Traced: BVH-accelerated bidirectional path tracing\n"
					"  - Converges over time when camera is still\n"
					"  - Physically accurate lighting and reflections\n"
					"  - Higher quality, slower performance"
				);
			}

			// Show warning if path tracing is enabled
			if (m_rendererMode == 1) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
				ImGui::TextWrapped("⚠ Path tracing mode is active. Performance will be reduced.");
				ImGui::TextWrapped("Camera movement will reset accumulation.");
				ImGui::PopStyleColor();
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Path Tracing Quality:");

			if (ImGui::SliderInt("Samples Per Pixel", &m_rtSamplesPerPixel, 1, 16)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_spp")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Number of rays traced per pixel per frame\n"
					"Higher values = less noise but slower\n"
					"1-4: Fast, noisy (use temporal accumulation)\n"
					"8-16: Slower, cleaner per-frame result"
				);
			}

			if (ImGui::SliderInt("Max Ray Bounces", &m_rtMaxBounces, 1, 8)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_bounces")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Maximum number of ray bounces for indirect lighting\n"
					"2-4: Fast, good for most scenes\n"
					"5-8: Accurate caustics and deep reflections"
				);
			}

			if (ImGui::SliderFloat("Resolution Scale", &m_rtResolutionScale, 0.25f, 1.0f, "%.2f")) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_res_scale")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Render path tracing at reduced resolution\n"
					"0.5 = half resolution (4x faster)\n"
					"0.25 = quarter resolution (16x faster)\n"
					"1.0 = full resolution (highest quality)"
				);
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Temporal Features:");

			if (ImGui::Checkbox("Enable Accumulation", &m_rtAccumulate)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_accumulate")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Accumulate samples over multiple frames\n"
					"Converges to noise-free result when camera is still\n"
					"Automatically resets on camera movement"
				);
			}

			if (ImGui::Checkbox("Enable Denoising (Future)", &m_rtDenoise)) {
				m_rtDenoise = false; // Not implemented yet
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_denoise")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("AI-based denoising (Not yet implemented)");
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Advanced Lighting:");

			if (ImGui::Checkbox("Next Event Estimation (NEE)", &m_rtEnableNEE)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_nee")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Explicitly sample lights for direct lighting\n"
					"Dramatically reduces noise and improves convergence\n"
					"Should always be enabled for best results"
				);
			}

			if (ImGui::Checkbox("Multiple Importance Sampling (MIS)", &m_rtEnableMIS)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_mis")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Balance between BSDF and light sampling\n"
					"Improves quality with complex lighting\n"
					"Minimal performance cost, recommended"
				);
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "SVGF Denoising:");

			if (ImGui::Checkbox("Enable Denoising", &m_rtDenoise)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_denoise")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Spatiotemporal Variance-Guided Filtering\n"
					"Edge-aware temporal and spatial denoising\n"
					"Dramatically reduces noise at low sample counts"
				);
			}

			if (m_rtDenoise) {
				if (ImGui::SliderFloat("Temporal Alpha", &m_svgfTemporalAlpha, 0.05f, 0.3f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##svgf_temporal")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Blend factor for temporal reprojection\nLower = more stable, Higher = less ghosting");
				}

				if (ImGui::SliderInt("Filter Iterations", &m_svgfAtrousIterations, 1, 6)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##svgf_iterations")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Number of à-trous filter passes\nMore = smoother but may over-blur");
				}

				if (ImGui::SliderFloat("Color Sensitivity", &m_svgfPhiColor, 1.0f, 20.0f, "%.1f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##svgf_color")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Color edge preservation\nLower = sharper edges, Higher = more smoothing");
				}

				if (ImGui::SliderFloat("Normal Sensitivity", &m_svgfPhiNormal, 8.0f, 128.0f, "%.1f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##svgf_normal")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Normal edge preservation\nHigher = sharper geometric edges");
				}

				if (ImGui::SliderFloat("Depth Sensitivity", &m_svgfPhiDepth, 0.001f, 0.1f, "%.3f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##svgf_depth")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Depth edge preservation\nLower = sharper depth discontinuities");
				}
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "BVH Debug Visualization:");

			if (ImGui::Checkbox("Show BVH Heatmap", &m_rtDisplayBVH)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_bvh_viz")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Visualize BVH traversal complexity\n"
					"Replaces normal rendering with a heatmap:\n"
					"  Blue/Black = Few tests (efficient)\n"
					"  Green/Yellow = Moderate tests\n"
					"  Red/White = Many tests (slow)\n"
					"Helps identify BVH performance bottlenecks"
				);
			}

			if (m_rtDisplayBVH) {
				if (ImGui::SliderInt("Heatmap Color Limit", &m_rtHeatmapColorLimit, 10, 200)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##rt_heatmap_limit")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Maximum complexity value for color mapping\n"
						"Lower = more sensitive (shows detail)\n"
						"Higher = less sensitive (shows only hotspots)\n"
						"Adjust based on scene complexity"
					);
				}

				if (ImGui::Checkbox("Show Multiple Layers", &m_rtDisplayMultipleBVHLayers)) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##rt_bvh_layers")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Cycle through BVH tree depth layers (not yet implemented)");
				}

				if (m_rtDisplayMultipleBVHLayers) {
					if (ImGui::SliderInt("Layer to Display", &m_rtBVHLayerToDisplay, 0, 10)) {
						SyncToRenderer();
					}
				}
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Environment Lighting (IBL):");

			if (ImGui::Checkbox("Enable IBL Sampling", &m_rtEnableIBL)) {
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("?##rt_ibl")) {}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Sample HDR environment map for lighting\n"
					"  - Sky background on ray miss\n"
					"  - Environment as light source (NEE)\n"
					"  - Physically accurate IBL integration\n"
					"Requires valid skybox to be loaded"
				);
			}

			if (m_rtEnableIBL) {
				if (ImGui::SliderFloat("IBL Intensity", &m_rtIBLIntensity, 0.0f, 5.0f, "%.2f")) {
					SyncToRenderer();
				}
				ImGui::SameLine();
				if (ImGui::Button("?##rt_ibl_intensity")) {}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Multiplier for environment lighting contribution\n"
						"1.0 = Physical accuracy\n"
						">1.0 = Brighter environment\n"
						"<1.0 = Dimmer environment"
					);
				}
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Quality Presets:");

			if (ImGui::Button("Preview (Fast)")) {
				m_rtSamplesPerPixel = 1;
				m_rtMaxBounces = 2;
				m_rtResolutionScale = 0.5f;
				m_rtAccumulate = true;
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("Balanced")) {
				m_rtSamplesPerPixel = 4;
				m_rtMaxBounces = 4;
				m_rtResolutionScale = 1.0f;
				m_rtAccumulate = true;
				SyncToRenderer();
			}
			ImGui::SameLine();
			if (ImGui::Button("Production")) {
				m_rtSamplesPerPixel = 8;
				m_rtMaxBounces = 6;
				m_rtResolutionScale = 1.0f;
				m_rtAccumulate = true;
				SyncToRenderer();
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Performance Info:");

			if (m_modularRenderer) {
				auto& ctx = m_modularRenderer->GetContext();

				// Show BVH stats if available
				ImGui::BulletText("BVH Acceleration: Active");
				ImGui::BulletText("Algorithm: Bidirectional Path Tracing");
				ImGui::BulletText("Starting from: G-Buffer surfaces");

				// Estimated performance impact
				float perfImpact = m_rtSamplesPerPixel * m_rtMaxBounces * m_rtResolutionScale * m_rtResolutionScale;
				ImGui::BulletText("Relative Cost: %.1fx realtime", perfImpact);

				if (m_rtAccumulate) {
					ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
						"✓ Temporal accumulation will smooth out noise");
				}
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Usage Tips:");
			ImGui::BulletText("Move camera to desired view");
			ImGui::BulletText("Switch to Path Traced mode");
			ImGui::BulletText("Wait for convergence (image clears up)");
			ImGui::BulletText("Switch back to Real-Time for navigation");

			ImGui::EndTabItem();
		}

		// Debug Tab
		if (ImGui::BeginTabItem("Debug")) {
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Debug Visualization:");

			const char* debugModes[] = { "None", "Albedo", "Normal", "Depth", "Shadow Maps", "Motion Vectors" };
			if (ImGui::Combo("Debug Mode", &m_debugMode, debugModes, 6)) {
				SyncToRenderer(); // Apply debug mode to renderer
			}

			ImGui::Separator();

			if (ImGui::Checkbox("Wireframe Mode", &m_wireframeMode)) {
				SyncToRenderer(); // Apply wireframe toggle
			}

			if (ImGui::Checkbox("Show Bounding Boxes", &m_showBoundingBoxes)) {
				SyncToRenderer(); // Apply bounding box toggle
			}

			if (ImGui::Checkbox("Show Light Gizmos", &m_showLightGizmos)) {
				SyncToRenderer(); // Apply light gizmo toggle
			}

			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Renderer Info:");

			if (m_modularRenderer) {
				auto& ctx = m_modularRenderer->GetContext();
				ImGui::Text("Modular Renderer Connected");
				ImGui::BulletText("Resolution: %dx%d", ctx.width, ctx.height);
				ImGui::BulletText("G-Buffer: Extended (6 attachments)");
				ImGui::BulletText("HDR Pipeline: Active");
				ImGui::BulletText("TAA: %s", ctx.enableTAA ? "Enabled" : "Disabled");
				ImGui::BulletText("Shadows: %s", ctx.enableShadows ? "Enabled" : "Disabled");

				if (ctx.lightManager) {
					ImGui::BulletText("Active Lights: %d", ctx.lightManager->GetActiveLightCount());
				}
			}
			else {
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No renderer connected");
				ImGui::Text("Connect ModularRenderer via");
				ImGui::Text("SetModularRenderer()");

				// Debug: Show all settings values
				ImGui::Separator();
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.8f, 1.0f), "Current Settings:");
				ImGui::Text("Exposure: %.2f", m_exposure);
				ImGui::Text("Gamma: %.2f", m_gamma);
				ImGui::Text("HDR: %s", m_enableHDR ? "Enabled" : "Disabled");
				ImGui::ColorEdit3("Sky Color", &m_envColor.x);
				ImGui::Text("Shadows: %s", m_enableShadows ? "Enabled" : "Disabled");
				ImGui::Text("Bloom: %s", m_enableBloom ? "Enabled" : "Disabled");
				ImGui::Text("TAA: %s", m_enableTAA ? "Enabled" : "Disabled");
				ImGui::Text("SSAO: %s", m_enableSSAO ? "Enabled" : "Disabled");

				// Show Tonemapper settings
				ImGui::Separator();
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Tonemapper Settings:");
				ImGui::Text("Type: %s",
					(m_tonemapType == 0) ? "None" :
					(m_tonemapType == 1) ? "ACES" :
					(m_tonemapType == 2) ? "Filmic (GT)" :
					"GT7");
				ImGui::Text("P: %.2f", m_tm_P);
				ImGui::Text("a: %.2f", m_tm_a);
				ImGui::Text("m: %.2f", m_tm_m);
				ImGui::Text("l: %.2f", m_tm_l);
				ImGui::Text("c: %.2f", m_tm_c);
				ImGui::Text("b: %.2f", m_tm_b);
				ImGui::Text("Output sRGB: %s", m_outputSRGB ? "Enabled" : "Disabled");

				// Show LPV settings
				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "LPV Global Illumination Settings:");
				ImGui::Text("Enable LPV: %s", m_enableLPV ? "Yes" : "No");
				if (m_enableLPV) {
					ImGui::Text("GI Strength: %.2f", m_lpvGIStrength);
					ImGui::Text("Grid Resolution: %d^3", m_lpvGridResolution);
					ImGui::Text("Voxel Size: %.2f m", m_lpvVoxelSize);
					ImGui::Text("RSM Resolution: %d", m_lpvRSMResolution);
					ImGui::Text("VPL Samples: %d", m_lpvVPLSampleCount);
					ImGui::Text("Propagation Iterations: %d", m_lpvPropagationIterations);
					ImGui::Text("Update Frequency: %d", m_lpvUpdateFrequency);
					ImGui::Text("Debug Visualization: %s", m_lpvDebugVisualization ? "On" : "Off");
				}
			}

			ImGui::EndTabItem();
		}

		// Quality Tab
		if (ImGui::BeginTabItem("Quality")) {
			ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Rendering Quality:");

			const char* qualityPresets[] = { "Low", "Medium", "High", "Ultra" };
			int currentQuality = 2; // Default to High
			if (ImGui::Combo("Quality Preset", &currentQuality, qualityPresets, 4)) {
				ApplyQualityPreset(currentQuality);
				SyncToRenderer(); // Apply preset to renderer
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	// Reset button
	ImGui::Separator();
	if (ImGui::Button("Reset to Defaults")) {
		ResetToDefaults();
		SyncToRenderer(); // Apply defaults to renderer
	}

	ImGui::SameLine();
	if (ImGui::Button("Sync from Renderer")) {
		SyncFromRenderer();
	}

	EndWindow();
}

void RenderingSettingsWindow::ApplyQualityPreset(int quality) {
	switch (quality) {
	case 0: // Low
		m_enableShadows = false;
		m_enableBloom = false;
		m_enableTAA = false;
		m_enableSSAO = false;
		m_textureFiltering = 1; // Linear
		break;
	case 1: // Medium
		m_enableShadows = true;
		m_enableBloom = false;
		m_enableTAA = false;
		m_enableSSAO = false;
		m_textureFiltering = 2; // Trilinear
		break;
	case 2: // High
		m_enableShadows = true;
		m_enableBloom = true;
		m_enableTAA = true;
		m_enableSSAO = true;
		m_textureFiltering = 3; // Anisotropic
		break;
	case 3: // Ultra
		m_enableShadows = true;
		m_enableBloom = true;
		m_enableTAA = true;
		m_enableSSAO = true;
		m_textureFiltering = 3; // Anisotropic
		m_shadowBias = 0.0003f; // Tighter shadows
		break;
	default:
		break;
	}
}

void RenderingSettingsWindow::ResetToDefaults() {
	// Post-processing - match RenderContext defaults
	m_exposure = 1.0f;
	m_gamma = 2.2f;
	m_enableHDR = true;
	m_envColor = glm::vec3(0.3f, 0.3f, 0.3f);

	// IBL intensity controls
	m_iblIntensity = 0.4f;
	m_skyboxExposure = 1.0f;
	m_diffuseIBLScale = 0.5f;
	m_specularIBLScale = 0.6f;

	// Shadows - match RenderContext defaults
	m_enableShadows = true;
	m_shadowBias = 0.0008f; // Increased to compensate for no normal offset
	m_shadowNear = 0.1f;
	m_shadowFar = 1000.0f;
	m_enablePCSS = false;
	m_lightSize = 0.02f;

	// Bloom - match RenderContext defaults
	m_enableBloom = true;
	m_bloomStrength = 0.8f;
	m_bloomKnee = 0.5f;
	m_bloomThreshold = 1.0f;

	// TAA - match RenderContext defaults
	m_enableTAA = true;
	m_taaBlendFactor = 0.15f;
	m_taaVarianceThreshold = 0.8f;
	m_taaLumaWeight = 0.2f;
	m_taaUseYCoCg = true;

	// SSAO - match RenderContext defaults
	m_enableSSAO = false;
	m_ssaoRadius = 0.5f;
	m_ssaoIntensity = 1.0f;

	//LPV - match RenderContext defaults
	m_enableLPV = true;
	m_lpvGIStrength = 1.0f;
	m_lpvGridResolution = 128;
	m_lpvVoxelSize = 0.5f;
	m_lpvRSMResolution = 512;
	m_lpvVPLSampleCount = 32000;
	m_lpvPropagationIterations = 5;
	m_lpvPropagationAttenuation = 0.9f;
	m_lpvPropagationBias = 0.1f;
	m_lpvEnableOcclusion = true;
	m_lpvUpdateFrequency = 1;
	m_lpvDebugVisualization = false;
	m_lpvDebugBoost = 5.0f;

	//Path Tracing settings - match RenderContext defaults
	m_rendererMode = 0;  // Deferred Real-time by default
	m_enablePathTracing = false;
	m_rtSamplesPerPixel = 4;
	m_rtMaxBounces = 4;
	m_rtResolutionScale = 1.0f;
	m_rtAccumulate = true;
	m_rtDenoise = false;
	m_rtEnableNEE = true;
	m_rtEnableMIS = true;

	// SVGF Denoising settings
	m_svgfTemporalAlpha = 0.15f;
	m_svgfVarianceClipGamma = 1.5f;
	m_svgfDepthThreshold = 0.05f;
	m_svgfNormalThreshold = 0.9f;
	m_svgfAtrousIterations = 4;
	m_svgfPhiColor = 5.0f;
	m_svgfPhiNormal = 32.0f;
	m_svgfPhiDepth = 0.01f;

	// Debug
	m_debugMode = 0;
	m_wireframeMode = false;
	m_showBoundingBoxes = false;
	m_showLightGizmos = false;

	// Quality
	m_textureFiltering = 4;
	m_lodBias = 0.0f;
	m_maxDrawDistance = 1000.0f;

	std::cout << "[RenderingSettings] Reset all settings to RenderContext defaults (including LPV)" << std::endl;
}