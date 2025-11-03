#include "ScreenSpaceShadowPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../ShaderLoader.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

namespace {
    // std140 layout matching the GLSL block
    struct SSSParamsStd140 {
        glm::vec2 invScreen;
        glm::vec2 screenSize;

        glm::vec3 lightDirVS;
        float     maxRayLength;
        int       numSteps;
        float     thickness;
        float     edgeThreshold;
        float     jitterStrength;
        int       enableJitter;
        int       debugMode;
        int       _pad0, _pad1;

        glm::mat4 invProj;
        glm::mat4 proj;

        uint32_t  frameIndex;
        uint32_t  _pad2, _pad3, _pad4;

        // NEW:
        int       enableBilateralBlur; // 0/1
        int       blurRadius;          // 1..3 (shader clamps to 1)
        float     depthSensitivity;    // ~0.05..0.2
        float     _pad5;               // align to vec4
    };

}

ScreenSpaceShadowPass::ScreenSpaceShadowPass() {}

ScreenSpaceShadowPass::~ScreenSpaceShadowPass() {
    // Texture now managed by smart pointer (automatic cleanup)
  m_shadowTex.reset();
    
    if (m_paramsUBO) {
        glDeleteBuffers(1, &m_paramsUBO);
        m_paramsUBO = 0;
    }
}

bool ScreenSpaceShadowPass::Initialize(RenderContext& ctx)
{
    m_cs = std::make_unique<ComputeShader>();
    if (!m_cs->CreateFromFile("shaders/screen_space_shadows_comp.glsl")) {
        std::cerr << "[SSS] Failed to compile compute shader.\n";
        return false;
    }

    createOutput(ctx.width, ctx.height);
    createUBO();
    return true;
}

void ScreenSpaceShadowPass::Resize(RenderContext& /*ctx*/, int w, int h)
{
    if (w == m_width && h == m_height) return;
    createOutput(w, h);
}

void ScreenSpaceShadowPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>&,
    const std::shared_ptr<DirectionalLight>& dirLight,
    const std::shared_ptr<Skybox>&)
{
  if (!m_cs || !m_cs->IsValid() || !ctx.gbufferFBO || !dirLight || !m_shadowTex) return;

    // Light direction convention
    // GetDirection() returns the direction light POINTS (from source to surface)
    // But screen-space shadows need to march FROM surface TOWARD light source
    // So we NEGATE to get the direction TO the light
    glm::vec3 Lw = glm::normalize(dirLight->GetDirection());
    glm::vec3 LightToSurfaceVS = glm::normalize(glm::mat3(ctx.view) * Lw);
    glm::vec3 SurfaceToLightVS = -LightToSurfaceVS;  // NEGATE for correct ray direction

    // Update UBO
    updateUBO(SurfaceToLightVS, ctx.proj);

    // Bind & dispatch
    glUseProgram(m_cs->GetProgramID());

 // Depth (binding=0)
  GLuint depthTex = ctx.gbufferFBO->GetDepthTexture();
    glBindTextureUnit(0, depthTex);
    glBindTexture(GL_TEXTURE_2D, depthTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Output (binding=1) - use new Texture class
    glBindImageTexture(1, m_shadowTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);

    // UBO (binding=2)
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_paramsUBO);

    // Dispatch
    GLuint gx = (m_width + 7) / 8;
    GLuint gy = (m_height + 7) / 8;
    glDispatchCompute(gx, gy, 1);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // Unbind
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, 0);
glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindTextureUnit(0, 0);
glUseProgram(0);

 // Advance frame index
    ++m_frameIndex;
}

void ScreenSpaceShadowPass::createOutput(int w, int h)
{
  m_width = w; 
    m_height = h;

    // Create R8 texture using new Texture builder
    std::cout << "[ScreenSpaceShadowPass] Creating shadow output texture (" << w << "x" << h << ")" << std::endl;
    
    m_shadowTex = Texture::Builder::Texture2D(w, h, GL_R8)
        .Format(GL_RED)
.DataType(GL_UNSIGNED_BYTE)
        .FilterMode(GL_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
 .TextureType(TextureType::Custom)
        .Build();

    if (!m_shadowTex || !m_shadowTex->IsValid()) {
   std::cerr << "[ScreenSpaceShadowPass] Failed to create shadow texture!" << std::endl;
     return;
    }

    // Initialize to fully lit (255)
    std::vector<uint8_t> ones(w * h, 255);
    m_shadowTex->Upload2D(0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, ones.data());

    std::cout << "[ScreenSpaceShadowPass] Shadow texture created: ID=" << m_shadowTex->ID() << std::endl;

    if (!m_paramsUBO) createUBO();
}

void ScreenSpaceShadowPass::createUBO()
{
    if (m_paramsUBO) glDeleteBuffers(1, &m_paramsUBO);
    glGenBuffers(1, &m_paramsUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SSSParamsStd140), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void ScreenSpaceShadowPass::updateUBO(const glm::vec3& lightDirVS,
    const glm::mat4& projection)
{
    SSSParamsStd140 p{};
    p.invScreen = glm::vec2(1.0f / float(m_width), 1.0f / float(m_height));
    p.screenSize = glm::vec2(float(m_width), float(m_height));
    p.lightDirVS = lightDirVS;
    p.maxRayLength = m_cfg.rayLength;
    p.numSteps = std::max(1, m_cfg.steps);
    p.thickness = m_cfg.thickness;
    p.edgeThreshold = m_cfg.edgeFade;
    p.jitterStrength = glm::clamp(m_cfg.jitterAmount, 0.0f, 1.0f);
    p.enableJitter = m_cfg.jitter ? 1 : 0;
    p.debugMode = m_cfg.debugMode;  // NEW: Pass debug mode to shader
    p.invProj = glm::inverse(projection);
    p.proj = projection;
    p.frameIndex = m_frameIndex;
    p.enableBilateralBlur = m_cfg.enableBilateralBlur ? 1 : 0;
    p.blurRadius = glm::clamp(m_cfg.blurRadius, 1, 3);
    p.depthSensitivity = m_cfg.depthSensitivity;


    // DEBUG: Log once per second (60 fps assumption)
    static int logCounter = 0;
    if (logCounter++ % 60 == 0) {
        std::cout << "[SSS] Light Dir (view space): (" 
                  << lightDirVS.x << ", " << lightDirVS.y << ", " << lightDirVS.z << ")\n";
        std::cout << "[SSS] Ray Length: " << p.maxRayLength 
                  << " | Steps: " << p.numSteps 
                  << " | Thickness: " << p.thickness 
                  << " | Debug Mode: " << p.debugMode << "\n";
    }

    glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(SSSParamsStd140), &p);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}
