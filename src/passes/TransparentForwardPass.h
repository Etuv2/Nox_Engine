#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include "../ComponentTypes.h"
#include <glm/glm.hpp>
#include <vector>

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
    struct TransparentCandidate {
        EntityID entity = 0;
        std::shared_ptr<Scene> model;
        glm::mat4 worldTransform{ 1.0f };
        float distanceToCamera = 0.0f;
    };

    GLuint m_shader = 0; // Forward PBR shader for transparent materials
    std::vector<TransparentCandidate> m_transparentCandidates;
};
