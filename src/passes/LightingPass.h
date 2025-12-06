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

    // Enable/disable verbose logging (disabled by default for performance)
    static constexpr bool VerboseLogging = false;

private:
    void SetupFallbackIBL();
    void CacheUniformLocations();
    
    GLuint m_shader = 0;
    TexturePtr m_fallbackCubemap;
    TexturePtr m_fallbackBRDF;
    GLuint m_ssaoTexture = 0;
    GLuint m_sssTexture = 0;  // Screen-space shadow texture
    GLuint m_ssgiTexture = 0; //Screen-space GI texture
    
    GLuint m_lpvTextureR = 0;
    GLuint m_lpvTextureG = 0;
    GLuint m_lpvTextureB = 0;

    // Cached uniform locations to avoid per-frame glGetUniformLocation calls
    struct UniformLocations {
        // Sampler uniforms
        GLint gPackedNormalRM = -1;
        GLint gAlbedoAO = -1;
        GLint gEmissiveSpec = -1;
        GLint gDepth = -1;
        GLint ssaoMap = -1;
        GLint screenSpaceShadowMap = -1;
        GLint ssgiMap = -1;
        GLint lpvTextureR = -1;
        GLint lpvTextureG = -1;
        GLint lpvTextureB = -1;
        GLint irradianceMap = -1;
        GLint prefilteredMap = -1;
        GLint brdfLUT = -1;
        GLint multiLightShadowArray = -1;
        
        // Matrix uniforms
        GLint invProjection = -1;
        GLint invView = -1;
        GLint view = -1;
        GLint viewPos = -1;
        
        // IBL uniforms
        GLint prefilteredMaxLOD = -1;
        GLint iblIntensity = -1;
        GLint diffuseIBLScale = -1;
        GLint specularIBLScale = -1;
        
        // Effect strength uniforms
        GLint aoStrength = -1;
        GLint sssStrength = -1;
        GLint ssgiStrength = -1;
        
        // LPV uniforms
        GLint enableLPV = -1;
        GLint lpvGridCenter = -1;
        GLint lpvGridResolution = -1;
        GLint lpvVoxelSize = -1;
        GLint lpvGIStrength = -1;
        GLint lpvGridOrientation = -1;
        GLint lpvDebugVisualization = -1;
        GLint lpvDebugBoost = -1;
        
        // Light uniforms
        GLint numLights = -1;
        GLint numDirectionalLights = -1;
        GLint numPointLights = -1;
        GLint numSpotLights = -1;
        GLint enableShadows = -1;
        GLint shadowBias = -1;
        GLint maxShadowBias = -1;
        GLint normalOffsetScale = -1;
        GLint cascadeBiasScale = -1;
        GLint cascadeCount = -1;
    } m_uniforms;
    
    bool m_uniformsCached = false;
};
