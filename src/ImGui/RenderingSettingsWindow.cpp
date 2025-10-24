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
    
    // Initialize settings with RenderContext defaults
    m_exposure = 1.0f;
    m_gamma = 2.2f; 
    m_enableHDR = true;
    m_envColor = glm::vec3(0.3f, 0.3f, 0.3f);
    
    // Shadow settings - match RenderContext defaults
    m_enableShadows = true;
    m_shadowBias = 0.0008f; // Increased to compensate for no normal offset
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
    m_ssgiRadius = 1.0f;
    m_ssgiSampleCount = 64;
    m_ssgiHalfRes = false;
    m_ssgiTemporalAlpha = 0.8f;
    m_ssgiNormalReject = 0.5f;
    m_ssgiDepthReject = 0.5f;
    m_ssgiThickness = 0.1f;
    
    // NEW: LPV GI settings - match RenderContext defaults
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
    
    // NEW: LPV settings
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
    
    //std::cout << "[RenderingSettings] Synced from renderer - Exposure: " << m_exposure 
    //          << ", Gamma: " << m_gamma << ", TAA: " << (m_enableTAA ? "ON" : "OFF")
    //          << ", Bloom: " << (m_enableBloom ? "ON" : "OFF") 
    //          << ", SSAO: " << (m_enableSSAO ? "ON" : "OFF")
    //          << ", LPV: " << (m_enableLPV ? "ON" : "OFF") << std::endl;
}

void RenderingSettingsWindow::SyncToRenderer() {
    if (!m_modularRenderer) return;
    
    auto& ctx = m_modularRenderer->GetContext();
    
    // Post-processing settings
    ctx.exposure = m_exposure;
    ctx.gamma = m_gamma;
    ctx.enableHDR = m_enableHDR;
    ctx.envColor = m_envColor;
    
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
    
    // NEW: LPV settings
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
    
    std::cout << "[RenderingSettings] Synced to renderer - Exposure: " << m_exposure 
              << ", Gamma: " << m_gamma << ", TAA: " << (m_enableTAA ? "ON" : "OFF")
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
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "✗ Not connected to renderer");
    }
    ImGui::Separator();
    
    if (ImGui::BeginTabBar("RenderingTabs")) {
        
        // Post-Processing Tab
        if (ImGui::BeginTabItem("Post-Process")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Tone Mapping & Exposure:");
            
            if (ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 5.0f, "%.2f")) {
                SyncToRenderer(); 
            }
            
            if (ImGui::SliderFloat("Gamma", &m_gamma, 1.0f, 3.0f, "%.2f")) {
                SyncToRenderer(); 
            }
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Environment:");
            
            if (ImGui::ColorEdit3("Sky Color", &m_envColor.x)) {
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
                
                const char* shadowSizes[] = {"512", "1024", "2048", "4096"};
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
        
        // NEW: Global Illumination Tab
        if (ImGui::BeginTabItem("Global Illumination")) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "Screen Space Global Illumination (SSGI):");
            if (ImGui::Checkbox("Enable SSGI", &m_enableSSGI)) { SyncToRenderer(); }
            if (m_enableSSGI) {
                if (ImGui::SliderFloat("SSGI Strength", &m_ssgiStrength, 0.0f, 3.0f, "%.2f")) { SyncToRenderer(); }
                if (ImGui::SliderFloat("SSGI Radius (VS)", &m_ssgiRadius, 0.1f, 5.0f, "%.2f")) { SyncToRenderer(); }
                if (ImGui::SliderInt("SSGI Samples", &m_ssgiSampleCount, 16, 512)) { SyncToRenderer(); }
                if (ImGui::Checkbox("Half Resolution", &m_ssgiHalfRes)) { SyncToRenderer(); }
                if (ImGui::SliderFloat("Temporal Alpha", &m_ssgiTemporalAlpha, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
                if (ImGui::SliderFloat("Normal Reject", &m_ssgiNormalReject, 0.0f, 1.0f, "%.2f")) { SyncToRenderer(); }
                if (ImGui::SliderFloat("Depth Sigma", &m_ssgiDepthReject, 0.01f, 2.0f, "%.2f")) { SyncToRenderer(); }
                if (ImGui::SliderFloat("Thickness (VS)", &m_ssgiThickness, 0.01f, 2.0f, "%.2f")) { SyncToRenderer(); }
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
                
                const char* gridSizes[] = {"64^3 (Fast)", "128^3 (Balanced)", "256^3 (Quality)"};
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
                
                const char* rsmSizes[] = {"256x256 (Fast)", "512x512 (Balanced)", "1024x1024 (Quality)"};
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
                
                // NEW: Debug visualization toggle
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
        
        // Debug Tab
        if (ImGui::BeginTabItem("Debug")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Debug Visualization:");
            
            const char* debugModes[] = {"None", "Albedo", "Normal", "Depth", "Shadow Maps", "Motion Vectors"};
            if (ImGui::Combo("Debug Mode", &m_debugMode, debugModes, 6)) {
                // Debug mode change
            }
            
            ImGui::Separator();
            
            if (ImGui::Checkbox("Wireframe Mode", &m_wireframeMode)) {
                // Wireframe toggle
            }
            
            if (ImGui::Checkbox("Show Bounding Boxes", &m_showBoundingBoxes)) {
                // Bounding box visualization
            }
            
            if (ImGui::Checkbox("Show Light Gizmos", &m_showLightGizmos)) {
                // Light gizmo visualization
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
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No renderer connected");
                ImGui::Text("Connect ModularRenderer via");
                ImGui::Text("SetModularRenderer()");
            }
            
            ImGui::EndTabItem();
        }
        
        // Quality Tab
        if (ImGui::BeginTabItem("Quality")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Rendering Quality:");
            
            const char* qualityPresets[] = {"Low", "Medium", "High", "Ultra"};
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
    
    // NEW: LPV - match RenderContext defaults
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