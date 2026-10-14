#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <memory>

class ComputeShader;

// Ambient occlusion with the HBAO+ structure (Bavoil & Sainz 2008; NVIDIA HBAO+):
//   1. deinterleave: linearize the gbuffer depth into 4x4 quarter-res layers (Tex2DArray)
//   2. HBAO: horizon search per layer, ssaoDirections x ssaoSteps (default 8 x 4), gbuffer normals, fixed 4x4 jitter table
//   3. reinterleave: back to the output resolution, view depth packed next to the AO
//   4. depth-aware separable blur (X then Y)
// Output: GetSSAOTexture() (R-channel AO, 1 = unoccluded), consumed unchanged by the lighting pass.
// Settings: RenderContext ssao* fields (see docs/hbao.md).
class HBAOPass : public RenderPass {
public:
    HBAOPass();
    ~HBAOPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    rhi::TextureHandle GetSSAOTexture() const { return m_outputTex; }

private:
    void CreateTextures(int width, int height, float resolutionScale);
    void DestroyTextures();

    std::unique_ptr<ComputeShader> m_deinterleaveShader;
    std::unique_ptr<ComputeShader> m_hbaoShader;
    std::unique_ptr<ComputeShader> m_reinterleaveShader;
    std::unique_ptr<ComputeShader> m_blurShader;

    rhi::TextureHandle m_linearDepthArray;   // R32F Tex2DArray, 16 quarter-res layers
    rhi::TextureHandle m_aoArray;            // R16F Tex2DArray, 16 quarter-res layers
    rhi::TextureHandle m_packedTex;          // RG16F (ao, view depth) at output resolution
    rhi::TextureHandle m_blurTex;            // RG16F intermediate of the separable blur
    rhi::TextureHandle m_outputTex;          // RG16F final AO (red channel used)

    int m_fullWidth = 0;
    int m_fullHeight = 0;
    int m_outWidth = 0;
    int m_outHeight = 0;
    int m_quarterWidth = 0;
    int m_quarterHeight = 0;
    float m_resolutionScale = 1.0f;
};
