#pragma once
#include "../RenderPass.h"
#include <memory>
#include <GL/glew.h>

class SceneGraph; 
class Camera; 
class DirectionalLight; 
class Skybox;
class ComputeShader;

/**
 * @brief Screen Space Global Illumination Pass
 * 
 * Implements dynamic, real-time diffuse global illumination using screen-space ray tracing.
 * Based on the approach by Shubham Sachdeva - provides noise-free GI through:
 * - Stochastic cosine-weighted hemisphere sampling per pixel
 * - Screen-space ray marching to find indirect lighting contributions
 * - Bilateral filtering for edge-aware denoising
 * - Temporal accumulation for stability
 * 
 * The pass runs at configurable resolution (default half-res) and uses compute shaders
 * for efficient parallel processing. Results are composited in the lighting pass.
 */
class SSGIPass : public RenderPass {
public:
    SSGIPass() = default;
    ~SSGIPass() override;

    bool Initialize(RenderContext& ctx) override;
    void Resize(RenderContext& ctx, int w, int h) override;

    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    /**
     * @brief Capture current frame color as history for next frame's ray marching
     * Call once per frame AFTER PostProcess to capture final color
     */
    void CaptureHistory(RenderContext& ctx);

    /**
     * @brief Get the final SSGI texture for compositing in lighting pass
     */
    GLuint GetSSGITexture() const { return m_ssgiTex; }

private:
    void allocTargets(int w, int h, bool halfRes);
    void freeTargets();

    // Compute shader stages
    void runDirections(RenderContext& ctx);
    void runRaymarch(RenderContext& ctx);
    void runDownsample(RenderContext& ctx);   // half -> quarter
    void runBilateral(RenderContext& ctx);    // Edge-aware denoising at quarter
    void runUpsample(RenderContext& ctx);     // quarter -> half with bilateral upsample
    void runTemporal(RenderContext& ctx);

    // Compute shaders for each stage
    std::unique_ptr<ComputeShader> m_csDirections;  // Generate stochastic directions
    std::unique_ptr<ComputeShader> m_csRaymarch;    // Screen-space ray marching
    std::unique_ptr<ComputeShader> m_csDownsample;  // 2x downsample
    std::unique_ptr<ComputeShader> m_csBilateral;   // Edge-aware denoising (quarter)
    std::unique_ptr<ComputeShader> m_csUpsample;    // 2x upsample + bilateral
    std::unique_ptr<ComputeShader> m_csTemporal;    // Temporal accumulation

    // Working textures
    GLuint m_dirTex = 0;        // RG16F stochastic directions (view space)
    GLuint m_ssgiRaw = 0;       // RGBA16F raw raymarch output (half/full)
    GLuint m_ssgiQuarter = 0;   // RGBA16F quarter-res downsample
    GLuint m_ssgiQuarterBlur = 0; // RGBA16F quarter-res blurred
    GLuint m_ssgiBlur = 0;      // RGBA16F upsampled + blurred (half/full)
    GLuint m_ssgiTex = 0;       // RGBA16F final resolved (what lighting pass samples)

    // History textures for temporal stability
    GLuint m_historyColor = 0;  // RGBA16F previous frame scene color
    GLuint m_historySSGI = 0;   // RGBA16F previous frame SSGI

    // Resolution tracking
    int m_w = 0, m_h = 0;       // full-res dimensions
    int m_hw = 0, m_hh = 0;     // half-res working dimensions
    int m_qw = 0, m_qh = 0;     // quarter-res dimensions
    bool m_halfRes = true;      // current half-res state

    // Temporal
    int m_frameIndex = 0;       // for stochastic directions animation
};
