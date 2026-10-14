#pragma once

#include "../RenderPass.h"
#include "../FrameBuffer.h"
#include "rhi/RHI.h"
#include <memory>

/**
 * @brief SelectionOutlinePass draws the editor's selection outline (docs/editor.md, "Selection outline").
 *
 * Runs after tone mapping and TAA, on the same target as the debug bounding boxes, so the outline is
 * neither jittered by TAA nor tone mapped. It is scheduled only when the editor's Scene viewport supplies a
 * selection (RenderContext::selectionOutline.enabled); game, standalone and benchmark output never do.
 *
 * 1. Mask: the mesh items of the selected and hovered entities (skinned meshes included) are drawn into an
 *    RGBA8 target with their own depth buffer. A fragment is "visible" when it is not behind the G-buffer
 *    depth, else "occluded". R/G hold selected visible/occluded, B/A hovered visible/occluded.
 * 2. Outline: a fullscreen pass colours the pixels within the outline width outside the silhouette.
 *    Occluded parts are drawn dashed and alpha-scaled; hovered parts use a thinner, lighter line.
 *
 * Costs nothing when no selection is set: the plan skips the pass, and the mask is allocated on first use.
 * It also returns early when the selected subtree has no mesh items (lights, cameras, empty nodes).
 */
class SelectionOutlinePass : public RenderPass {
public:
    SelectionOutlinePass();
    ~SelectionOutlinePass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    // Mesh draws of the last Execute (selected + hovered); 0 when the pass returned early.
    size_t GetLastDrawCount() const { return m_lastDrawCount; }

private:
    rhi::ProgramHandle m_maskShader;
    rhi::ProgramHandle m_outlineShader;
    rhi::UniformLocation m_locMaskSceneDepth = -1;
    rhi::UniformLocation m_locMaskProjAB = -1;
    rhi::UniformLocation m_locMaskRole = -1;
    rhi::UniformLocation m_locOutlineMask = -1;
    rhi::UniformLocation m_locOutlineSelectedColor = -1;
    rhi::UniformLocation m_locOutlineSelectedWidth = -1;
    rhi::UniformLocation m_locOutlineHoverColor = -1;
    rhi::UniformLocation m_locOutlineHoverWidth = -1;
    rhi::UniformLocation m_locOutlineOccludedAlpha = -1;
    rhi::UniformLocation m_locOutlineDashed = -1;
    std::unique_ptr<FrameBuffer> m_mask;   // RGBA8 mask + depth, allocated on first use
    size_t m_lastDrawCount = 0;
};
