#pragma once

#include "../RenderPass.h"
#include "../Texture.h"  // Include new Texture system
#include <memory>

class ComputeShader;
class RenderContext;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;

/**
 * @brief Screen Space Global Illumination Pass using compute shaders
 * Refactored to use the new Texture class for improved resource management
 */
class SSGIPass : public RenderPass {
public:
    SSGIPass() = default;
    ~SSGIPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
   const std::shared_ptr<SceneGraph>& sceneGraph,
       const std::shared_ptr<Camera>& camera,
         const std::shared_ptr<DirectionalLight>& dirLight,
     const std::shared_ptr<Skybox>& skybox) override;

 // Capture current HDR frame for ray marching history
    void CaptureHistory(RenderContext& ctx);

    // Get final SSGI texture
    GLuint GetSSGITexture() const { return m_ssgiTex ? m_ssgiTex->ID() : 0; }

private:
    // Compute shaders for SSGI pipeline
    std::unique_ptr<ComputeShader> m_csDirections;   // Stochastic direction generation
  std::unique_ptr<ComputeShader> m_csRaymarch;   // Screen-space ray marching
    std::unique_ptr<ComputeShader> m_csDownsample;   // Downsample for blur
    std::unique_ptr<ComputeShader> m_csBilateral;    // Edge-aware bilateral blur
    std::unique_ptr<ComputeShader> m_csUpsample;     // Bilateral upsample
    std::unique_ptr<ComputeShader> m_csTemporal;     // Temporal accumulation

    // REFACTORED: Use TexturePtr instead of raw GLuint
    TexturePtr m_dirTex;            // Stochastic directions (RG16F)
    TexturePtr m_ssgiRaw;        // Raw ray march output (RGBA16F)
    TexturePtr m_ssgiQuarter;  // Quarter-res downsampled (RGBA16F)
    TexturePtr m_ssgiQuarterBlur;   // Quarter-res blurred (RGBA16F)
    TexturePtr m_ssgiBlur;          // Denoised/upsampled (RGBA16F)
    TexturePtr m_ssgiTex;// Final temporal result (RGBA16F)
    
 // Temporal history textures
    TexturePtr m_historyColor;      // Previous frame HDR color
    TexturePtr m_historySSGI;     // Previous frame SSGI

    // Kawase blur resources
    GLuint m_kawaseShader = 0;
    GLuint m_kawaseFBO = 0;
    TexturePtr m_kawasePing;    // Kawase ping buffer
    TexturePtr m_kawasePong;   // Kawase pong buffer
    int m_kawasePasses = 3;         // Number of Kawase blur iterations

    // Resolution tracking
    int m_w = 0, m_h = 0;        // Full resolution
    int m_hw = 0, m_hh = 0;    // Half/working resolution
    int m_qw = 0, m_qh = 0;      // Quarter resolution
    bool m_halfRes = true; // Use half resolution for performance
    int m_frameIndex = 0;           // Frame counter for stochastic sampling

    // Pipeline stages
    void runDirections(RenderContext& ctx);
    void runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera);
    void runDownsample(RenderContext& ctx);
  void runBilateral(RenderContext& ctx);
    void runUpsample(RenderContext& ctx);
    void runTemporal(RenderContext& ctx);
    void runKawase(RenderContext& ctx);
};
