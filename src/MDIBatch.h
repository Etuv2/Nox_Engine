#pragma once
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include "rhi/RHI.h"
#include "GpuBuffer.h"
/*
* Multi-Draw Indirect (MDI) Batch for rendering multiple objects with a single draw call.
* Each object has its own model matrix and draw parameters.
* Supports grouping by VAO for efficient rendering.
*/
// Standalone MDI batch used by SceneNode/SceneGraph/LightManager to avoid circular deps
struct MDI_RenderableObject {
	uint32_t count = 0;
	uint32_t firstIndex = 0;
	uint32_t baseVertex = 0;
	glm::mat4 modelMatrix{ 1.0f };
	uint32_t transformID = 0;
	glm::vec4 boundingSphere{ 0.0f }; // x, y, z, radius (optional)
	// Vertex input (vertex + index buffers) for this draw
	rhi::VertexInputHandle vao;
};

// Indirect indexed draw arguments (DrawElementsIndirectCommand layout).
struct MDI_DrawCommand {
	uint32_t count = 0;
	uint32_t instanceCount = 1;
	uint32_t firstIndex = 0;
	uint32_t baseVertex = 0;
	uint32_t baseInstance = 0;
};

class MDIBatch {
public:
	MDIBatch() = default;
	~MDIBatch() = default;

	// Move semantics for unique_ptr members
	MDIBatch(MDIBatch&& other) noexcept
		: m_objects(std::move(other.m_objects))
		, m_indirectBuffer(std::move(other.m_indirectBuffer))
		, m_modelMatrixSSBO(std::move(other.m_modelMatrixSSBO))
		, m_groupCache(std::move(other.m_groupCache))
		, m_tempCommands(std::move(other.m_tempCommands))
		, m_tempModelMats(std::move(other.m_tempModelMats))
		, m_isDirty(other.m_isDirty)
		, m_uploadedCount(other.m_uploadedCount)
	{
		other.m_isDirty = true;
		other.m_uploadedCount = 0;
	}

	MDIBatch& operator=(MDIBatch&& other) noexcept {
		if (this != &other) {
			m_objects = std::move(other.m_objects);
			m_indirectBuffer = std::move(other.m_indirectBuffer);
			m_modelMatrixSSBO = std::move(other.m_modelMatrixSSBO);
			m_groupCache = std::move(other.m_groupCache);
			m_tempCommands = std::move(other.m_tempCommands);
			m_tempModelMats = std::move(other.m_tempModelMats);
			m_isDirty = other.m_isDirty;
			m_uploadedCount = other.m_uploadedCount;
			other.m_isDirty = true;
			other.m_uploadedCount = 0;
		}
		return *this;
	}

	// Delete copy operations (unique_ptr can't be copied)
	MDIBatch(const MDIBatch&) = delete;
	MDIBatch& operator=(const MDIBatch&) = delete;

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
			cmd.baseInstance = static_cast<uint32_t>(i); // index into model matrix array
			m_tempCommands.push_back(cmd);
		}

		// Create indirect buffer if needed or if previous buffer became invalid
		if (!m_indirectBuffer || !m_indirectBuffer->IsValid()) {
			m_indirectBuffer = std::make_unique<GpuBuffer>(
				BufferType::DrawIndirect,
				BufferUsage::DynamicDraw
			);
			m_indirectBuffer->SetLabel("MDIBatch_IndirectBuffer");
		}
		if (!m_indirectBuffer->SetData(m_tempCommands)) {
			// Buffer allocation failed - recreate the buffer
			m_indirectBuffer = std::make_unique<GpuBuffer>(
				BufferType::DrawIndirect,
				BufferUsage::DynamicDraw
			);
			m_indirectBuffer->SetLabel("MDIBatch_IndirectBuffer");
			m_indirectBuffer->SetData(m_tempCommands);
		}

		// Upload a tightly packed array of model matrices for SSBO binding=3
		m_tempModelMats.clear();
		m_tempModelMats.reserve(m_objects.size());
		for (const auto& o : m_objects) m_tempModelMats.push_back(o.modelMatrix);

		// Create SSBO if needed or if previous buffer became invalid
		if (!m_modelMatrixSSBO || !m_modelMatrixSSBO->IsValid()) {
			m_modelMatrixSSBO = std::make_unique<GpuBuffer>(
				BufferType::ShaderStorage,
				BufferUsage::DynamicDraw
			);
			m_modelMatrixSSBO->SetLabel("MDIBatch_ModelMatrixSSBO");
		}
		if (!m_modelMatrixSSBO->SetData(m_tempModelMats)) {
			// Buffer allocation failed - recreate the buffer
			m_modelMatrixSSBO = std::make_unique<GpuBuffer>(
				BufferType::ShaderStorage,
				BufferUsage::DynamicDraw
			);
			m_modelMatrixSSBO->SetLabel("MDIBatch_ModelMatrixSSBO");
			m_modelMatrixSSBO->SetData(m_tempModelMats);
		}

		m_uploadedCount = m_objects.size();
		m_isDirty = false;
	}

	void Bind() const {
		// Bind model matrices SSBO at binding=3 (shader must match)
		if (m_modelMatrixSSBO) m_modelMatrixSSBO->BindBase(3);
	}

	int GetCommandCount() const { return static_cast<int>(m_objects.size()); }

	// Render MDI draws grouped by vertex input so the correct vertex/index buffers are used
	void RenderBatchedByVAO(rhi::Topology mode = rhi::Topology::Triangles, rhi::IndexType indexType = rhi::IndexType::UInt32) {
		if (m_objects.empty()) return;

		// Ensure buffers exist and are bound - only upload if dirty
		UploadToGPU();
		if (m_modelMatrixSSBO) m_modelMatrixSSBO->BindBase(3);

		// Rebuild grouping cache if dirty
		if (m_isDirty || m_groupCache.empty()) {
			RebuildGroupCache();
		}

		// Single allocation for all commands across all VAO groups
		m_tempCommands.clear();
		m_tempCommands.reserve(m_objects.size());

		if (!m_indirectBuffer) {
			m_indirectBuffer = std::make_unique<GpuBuffer>(
				BufferType::DrawIndirect,
				BufferUsage::DynamicDraw
			);
		}

		// Build complete command buffer with proper offsets
		struct DrawGroup {
			rhi::VertexInputHandle vao;
			size_t offset;
			size_t count;
		};
		std::vector<DrawGroup> drawGroups;
		drawGroups.reserve(m_groupCache.size());

		for (const auto& kv : m_groupCache) {
			const rhi::VertexInputHandle vao(kv.first);
			const auto& indices = kv.second;
			if (!vao || indices.empty()) continue;

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
				cmd.baseInstance = static_cast<uint32_t>(idx);
				m_tempCommands.push_back(cmd);
			}

			drawGroups.push_back(group);
		}

		// Single upload for all commands
		if (!m_tempCommands.empty()) {
			m_indirectBuffer->SetData(m_tempCommands);
		}

		// Issue draws for each group using offsets into the single buffer
		rhi::Device& gpu = rhi::Gpu();
		for (const auto& group : drawGroups) {
			gpu.BindVertexInput(group.vao);
			gpu.DrawIndexedIndirect(mode, indexType, m_indirectBuffer->GetID(), group.offset * sizeof(MDI_DrawCommand),
				static_cast<uint32_t>(group.count), sizeof(MDI_DrawCommand));
			gpu.BindVertexInput({});
		}
	}

	// Compatibility path: set a per-draw uniform uObjectIndex and issue one indirect draw per command
	// Requires the current shader program to have a uniform int uObjectIndex
	void RenderBatchedByVAOWithUniform(rhi::Topology mode, rhi::IndexType indexType, rhi::UniformLocation locObjectIndex) {
		if (m_objects.empty()) return;
		UploadToGPU();
		if (m_modelMatrixSSBO) m_modelMatrixSSBO->BindBase(3);

		// Rebuild grouping cache if dirty
		if (m_isDirty || m_groupCache.empty()) {
			RebuildGroupCache();
		}

		if (!m_indirectBuffer) {
			m_indirectBuffer = std::make_unique<GpuBuffer>(
				BufferType::DrawIndirect,
				BufferUsage::DynamicDraw
			);
		}

		rhi::Device& gpu = rhi::Gpu();
		for (const auto& kv : m_groupCache) {
			const rhi::VertexInputHandle vao(kv.first);
			const auto& indices = kv.second;
			if (!vao || indices.empty()) continue;

			m_tempCommands.clear();
			m_tempCommands.reserve(indices.size());
			for (size_t idx : indices) {
				const auto& obj = m_objects[idx];
				MDI_DrawCommand cmd{};
				cmd.count = obj.count;
				cmd.instanceCount = 1;
				cmd.firstIndex = obj.firstIndex;
				cmd.baseVertex = obj.baseVertex;
				cmd.baseInstance = static_cast<uint32_t>(idx);
				m_tempCommands.push_back(cmd);
			}

			// Upload commands for this VAO
			m_indirectBuffer->SetData(m_tempCommands);
			gpu.BindVertexInput(vao);

			for (size_t i = 0; i < m_tempCommands.size(); ++i) {
				if (locObjectIndex >= 0) gpu.SetUniform(locObjectIndex, static_cast<int>(m_tempCommands[i].baseInstance));
				gpu.DrawIndexedIndirect(mode, indexType, m_indirectBuffer->GetID(), i * sizeof(MDI_DrawCommand), 1, sizeof(MDI_DrawCommand));
			}

			gpu.BindVertexInput({});
		}
	}

private:
	void RebuildGroupCache() {
		m_groupCache.clear();
		m_groupCache.reserve(m_objects.size() / 4); // Estimate
		for (size_t i = 0; i < m_objects.size(); ++i) {
			m_groupCache[m_objects[i].vao.id].push_back(i);
		}
	}

	std::vector<MDI_RenderableObject> m_objects;
	mutable GpuBufferPtr m_indirectBuffer;     // indirect draw arguments
	mutable GpuBufferPtr m_modelMatrixSSBO;    // storage buffer (binding=3) with mat4 modelMatrices[]

	// Performance optimizations: cached data and reusable buffers
	mutable std::unordered_map<uint32_t, std::vector<size_t>> m_groupCache;   // vertex input id -> objects
	mutable std::vector<MDI_DrawCommand> m_tempCommands;
	mutable std::vector<glm::mat4> m_tempModelMats;
	bool m_isDirty = true;
	size_t m_uploadedCount = 0;
};
