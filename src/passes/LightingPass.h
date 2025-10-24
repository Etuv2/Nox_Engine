#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

/**
 * LightingPass performs deferred lighting:
 * - Binds all G-buffer attachments
 * - Copies depth from G-buffer to HDR FBO
 * - Binds SSAO result
 * - Binds Screen Space Shadow result
 * - Uses skybox IBL if valid, else fallback white-cube/neutral BRDF
 * - Uploads inverse matrices
 * - Binds LightManager SSBOs and shadow array
 * - Sets shadow bias and light counts
 * - Binds LPV textures for global illumination
 * - Draws fullscreen quad to HDR FBO
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

    // CRITICAL: Allow SSAOPass to provide its output texture
    void SetSSAOTexture(GLuint ssaoTex) { m_ssaoTexture = ssaoTex; }
    
    // CRITICAL: Allow ScreenSpaceShadowPass to provide its output texture
    void SetScreenSpaceShadowTexture(GLuint sssTex) { m_sssTexture = sssTex; }

    // NEW: Allow SSGIPass to provide its SSGI texture
    void SetSSGITexture(GLuint ssgiTex) { m_ssgiTexture = ssgiTex; }

    // NEW: Allow LPVPass to provide its LPV 3D textures for global illumination
    void SetLPVTextures(GLuint lpvR, GLuint lpvG, GLuint lpvB) {
        m_lpvTextureR = lpvR;
        m_lpvTextureG = lpvG;
        m_lpvTextureB = lpvB;
    }

private:
    void SetupFallbackIBL();
    
    GLuint m_shader = 0;
    GLuint m_fallbackCubemap = 0;
    GLuint m_fallbackBRDF = 0;
    GLuint m_ssaoTexture = 0;
    GLuint m_sssTexture = 0;  // Screen-space shadow texture
    GLuint m_ssgiTexture = 0; // NEW: Screen-space GI texture
    
    // NEW: LPV 3D textures for global illumination
    GLuint m_lpvTextureR = 0;
    GLuint m_lpvTextureG = 0;
    GLuint m_lpvTextureB = 0;
};
