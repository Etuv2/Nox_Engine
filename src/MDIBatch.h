#pragma once
#include <vector>
#include <unordered_map>
#include <GL/glew.h>
#include <glm/glm.hpp>

// Standalone MDI batch used by SceneNode/SceneGraph/LightManager to avoid circular deps
struct MDI_RenderableObject {
    GLuint count = 0;
    GLuint firstIndex = 0;
    GLuint baseVertex = 0;
    glm::mat4 modelMatrix{1.0f};
    glm::vec4 boundingSphere{0.0f}; // x, y, z, radius (optional)
    // VAO that encapsulates VBO/EBO for this draw
    GLuint vao = 0;
};

struct MDI_DrawCommand {
    GLuint count = 0;
    GLuint instanceCount = 1;
    GLuint firstIndex = 0;
    GLuint baseVertex = 0;
    GLuint baseInstance = 0;
};

class MDIBatch {
public:
    MDIBatch() = default;
    ~MDIBatch() {
        if (m_indirectBuffer) glDeleteBuffers(1, &m_indirectBuffer);
        if (m_modelMatrixSSBO) glDeleteBuffers(1, &m_modelMatrixSSBO);
    }

    void Clear() { m_objects.clear(); }

    void AddObject(const MDI_RenderableObject& obj) { m_objects.push_back(obj); }

    // Read-only access for culling/building filtered batches
    const std::vector<MDI_RenderableObject>& GetObjects() const { return m_objects; }

    void UploadToGPU() {
        if (m_objects.empty()) return;

        // Build indirect commands from objects
        std::vector<MDI_DrawCommand> commands;
        commands.reserve(m_objects.size());
        for (size_t i = 0; i < m_objects.size(); ++i) {
            MDI_DrawCommand cmd{};
            cmd.count = m_objects[i].count;
            cmd.instanceCount = 1;
            cmd.firstIndex = m_objects[i].firstIndex;
            cmd.baseVertex = m_objects[i].baseVertex;
            cmd.baseInstance = static_cast<GLuint>(i); // index into model matrix array
            commands.push_back(cmd);
        }

        if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, commands.size() * sizeof(MDI_DrawCommand), commands.data(), GL_DYNAMIC_DRAW);

        // Upload a tightly packed array of model matrices for SSBO binding=3
        std::vector<glm::mat4> modelMats;
        modelMats.reserve(m_objects.size());
        for (const auto& o : m_objects) modelMats.push_back(o.modelMatrix);

        if (m_modelMatrixSSBO == 0) glGenBuffers(1, &m_modelMatrixSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_modelMatrixSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, modelMats.size() * sizeof(glm::mat4), modelMats.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void Bind() const {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);
        // Bind model matrices SSBO at binding=3 (shader must match)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_modelMatrixSSBO);
    }

    GLsizei GetCommandCount() const { return static_cast<GLsizei>(m_objects.size()); }

    // Render MDI draws grouped by VAO so the correct EBO/VBO are bound
    void RenderBatchedByVAO(GLenum mode = GL_TRIANGLES, GLenum indexType = GL_UNSIGNED_INT) {
        if (m_objects.empty()) return;

        // Ensure buffers exist and are bound
        UploadToGPU();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_modelMatrixSSBO);

        // Group object indices by VAO
        std::unordered_map<GLuint, std::vector<size_t>> groups;
        groups.reserve(m_objects.size());
        for (size_t i = 0; i < m_objects.size(); ++i) {
            groups[m_objects[i].vao].push_back(i);
        }

        // Temporary buffer to hold commands per VAO group
        if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);

        for (const auto& kv : groups) {
            GLuint vao = kv.first;
            const auto& indices = kv.second;
            if (vao == 0 || indices.empty()) continue;

            std::vector<MDI_DrawCommand> cmds;
            cmds.reserve(indices.size());
            for (size_t idx : indices) {
                const auto& obj = m_objects[idx];
                MDI_DrawCommand cmd{};
                cmd.count = obj.count;
                cmd.instanceCount = 1;
                cmd.firstIndex = obj.firstIndex;
                cmd.baseVertex = obj.baseVertex;
                cmd.baseInstance = static_cast<GLuint>(idx); // index into modelMatrices[]
                cmds.push_back(cmd);
            }

            // Upload commands for this group
            glBufferData(GL_DRAW_INDIRECT_BUFFER, cmds.size() * sizeof(MDI_DrawCommand), cmds.data(), GL_DYNAMIC_DRAW);

            // Bind VAO/EBO and issue multi-draw for this group
            glBindVertexArray(vao);
            glMultiDrawElementsIndirect(mode, indexType, (void*)0, static_cast<GLsizei>(cmds.size()), sizeof(MDI_DrawCommand));
            glBindVertexArray(0);
        }

        // Unbind buffers to avoid state leaks
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    // Compatibility path: set a per-draw uniform uObjectIndex and issue glDrawElementsIndirect per command
    // Requires the current shader program to have a uniform int uObjectIndex
    void RenderBatchedByVAOWithUniform(GLenum mode, GLenum indexType, GLint locObjectIndex) {
        if (m_objects.empty()) return;
        UploadToGPU();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_modelMatrixSSBO);

        std::unordered_map<GLuint, std::vector<size_t>> groups;
        for (size_t i = 0; i < m_objects.size(); ++i) groups[m_objects[i].vao].push_back(i);

        if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);

        for (const auto& kv : groups) {
            GLuint vao = kv.first;
            const auto& indices = kv.second;
            if (vao == 0 || indices.empty()) continue;

            std::vector<MDI_DrawCommand> cmds;
            cmds.reserve(indices.size());
            for (size_t idx : indices) {
                const auto& obj = m_objects[idx];
                MDI_DrawCommand cmd{};
                cmd.count = obj.count;
                cmd.instanceCount = 1;
                cmd.firstIndex = obj.firstIndex;
                cmd.baseVertex = obj.baseVertex;
                cmd.baseInstance = static_cast<GLuint>(idx);
                cmds.push_back(cmd);
            }

            // Upload commands for this VAO
            glBufferData(GL_DRAW_INDIRECT_BUFFER, cmds.size() * sizeof(MDI_DrawCommand), cmds.data(), GL_DYNAMIC_DRAW);
            glBindVertexArray(vao);

            for (GLsizei i = 0; i < (GLsizei)cmds.size(); ++i) {
                if (locObjectIndex >= 0) glUniform1i(locObjectIndex, (GLint)cmds[i].baseInstance);
                const void* offset = (const void*)(i * sizeof(MDI_DrawCommand));
                glDrawElementsIndirect(mode, indexType, offset);
            }

            glBindVertexArray(0);
        }

        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

private:
    std::vector<MDI_RenderableObject> m_objects;
    GLuint m_indirectBuffer = 0;     // GL_DRAW_INDIRECT_BUFFER
    GLuint m_modelMatrixSSBO = 0;    // GL_SHADER_STORAGE_BUFFER (binding=3) with mat4 modelMatrices[]
};
