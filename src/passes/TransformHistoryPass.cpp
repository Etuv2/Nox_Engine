#include "TransformHistoryPass.h"

#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../SceneGraph.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <iostream>
#include <vector>

TransformHistoryPass::TransformHistoryPass() = default;

TransformHistoryPass::~TransformHistoryPass()
{
    if (m_historySSBO) {
        rhi::Gpu().DestroyBuffer(m_historySSBO);
    }
    if (m_visibleListSSBO) {
        rhi::Gpu().DestroyBuffer(m_visibleListSSBO);
    }
}

bool TransformHistoryPass::Initialize(RenderContext&)
{
    m_shader = std::make_unique<ComputeShader>();
    if (!m_shader->CreateFromFile("shaders/transform_history_comp.glsl")) {
        std::cerr << "[TransformHistoryPass] Failed to create compute shader.\n";
        return false;
    }
    return true;
}

void TransformHistoryPass::Resize(RenderContext&, int, int)
{
}

void TransformHistoryPass::EnsureBuffers(size_t transformRecordCount)
{
    const size_t requiredHistoryCount = std::max<size_t>(transformRecordCount, 1);
    if (requiredHistoryCount > m_historyCapacity) {
        std::vector<TransformHistoryRecord> empty(requiredHistoryCount);
        const size_t historyBytes = empty.size() * sizeof(TransformHistoryRecord);
        if (!m_historySSBO) {
            rhi::BufferDesc desc;
            desc.size = historyBytes;
            desc.initialData = empty.data();
            desc.usage = rhi::BufferUsage::DynamicDraw;
            m_historySSBO = rhi::Gpu().CreateBuffer(desc);
        } else {
            rhi::Gpu().ReallocateBuffer(m_historySSBO, historyBytes, empty.data(), rhi::BufferUsage::DynamicDraw);
        }
        m_historyCapacity = requiredHistoryCount;
    }

    const size_t requiredVisibleCount = std::max<size_t>(transformRecordCount + 1, 2);
    if (requiredVisibleCount > m_visibleCapacity) {
        std::vector<uint32_t> zeroData(requiredVisibleCount, 0u);
        const size_t visibleBytes = zeroData.size() * sizeof(uint32_t);
        if (!m_visibleListSSBO) {
            rhi::BufferDesc desc;
            desc.size = visibleBytes;
            desc.initialData = zeroData.data();
            desc.usage = rhi::BufferUsage::DynamicDraw;
            m_visibleListSSBO = rhi::Gpu().CreateBuffer(desc);
        } else {
            rhi::Gpu().ReallocateBuffer(m_visibleListSSBO, visibleBytes, zeroData.data(), rhi::BufferUsage::DynamicDraw);
        }
        m_visibleCapacity = requiredVisibleCount;
    }
}

void TransformHistoryPass::ResetVisibleListHeader()
{
    if (!m_visibleListSSBO || !m_historySSBO) {
        return;
    }

    const uint32_t zeros[4] = { 0u, 0u, 0u, 0u };
    std::vector<TransformHistoryRecord> emptyHistory(m_historyCapacity);
    rhi::Gpu().UpdateBuffer(m_historySSBO, 0,
        emptyHistory.size() * sizeof(TransformHistoryRecord),
        emptyHistory.data());
    rhi::Gpu().UpdateBuffer(m_visibleListSSBO, 0, sizeof(zeros), zeros);
}

void TransformHistoryPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>& sceneGraph,
    const std::shared_ptr<Camera>&,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&)
{
    if (!m_shader || !m_shader->IsValid() || !ctx.gbufferFBO || !sceneGraph) {
        return;
    }

    auto* renderSystem = sceneGraph->GetRenderSystem();
    if (!renderSystem) {
        return;
    }

    const rhi::BufferHandle transformBuffer = renderSystem->GetTransformBufferID();
    const size_t transformCount = renderSystem->GetTransformRecordCount();
    if (!transformBuffer || transformCount == 0) {
        return;
    }

    EnsureBuffers(transformCount);
    ResetVisibleListHeader();

    rhi::Gpu().UseProgram(m_shader->GetProgramID());

    BIND_TEXTURE_2D(TextureUnits::GBUFFER_TRANSFORM_ID, ctx.gbufferFBO->GetColorAttachment(5));

    m_shader->SetUniform("uTransformIDBuffer", TextureUnits::GBUFFER_TRANSFORM_ID);
    m_shader->SetUniform("uResolution", glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));
    m_shader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_shader->SetUniform("uMaxTransformID", static_cast<int>(transformCount));

    rhi::Gpu().BindStorageBuffer(6, transformBuffer);
    rhi::Gpu().BindStorageBuffer(7, m_historySSBO);
    rhi::Gpu().BindStorageBuffer(8, m_visibleListSSBO);

    const uint32_t groupsX = (static_cast<uint32_t>(ctx.width) + 7u) / 8u;
    const uint32_t groupsY = (static_cast<uint32_t>(ctx.height) + 7u) / 8u;
    m_shader->Dispatch(groupsX, groupsY, 1);
    m_shader->WaitForCompletion(rhi::BarrierStorage | rhi::BarrierImageAccess);
    rhi::Gpu().UseProgram(rhi::ProgramHandle{});

    ++m_frameIndex;
}
