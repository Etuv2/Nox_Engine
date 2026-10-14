#include "SelectionOutlinePass.h"
#include "../PresentTarget.h"
#include "../SceneGraph.h"
#include "../RenderSystem.h"
#include "../RenderContext.h"
#include "../ScreenQuad.h"
#include "../ShaderLoader.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <iostream>
#include <glm/glm.hpp>

namespace {
// Hovered parts: a thinner, lighter line than the selection.
constexpr glm::vec3 kHoverColor{ 1.0f, 0.82f, 0.55f };
constexpr float kHoverWidth = 1.0f;
}

SelectionOutlinePass::SelectionOutlinePass() {}

SelectionOutlinePass::~SelectionOutlinePass() {
    if (m_maskShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_maskShader);
    if (m_outlineShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_outlineShader);
}

bool SelectionOutlinePass::Initialize(RenderContext& context) {
    m_maskShader = CreateShaderProgram("shaders/selection_mask_vert.glsl", "shaders/selection_mask_frag.glsl");
    m_outlineShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/selection_outline_frag.glsl");
    if (!m_maskShader || !m_outlineShader) {
        std::cerr << "[SelectionOutlinePass] Failed to create shader programs\n";
        return false;
    }

    rhi::Device& gpu = rhi::Gpu();
    m_locMaskSceneDepth = gpu.GetUniformLocation(m_maskShader, "uSceneDepth");
    m_locMaskProjAB = gpu.GetUniformLocation(m_maskShader, "uProjAB");
    m_locMaskRole = gpu.GetUniformLocation(m_maskShader, "uRole");
    m_locOutlineMask = gpu.GetUniformLocation(m_outlineShader, "uMask");
    m_locOutlineSelectedColor = gpu.GetUniformLocation(m_outlineShader, "uSelectedColor");
    m_locOutlineSelectedWidth = gpu.GetUniformLocation(m_outlineShader, "uSelectedWidth");
    m_locOutlineHoverColor = gpu.GetUniformLocation(m_outlineShader, "uHoverColor");
    m_locOutlineHoverWidth = gpu.GetUniformLocation(m_outlineShader, "uHoverWidth");
    m_locOutlineOccludedAlpha = gpu.GetUniformLocation(m_outlineShader, "uOccludedAlpha");
    m_locOutlineDashed = gpu.GetUniformLocation(m_outlineShader, "uOccludedDashed");
    (void)context;
    return true;
}

void SelectionOutlinePass::Resize(RenderContext& context, int newWidth, int newHeight) {
    if (m_mask) m_mask->Resize(newWidth, newHeight);
}

void SelectionOutlinePass::Execute(RenderContext& ctx,
                                   const std::shared_ptr<SceneGraph>& sceneGraph,
                                   const std::shared_ptr<Camera>& camera,
                                   const std::shared_ptr<DirectionalLight>& dirLight,
                                   const std::shared_ptr<Skybox>& skybox) {
    m_lastDrawCount = 0;
    const RenderContext::SelectionOutline& sel = ctx.selectionOutline;
    if (!sel.enabled || (sel.selectedEntities.empty() && sel.hoveredEntities.empty())) {
        return;
    }
    if (!m_maskShader || !m_outlineShader || !sceneGraph || !ctx.gbufferFBO || !ctx.screenQuad) {
        return;
    }
    RenderSystem* renderSystem = sceneGraph->GetRenderSystem();
    if (!renderSystem || ctx.width <= 0 || ctx.height <= 0) {
        return;
    }

    if (!m_mask) {
        m_mask = std::make_unique<FrameBuffer>(ctx.width, ctx.height, std::vector<rhi::Format>{ rhi::Format::RGBA8 });
        if (!m_mask->IsComplete()) {
            std::cerr << "[SelectionOutlinePass] Mask framebuffer not complete\n";
            m_mask.reset();
            return;
        }
    }
    else if (m_mask->GetWidth() != ctx.width || m_mask->GetHeight() != ctx.height) {
        m_mask->Resize(ctx.width, ctx.height);
    }

    rhi::Device& gpu = rhi::Gpu();
    const rhi::StateSnapshot saved = gpu.CaptureState();
    const bool blendWasEnabled = gpu.IsBlendEnabled();
    const auto restoreState = [&]() {
        gpu.BindFramebuffer(PresentTarget::Get());
        gpu.SetDepthTest(saved.depthTest);
        gpu.SetDepthWrite(saved.depthWrite);
        gpu.SetDepthFunc(saved.depthFunc);
        gpu.SetCullEnabled(saved.cullEnabled);
        gpu.SetCullFace(saved.cullFace);
        gpu.SetFrontFace(saved.frontFace);
        gpu.SetBlendEnabled(blendWasEnabled);
        gpu.SetBlendFunc(saved.blend.srcColor, saved.blend.dstColor);
        gpu.UseProgram({});
    };

    // 1. Mask. Selected = channel pair 0, hovered = pair 1. Depth test keeps the front-most fragment per
    //    pixel, so the visibility test runs against the one surface that will be outlined.
    m_mask->Bind();
    gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
    gpu.SetBlendEnabled(false);
    gpu.SetColorMask(true, true, true, true);
    gpu.SetDepthTest(true);
    gpu.SetDepthWrite(true);
    gpu.SetDepthFunc(rhi::CompareOp::LessEqual);
    gpu.SetClearColor(glm::vec4(0.0f));
    gpu.SetClearDepth(1.0f);
    gpu.Clear(rhi::ClearColor | rhi::ClearDepth);

    gpu.UseProgram(m_maskShader);
    gpu.BindTexture(TextureUnits::SELECTION_SCENE_DEPTH, ctx.gbufferFBO->GetDepthTexture());
    if (m_locMaskSceneDepth >= 0) gpu.SetUniform(m_locMaskSceneDepth, TextureUnits::SELECTION_SCENE_DEPTH);
    if (m_locMaskProjAB >= 0) gpu.SetUniform(m_locMaskProjAB, glm::vec2(ctx.proj[2][2], ctx.proj[3][2]));
    if (m_locMaskRole >= 0) gpu.SetUniform(m_locMaskRole, 0);
    m_lastDrawCount += renderSystem->RenderEntityItems(m_maskShader, sel.selectedEntities, ctx.view, ctx.proj);
    if (m_locMaskRole >= 0) gpu.SetUniform(m_locMaskRole, 1);
    m_lastDrawCount += renderSystem->RenderEntityItems(m_maskShader, sel.hoveredEntities, ctx.view, ctx.proj);
    gpu.BindTexture(TextureUnits::SELECTION_SCENE_DEPTH, rhi::TextureHandle{});

    // Nothing with geometry (lights, cameras, empty nodes): the editor draws their highlight itself.
    if (m_lastDrawCount == 0) {
        restoreState();
        return;
    }

    // 2. Outline: fullscreen dilation of the mask onto the frame after tone mapping and TAA.
    gpu.BindFramebuffer(PresentTarget::Get());
    gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
    gpu.SetDepthTest(false);
    gpu.SetDepthWrite(false);
    gpu.SetCullEnabled(false);
    gpu.SetBlendEnabled(true);
    gpu.SetBlendFunc(rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::OneMinusSrcAlpha);

    gpu.UseProgram(m_outlineShader);
    gpu.BindTexture(0, m_mask->GetColorAttachment(0));
    if (m_locOutlineMask >= 0) gpu.SetUniform(m_locOutlineMask, 0);
    if (m_locOutlineSelectedColor >= 0) gpu.SetUniform(m_locOutlineSelectedColor, sel.selectedColor);
    if (m_locOutlineSelectedWidth >= 0) gpu.SetUniform(m_locOutlineSelectedWidth, std::clamp(sel.selectedWidth, 1.0f, 4.0f));
    if (m_locOutlineHoverColor >= 0) gpu.SetUniform(m_locOutlineHoverColor, kHoverColor);
    if (m_locOutlineHoverWidth >= 0) gpu.SetUniform(m_locOutlineHoverWidth, kHoverWidth);
    if (m_locOutlineOccludedAlpha >= 0) gpu.SetUniform(m_locOutlineOccludedAlpha, std::clamp(sel.occludedAlpha, 0.0f, 1.0f));
    if (m_locOutlineDashed >= 0) gpu.SetUniform(m_locOutlineDashed, sel.occludedDashed ? 1 : 0);
    ctx.screenQuad->Render();
    gpu.BindTexture(0, rhi::TextureHandle{});

    restoreState();
}
