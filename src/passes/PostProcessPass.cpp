#include "PostProcessPass.h"
#include "../PresentTarget.h"
#include "PassLogging.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
    // CIE 1931 xy chromaticity of a white point at temperature T (Kelvin): Planckian locus below
    // 4000 K, CIE daylight locus above (same split as UE4's white balance). Tint shifts along the
    // isotherm in CIE 1960 uv (negative = green, positive = magenta).
    glm::vec2 WhitePointXY(float temperature, float tint) {
        const float T = std::clamp(temperature, 1500.0f, 15000.0f);
        const float T2 = T * T;
        const float u = (0.860117757f + 1.54118254e-4f * T + 1.28641212e-7f * T2) /
            (1.0f + 8.42420235e-4f * T + 7.08145163e-7f * T2);
        const float v = (0.317398726f + 4.22806245e-5f * T + 4.20481691e-8f * T2) /
            (1.0f - 2.89741816e-5f * T + 1.61456053e-7f * T2);
        glm::vec2 xy;
        if (T < 4000.0f) {
            xy = glm::vec2(3.0f * u, 2.0f * v) / (2.0f * u - 8.0f * v + 4.0f);
        }
        else {
            const float x = T <= 7000.0f
                ? 0.244063f + 0.09911e3f / T + 2.9678e6f / T2 - 4.6070e9f / (T2 * T)
                : 0.237040f + 0.24748e3f / T + 1.9018e6f / T2 - 2.0064e9f / (T2 * T);
            xy = glm::vec2(x, -3.0f * x * x + 2.87f * x - 0.275f);
        }
        if (tint != 0.0f) {
            // Approximate isotherm shift: move along CIE 1960 v (positive tint = less green).
            const float denom = -2.0f * xy.x + 12.0f * xy.y + 3.0f;
            glm::vec2 uv(4.0f * xy.x / denom, 6.0f * xy.y / denom);
            uv.y -= tint * 0.033f;
            xy = glm::vec2(3.0f * uv.x, 2.0f * uv.y) / (2.0f * uv.x - 8.0f * uv.y + 4.0f);
        }
        return xy;
    }

    glm::vec3 XYZFromXY(const glm::vec2& xy) {
        return glm::vec3(xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y);
    }

    // Linear Rec.709 matrix that maps a scene lit by `temperature` to neutral (Bradford, D65 target).
    glm::mat3 WhiteBalanceMatrix(float temperature, float tint) {
        if (std::abs(temperature - 6500.0f) < 1.0f && std::abs(tint) < 1e-4f) {
            return glm::mat3(1.0f);
        }
        // glm matrices are column-major: each glm::vec3 below is a column.
        const glm::mat3 rgbToXYZ(
            glm::vec3(0.4124564f, 0.2126729f, 0.0193339f),
            glm::vec3(0.3575761f, 0.7151522f, 0.1191920f),
            glm::vec3(0.1804375f, 0.0721750f, 0.9503041f));
        const glm::mat3 xyzToRGB = glm::inverse(rgbToXYZ);
        const glm::mat3 bradford(
            glm::vec3(0.8951f, -0.7502f, 0.0389f),
            glm::vec3(0.2664f, 1.7135f, -0.0685f),
            glm::vec3(-0.1614f, 0.0367f, 1.0296f));
        const glm::mat3 bradfordInv = glm::inverse(bradford);
        const glm::vec3 srcLMS = bradford * XYZFromXY(WhitePointXY(temperature, tint));
        const glm::vec3 dstLMS = bradford * XYZFromXY(glm::vec2(0.31271f, 0.32902f));
        glm::mat3 scale(1.0f);
        scale[0][0] = dstLMS.x / srcLMS.x;
        scale[1][1] = dstLMS.y / srcLMS.y;
        scale[2][2] = dstLMS.z / srcLMS.z;
        return xyzToRGB * bradfordInv * scale * bradford * rgbToXYZ;
    }
}

PostProcessPass::PostProcessPass() {}

PostProcessPass::~PostProcessPass() {
    if (m_shader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_shader);
}

bool PostProcessPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                   "shaders/postprocess_frag.glsl");
    if (!m_shader) {
        std::cerr << "[PostProcessPass] Failed to create shader.\n";
        return false;
    }

    std::cout << "[PostProcessPass] Initialized successfully.\n";
    return true;
}

void PostProcessPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // Viewport handled in Execute
}

void PostProcessPass::Execute(RenderContext& ctx,
                              const std::shared_ptr<SceneGraph>& sceneGraph,
                              const std::shared_ptr<Camera>& camera,
                              const std::shared_ptr<DirectionalLight>& dirLight,
                              const std::shared_ptr<Skybox>& skybox) {
    // Output to backbuffer
    rhi::Device& gpu = rhi::Gpu();
    gpu.BindFramebuffer(PresentTarget::Get());
    gpu.SetViewport({ 0, 0, ctx.width, ctx.height });

    // We handle output gamma in the shader, disable fixed-function SRGB conversion
    gpu.SetFeature(rhi::Feature::FramebufferSRGB, false);
    
    // Disable depth/blend for fullscreen quad
    gpu.SetDepthTest(false);
    gpu.SetBlendEnabled(false);
    
    // Clear with environment color
    gpu.SetClearColor(glm::vec4(ctx.envColor.r, ctx.envColor.g, ctx.envColor.b, 1.0f));
    gpu.Clear(rhi::ClearColor | rhi::ClearDepth);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Using shader: " << m_shader);
    gpu.UseProgram(m_shader);

    // Helper to set uniform only if present
    auto set1f = [&](const char* name, float v){ rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, name); if (loc >= 0) gpu.SetUniform(loc, v); };
    auto set1i = [&](const char* name, int v){ rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, name); if (loc >= 0) gpu.SetUniform(loc, v); };

    set1f("exposure", ctx.exposure);
    set1f("gamma", ctx.gamma);

    // Tonemapper uniforms
    set1i("uTonemap", static_cast<int>(ctx.tonemapType));
    set1f("uP", ctx.tm_P);
    set1f("ua", ctx.tm_a);
    set1f("um", ctx.tm_m);
    set1f("ul", ctx.tm_l);
    set1f("uc", ctx.tm_c);
    set1f("ub", ctx.tm_b);
    set1i("uOutputSRGB", ctx.outputSRGB ? 1 : 0);

    // GT7 params
    set1f("uTm7PeakNits", ctx.tm7_peakNits);
    set1f("uTm7Blend", ctx.tm7_blend);
    set1f("uTm7FadeStart", ctx.tm7_fadeStart);
    set1f("uTm7FadeEnd", ctx.tm7_fadeEnd);
    set1i("uTm7UseJzazbz", ctx.tm7_useJzazbz ? 1 : 0);

    // Image formation (docs/nfs2015 M3)
    auto set3f = [&](const char* name, const glm::vec3& v){ rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, name); if (loc >= 0) gpu.SetUniform(loc, v); };
    set1i("uBloomMode", ctx.bloomMode);
    set1f("uBloomStrength", ctx.bloomStrength);
    set1f("uBloomScatterThreshold", ctx.bloomMode == 1 ? ctx.bloomScatterThreshold : 0.0f);
    set1i("uHasBloom", (ctx.enableBloom && m_bloomTexture.IsValid()) ? 1 : 0);
    const glm::mat3 whiteBalance = WhiteBalanceMatrix(ctx.whiteBalanceTemperature, ctx.whiteBalanceTint);
    if (rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "uWhiteBalance"); loc >= 0) {
        gpu.SetUniform(loc, whiteBalance);
    }
    set1f("uContrast", ctx.gradeContrast);
    set1f("uSaturation", ctx.gradeSaturation);
    set3f("uShadowTint", ctx.gradeShadowTint);
    set3f("uHighlightTint", ctx.gradeHighlightTint);
    set1f("uSplitToneStrength", ctx.gradeSplitToneStrength);
    set3f("uLift", ctx.gradeLift);
    set3f("uGammaAdjust", ctx.gradeGamma);
    set3f("uGain", ctx.gradeGain);
    set1f("uVignette", ctx.vignetteIntensity);
    set1f("uFilmGrain", ctx.filmGrainIntensity);
    set1f("uChromaticAberration", ctx.chromaticAberration);
    set1i("uDither", ctx.enableOutputDither ? 1 : 0);
    if (rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "uFrameIndex"); loc >= 0) gpu.SetUniform(loc, m_frameIndex++);
    if (rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "uResolution"); loc >= 0) gpu.SetUniform(loc, glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));

    FrameBuffer* sourceFBO = ctx.postSourceFBO ? ctx.postSourceFBO
    	: ((ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : ctx.hdrFBO.get());
    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Source FBO: " << (sourceFBO ? sourceFBO->GetFBO() : rhi::FramebufferHandle{}));
    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Source texture: " << (sourceFBO ? sourceFBO->GetColorAttachment(0) : rhi::TextureHandle{}));
    
    // Bind HDR scene (or TAA-resolved output if TAA is enabled)
    if (sourceFBO) {
        rhi::TextureHandle hdrTex = sourceFBO->GetColorAttachment(0);
        if (gpu.IsTexture(hdrTex)) {
            gpu.BindTexture(0, hdrTex);
            PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bound source texture successfully");
        } else {
            std::cerr << "[PostProcessPass] ERROR: Invalid source texture!" << std::endl;
        }
    } else {
        std::cerr << "[PostProcessPass] ERROR: Source FBO is null!" << std::endl;
    }
    set1i("hdrBuffer", 0);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bloom texture: " << m_bloomTexture);
    // Bind bloom result
    if (m_bloomTexture.IsValid() && gpu.IsTexture(m_bloomTexture)) {
        gpu.BindTexture(1, m_bloomTexture);
        PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bound bloom texture successfully");
    } else {
        PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] No valid bloom texture, using black");
        // Optionally bind 0
        gpu.BindTexture(1, rhi::TextureHandle{});
    }
    set1i("bloomBlur", 1);

    // Inspection of intermediate images (RenderContext::postDebugView).
    int debugView = ctx.postDebugView;
    rhi::TextureHandle debugTex;
    rhi::TextureHandle debugTex2;
    FrameBuffer* taaOut = (ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : nullptr;
    switch (debugView) {
    case 1: debugTex = ctx.hdrFBO ? ctx.hdrFBO->GetColorAttachment(0) : rhi::TextureHandle{}; break;
    case 2: case 7: debugTex = taaOut ? taaOut->GetColorAttachment(0) : rhi::TextureHandle{}; break;
    case 3: debugTex = ctx.ssrTexture; break;
    case 4: debugTex = m_bloomTexture; break;
    case 5: debugTex = ctx.velocityTex; break;
    case 6:
        debugTex = sourceFBO ? sourceFBO->GetColorAttachment(0) : rhi::TextureHandle{};
        debugTex2 = taaOut ? taaOut->GetColorAttachment(0) : rhi::TextureHandle{};
        break;
    case 8: debugTex = ctx.ssaoTexture; break;
    default: break;
    }
    if (debugView != 0 && !debugTex.IsValid()) debugView = -1; // requested image not produced: black
    set1i("uDebugView", debugView);
    gpu.BindTexture(2, debugTex);
    set1i("uDebugTex", 2);
    gpu.BindTexture(3, debugTex2.IsValid() ? debugTex2 : debugTex);
    set1i("uDebugTex2", 3);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Rendering fullscreen quad...");
    // Render fullscreen quad
    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    } else {
        std::cerr << "[PostProcessPass] ERROR: ScreenQuad is null!" << std::endl;
    }
}
