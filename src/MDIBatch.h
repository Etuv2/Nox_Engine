#pragma once
#include <vector>
#include <unordered_map>
#include <GL/glew.h>
#include <glm/glm.hpp>
/*
* Multi-Draw Indirect (MDI) Batch for rendering multiple objects with a single draw call.
* Each object has its own model matrix and draw parameters.
* Supports grouping by VAO for efficient rendering.
*/
// Standalone MDI batch used by SceneNode/SceneGraph/LightManager to avoid circular deps
struct MDI_RenderableObject {
	GLuint count = 0;
	GLuint firstIndex = 0;
	GLuint baseVertex = 0;
	glm::mat4 modelMatrix{ 1.0f };
	glm::vec4 boundingSphere{ 0.0f }; // x, y, z, radius (optional)
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

	void Clear() {
		m_objects.clear();
		m_isDirty = true;
	}

	void AddObject(const MDI_RenderableObject& obj) {
		m_objects.push_back(obj);
		m_isDirty = true;
	}

	// Read-only access for culling/building filtered batches
	const std::vector<MDI_RenderableObject>& GetObjects() const { return m_objects; }

	void UploadToGPU() {
		if (m_objects.empty()) return;
		if (!m_isDirty && m_uploadedCount == m_objects.size()) return; // Skip if nothing changed

		// Build indirect commands from objects
		m_tempCommands.clear();
		m_tempCommands.reserve(m_objects.size());
		for (size_t i = 0; i < m_objects.size(); ++i) {
			MDI_DrawCommand cmd{};
			cmd.count = m_objects[i].count;
			cmd.instanceCount = 1;
			cmd.firstIndex = m_objects[i].firstIndex;
			cmd.baseVertex = m_objects[i].baseVertex;
			cmd.baseInstance = static_cast<GLuint>(i); // index into model matrix array
			m_tempCommands.push_back(cmd);
		}

		if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);
		glBufferData(GL_DRAW_INDIRECT_BUFFER, m_tempCommands.size() * sizeof(MDI_DrawCommand), m_tempCommands.data(), GL_DYNAMIC_DRAW);

		// Upload a tightly packed array of model matrices for SSBO binding=3
		m_tempModelMats.clear();
		m_tempModelMats.reserve(m_objects.size());
		for (const auto& o : m_objects) m_tempModelMats.push_back(o.modelMatrix);

		if (m_modelMatrixSSBO == 0) glGenBuffers(1, &m_modelMatrixSSBO);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_modelMatrixSSBO);
		glBufferData(GL_SHADER_STORAGE_BUFFER, m_tempModelMats.size() * sizeof(glm::mat4), m_tempModelMats.data(), GL_DYNAMIC_DRAW);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

		m_uploadedCount = m_objects.size();
		m_isDirty = false;
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

		// Ensure buffers exist and are bound - only upload if dirty
		UploadToGPU();
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_modelMatrixSSBO);

		// Rebuild grouping cache if dirty
		if (m_isDirty || m_groupCache.empty()) {
			RebuildGroupCache();
		}

		// Single allocation for all commands across all VAO groups
		m_tempCommands.clear();
		m_tempCommands.reserve(m_objects.size());

		if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);

		// Build complete command buffer with proper offsets
		struct DrawGroup {
			GLuint vao;
			size_t offset;
			size_t count;
		};
		std::vector<DrawGroup> drawGroups;
		drawGroups.reserve(m_groupCache.size());

		for (const auto& kv : m_groupCache) {
			GLuint vao = kv.first;
			const auto& indices = kv.second;
			if (vao == 0 || indices.empty()) continue;

			DrawGroup group;
			group.vao = vao;
			group.offset = m_tempCommands.size();
			group.count = indices.size();

			for (size_t idx : indices) {
				const auto& obj = m_objects[idx];
				MDI_DrawCommand cmd{};
				cmd.count = obj.count;
				cmd.instanceCount = 1;
				cmd.firstIndex = obj.firstIndex;
				cmd.baseVertex = obj.baseVertex;
				cmd.baseInstance = static_cast<GLuint>(idx);
				m_tempCommands.push_back(cmd);
			}

			drawGroups.push_back(group);
		}

		// Single upload for all commands
		if (!m_tempCommands.empty()) {
			glBufferData(GL_DRAW_INDIRECT_BUFFER, m_tempCommands.size() * sizeof(MDI_DrawCommand),
				m_tempCommands.data(), GL_DYNAMIC_DRAW);
		}

		// Issue draws for each VAO group using offsets into the single buffer
		for (const auto& group : drawGroups) {
			glBindVertexArray(group.vao);
			const void* offset = (const void*)(group.offset * sizeof(MDI_DrawCommand));
			glMultiDrawElementsIndirect(mode, indexType, offset, static_cast<GLsizei>(group.count), sizeof(MDI_DrawCommand));
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

		// Rebuild grouping cache if dirty
		if (m_isDirty || m_groupCache.empty()) {
			RebuildGroupCache();
		}

		if (m_indirectBuffer == 0) glGenBuffers(1, &m_indirectBuffer);
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);

		for (const auto& kv : m_groupCache) {
			GLuint vao = kv.first;
			const auto& indices = kv.second;
			if (vao == 0 || indices.empty()) continue;

			m_tempCommands.clear();
			m_tempCommands.reserve(indices.size());
			for (size_t idx : indices) {
				const auto& obj = m_objects[idx];
				MDI_DrawCommand cmd{};
				cmd.count = obj.count;
				cmd.instanceCount = 1;
				cmd.firstIndex = obj.firstIndex;
				cmd.baseVertex = obj.baseVertex;
				cmd.baseInstance = static_cast<GLuint>(idx);
				m_tempCommands.push_back(cmd);
			}

			// Upload commands for this VAO
			glBufferData(GL_DRAW_INDIRECT_BUFFER, m_tempCommands.size() * sizeof(MDI_DrawCommand),
				m_tempCommands.data(), GL_DYNAMIC_DRAW);
			glBindVertexArray(vao);

			for (GLsizei i = 0; i < (GLsizei)m_tempCommands.size(); ++i) {
				if (locObjectIndex >= 0) glUniform1i(locObjectIndex, (GLint)m_tempCommands[i].baseInstance);
				const void* offset = (const void*)(i * sizeof(MDI_DrawCommand));
				glDrawElementsIndirect(mode, indexType, offset);
			}

			glBindVertexArray(0);
		}

		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
	}

private:
	void RebuildGroupCache() {
		m_groupCache.clear();
		m_groupCache.reserve(m_objects.size() / 4); // Estimate
		for (size_t i = 0; i < m_objects.size(); ++i) {
			m_groupCache[m_objects[i].vao].push_back(i);
		}
	}

	std::vector<MDI_RenderableObject> m_objects;
	GLuint m_indirectBuffer = 0;     // GL_DRAW_INDIRECT_BUFFER
	GLuint m_modelMatrixSSBO = 0;    // GL_SHADER_STORAGE_BUFFER (binding=3) with mat4 modelMatrices[]

	// Performance optimizations: cached data and reusable buffers
	std::unordered_map<GLuint, std::vector<size_t>> m_groupCache;
	std::vector<MDI_DrawCommand> m_tempCommands;
	std::vector<glm::mat4> m_tempModelMats;
	bool m_isDirty = true;
	size_t m_uploadedCount = 0;
};
