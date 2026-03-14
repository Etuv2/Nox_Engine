#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

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
        GLint view = -1;
        GLint projection = -1;
        GLint model = -1;
        GLint normalMatrix = -1;
        GLint viewPos = -1;
        GLint screenSize = -1;
        GLint gDepth = -1;
        GLint irradianceMap = -1;
        GLint prefilteredMap = -1;
        GLint brdfLUT = -1;
        GLint prefilteredMaxLOD = -1;
        GLint numLights = -1;
        GLint multiLightShadowArray = -1;
    };

    GLuint m_shader = 0; // Forward PBR shader for transparent materials
    UniformLocations m_uniforms;

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;
};
