#pragma once

#include "rhi/RHI.h"
#include <vector>
#include <memory>
#include <string>

/**
 * @brief Role of a GpuBuffer; decides which slot kind BindBase() uses.
 *
 * Vertex/index buffers are attached through rhi::VertexInputDesc (not bound directly).
 */
enum class BufferType {
	Vertex,
	Index,
	Uniform,
	ShaderStorage,
	DrawIndirect,
	DispatchIndirect,
	AtomicCounter,
	Staging,
};

using BufferUsage = rhi::BufferUsage;

/**
 * @brief GpuBuffer - RAII owner of an rhi buffer.
 *
 * Supports resizable storage (Allocate / Reallocate keep the handle), fixed storage with
 * mapping flags (AllocateImmutable), partial updates, mapping, copies and slot binding.
 *
 * @code
 *   GpuBuffer ssbo(BufferType::ShaderStorage);
 *   ssbo.AllocateImmutable(dataSize, rhi::BufferStorage::DynamicUpdate);
 *   ssbo.BindBase(0);
 *   auto* ptr = ssbo.Map<MyStruct>(rhi::MapAccess::Write);
 *   ssbo.Unmap();
 * @endcode
 */
class GpuBuffer {
public:
	explicit GpuBuffer(BufferType type = BufferType::Vertex, BufferUsage usage = BufferUsage::StaticDraw);
	GpuBuffer(BufferType type, size_t sizeBytes, const void* data, BufferUsage usage = BufferUsage::StaticDraw);
	~GpuBuffer();

	GpuBuffer(const GpuBuffer&) = delete;
	GpuBuffer& operator=(const GpuBuffer&) = delete;
	GpuBuffer(GpuBuffer&& other) noexcept;
	GpuBuffer& operator=(GpuBuffer&& other) noexcept;

	// Resizable storage (replaces the contents; the handle stays the same).
	bool Allocate(size_t sizeBytes, const void* data = nullptr);
	// Fixed-size storage with the given flags (once per buffer).
	bool AllocateImmutable(size_t sizeBytes, rhi::BufferStorage flags, const void* data = nullptr);
	bool Reallocate(size_t sizeBytes, const void* data = nullptr);

	template<typename T>
	bool SetData(const std::vector<T>& data) { return Allocate(data.size() * sizeof(T), data.data()); }
	template<typename T>
	bool SetData(const T* data, size_t count) { return Allocate(count * sizeof(T), data); }
	template<typename T>
	void UpdateSubData(const std::vector<T>& data, size_t offsetBytes = 0) { SubData(offsetBytes, data.size() * sizeof(T), data.data()); }

	void SubData(size_t offsetBytes, size_t sizeBytes, const void* data);

	// Indexed binding by role: uniform, storage or atomic-counter slot.
	void BindBase(uint32_t index) const;
	void BindRange(uint32_t index, size_t offsetBytes, size_t sizeBytes) const;

	template<typename T = void>
	T* Map(rhi::MapAccess access = rhi::MapAccess::Write) { return static_cast<T*>(MapRange(0, m_size, access)); }
	template<typename T = void>
	T* MapRange(size_t offsetBytes, size_t sizeBytes, rhi::MapAccess access) { return static_cast<T*>(MapRangeRaw(offsetBytes, sizeBytes, access)); }
	bool Unmap();
	void FlushMappedRange(size_t offsetBytes, size_t sizeBytes);
	bool IsMapped() const { return m_isMapped; }

	void CopyFrom(const GpuBuffer& source, size_t readOffset, size_t writeOffset, size_t sizeBytes);
	void Read(size_t offsetBytes, size_t sizeBytes, void* out) const;

	rhi::BufferHandle GetID() const { return m_buffer; }
	BufferType GetType() const { return m_type; }
	BufferUsage GetUsage() const { return m_usage; }
	size_t GetSize() const { return m_size; }
	bool IsValid() const { return m_buffer.IsValid(); }
	bool IsImmutable() const { return m_isImmutable; }

	void SetLabel(const std::string& label);

private:
	void* MapRangeRaw(size_t offsetBytes, size_t sizeBytes, rhi::MapAccess access);
	void Release();

	rhi::BufferHandle m_buffer;
	BufferType m_type;
	BufferUsage m_usage;
	size_t m_size = 0;
	bool m_isImmutable = false;
	bool m_isMapped = false;
};

using GpuBufferPtr = std::unique_ptr<GpuBuffer>;

template<typename... Args>
inline GpuBufferPtr MakeBuffer(Args&&... args) {
	return std::make_unique<GpuBuffer>(std::forward<Args>(args)...);
}
