#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include "../ComponentTypes.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

class Scene;

/**
 * @brief TransparentForwardPass renders transparent objects with physically correct glass-like materials.
 * 
 */
class TransparentForwardPass : public RenderPass {
public:
    TransparentForwardPass();
    ~TransparentForwardPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    struct UniformLocations {
        rhi::UniformLocation view = -1;
        rhi::UniformLocation projection = -1;
        rhi::UniformLocation model = -1;
        rhi::UniformLocation normalMatrix = -1;
        rhi::UniformLocation viewPos = -1;
        rhi::UniformLocation screenSize = -1;
        rhi::UniformLocation gDepth = -1;
        rhi::UniformLocation irradianceMap = -1;
        rhi::UniformLocation prefilteredMap = -1;
        rhi::UniformLocation brdfLUT = -1;
        rhi::UniformLocation prefilteredMaxLOD = -1;
        rhi::UniformLocation iblIntensity = -1;
        rhi::UniformLocation diffuseIBLScale = -1;
        rhi::UniformLocation specularIBLScale = -1;
        rhi::UniformLocation numLights = -1;
        rhi::UniformLocation multiLightShadowArray = -1;
        rhi::UniformLocation enableShadows = -1;
        rhi::UniformLocation shadowBias = -1;
        rhi::UniformLocation cascadeSplits = -1;
        rhi::UniformLocation cascadeBlendDistance = -1;
        rhi::UniformLocation cascadeBlendFactor = -1;
        rhi::UniformLocation shadowDarkness = -1;
        rhi::UniformLocation shadowMinBrightness = -1;
        rhi::UniformLocation shadowTransitionHardness = -1;
    };

    struct TransparentCandidate {
        EntityID entity = INVALID_ENTITY;
        std::shared_ptr<Scene> model;
        glm::mat4 worldTransform = glm::mat4(1.0f);
        float distanceToCamera = 0.0f;
    };

    rhi::ProgramHandle m_shader; // Forward PBR shader for transparent materials
    UniformLocations m_uniforms;
    std::vector<TransparentCandidate> m_transparentCandidates;

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;
};
