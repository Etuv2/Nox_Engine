#pragma once

#include "../RenderPass.h"
#include "../ComputeShader.h"
#include "../TransformGpuContract.h"
#include <memory>

class TransformHistoryPass : public RenderPass {
public:
    TransformHistoryPass();
    ~TransformHistoryPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

private:
    void EnsureBuffers(size_t transformRecordCount);
    void ResetVisibleListHeader();

    std::unique_ptr<ComputeShader> m_shader;
    GLuint m_historySSBO = 0;
    GLuint m_visibleListSSBO = 0;
    size_t m_historyCapacity = 0;
    size_t m_visibleCapacity = 0;
    uint32_t m_frameIndex = 0;
};
