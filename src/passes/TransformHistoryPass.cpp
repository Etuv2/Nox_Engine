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
        glDeleteBuffers(1, &m_historySSBO);
    }
    if (m_visibleListSSBO) {
        glDeleteBuffers(1, &m_visibleListSSBO);
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
        if (!m_historySSBO) {
            glGenBuffers(1, &m_historySSBO);
        }
        std::vector<TransformHistoryRecord> empty(requiredHistoryCount);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_historySSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            static_cast<GLsizeiptr>(empty.size() * sizeof(TransformHistoryRecord)),
            empty.data(),
            GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        m_historyCapacity = requiredHistoryCount;
    }

    const size_t requiredVisibleCount = std::max<size_t>(transformRecordCount + 1, 2);
    if (requiredVisibleCount > m_visibleCapacity) {
        if (!m_visibleListSSBO) {
            glGenBuffers(1, &m_visibleListSSBO);
        }
        std::vector<uint32_t> zeroData(requiredVisibleCount, 0u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_visibleListSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            static_cast<GLsizeiptr>(zeroData.size() * sizeof(uint32_t)),
            zeroData.data(),
            GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_historySSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
        static_cast<GLsizeiptr>(emptyHistory.size() * sizeof(TransformHistoryRecord)),
        emptyHistory.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_visibleListSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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

    const GLuint transformBuffer = renderSystem->GetTransformBufferID();
    const size_t transformCount = renderSystem->GetTransformRecordCount();
    if (!transformBuffer || transformCount == 0) {
        return;
    }

    EnsureBuffers(transformCount);
    ResetVisibleListHeader();

    glUseProgram(m_shader->GetProgramID());

    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_TRANSFORM_ID);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));

    m_shader->SetUniform("uTransformIDBuffer", TextureUnits::GBUFFER_TRANSFORM_ID);
    m_shader->SetUniform("uResolution", glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));
    m_shader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_shader->SetUniform("uMaxTransformID", static_cast<int>(transformCount));

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, transformBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_historySSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_visibleListSSBO);

    const GLuint groupsX = (static_cast<GLuint>(ctx.width) + 7u) / 8u;
    const GLuint groupsY = (static_cast<GLuint>(ctx.height) + 7u) / 8u;
    m_shader->Dispatch(groupsX, groupsY, 1);
    m_shader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);

    ++m_frameIndex;
}
