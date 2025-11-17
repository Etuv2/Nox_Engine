#pragma once
#include "../RenderPass.h"
#include "../Texture.h"
#include <GL/glew.h>
#include <memory>

/**
 * LightingPass performs deferred lighting using G-buffer data
 */
class LightingPass : public RenderPass {
public:
    LightingPass();
    ~LightingPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    //Allow SSAOPass to provide its output texture
    void SetSSAOTexture(GLuint ssaoTex) { m_ssaoTexture = ssaoTex; }
    
    //Allow ScreenSpaceShadowPass to provide its output texture
    void SetScreenSpaceShadowTexture(GLuint sssTex) { m_sssTexture = sssTex; }

    //Allow SSGIPass to provide its SSGI texture
    void SetSSGITexture(GLuint ssgiTex) { m_ssgiTexture = ssgiTex; }

    //Allow LPVPass to provide its LPV 3D textures for global illumination
    void SetLPVTextures(GLuint lpvR, GLuint lpvG, GLuint lpvB) {
        m_lpvTextureR = lpvR;
        m_lpvTextureG = lpvG;
        m_lpvTextureB = lpvB;
    }

private:
    void SetupFallbackIBL();
    
    GLuint m_shader = 0;
    TexturePtr m_fallbackCubemap;
    TexturePtr m_fallbackBRDF;
    GLuint m_ssaoTexture = 0;
    GLuint m_sssTexture = 0;  // Screen-space shadow texture
    GLuint m_ssgiTexture = 0; //Screen-space GI texture
    
    GLuint m_lpvTextureR = 0;
    GLuint m_lpvTextureG = 0;
    GLuint m_lpvTextureB = 0;
};
