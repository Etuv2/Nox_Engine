#include "GpuBuffer.h"
#include "SliceTask.h"
#include <iostream>

namespace {
	rhi::BufferHandle CreateEmpty(BufferUsage usage) {
		rhi::BufferDesc desc;
		desc.size = 0;
		desc.usage = usage;
		return rhi::Gpu().CreateBuffer(desc);
	}
}

GpuBuffer::GpuBuffer(BufferType type, BufferUsage usage)
	: m_type(type)
	, m_usage(usage)
{
	// The buffer object exists (and IsValid()) before storage is allocated.
	m_buffer = CreateEmpty(usage);
	if (!m_buffer) std::cerr << "[GpuBuffer] Failed to create buffer object\n";
}

GpuBuffer::GpuBuffer(BufferType type, size_t sizeBytes, const void* data, BufferUsage usage)
	: m_type(type)
	, m_usage(usage)
{
	m_buffer = CreateEmpty(usage);
	if (!m_buffer) {
		std::cerr << "[GpuBuffer] Failed to create buffer object\n";
		return;
	}
	Allocate(sizeBytes, data);
}

GpuBuffer::~GpuBuffer() {
	Release();
}

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
	: m_buffer(other.m_buffer)
	, m_type(other.m_type)
	, m_usage(other.m_usage)
	, m_size(other.m_size)
	, m_isImmutable(other.m_isImmutable)
	, m_isMapped(other.m_isMapped)
{
	other.m_buffer = {};
	other.m_size = 0;
	other.m_isMapped = false;
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
	if (this != &other) {
		Release();
		m_buffer = other.m_buffer;
		m_type = other.m_type;
		m_usage = other.m_usage;
		m_size = other.m_size;
		m_isImmutable = other.m_isImmutable;
		m_isMapped = other.m_isMapped;
		other.m_buffer = {};
		other.m_size = 0;
		other.m_isMapped = false;
	}
	return *this;
}

void GpuBuffer::Release() {
	if (m_buffer && rhi::HasGpu()) {
		if (m_isMapped) {
			rhi::Gpu().UnmapBuffer(m_buffer);
			m_isMapped = false;
		}
		rhi::Gpu().DestroyBuffer(m_buffer);
	}
	m_buffer = {};
	m_size = 0;
	m_isImmutable = false;
}

bool GpuBuffer::Allocate(size_t sizeBytes, const void* data) {
	if (data) SliceTask::ChargeUpload(sizeBytes);  // upload budget: a frame's uploads are bounded (SliceTask.h)
	if (m_isImmutable) {
		std::cerr << "[GpuBuffer] Cannot reallocate immutable buffer (create a new one)\n";
		return false;
	}
	if (sizeBytes == 0) {
		m_size = 0;
		return true;
	}
	rhi::Device& gpu = rhi::Gpu();
	if (!m_buffer) {
		std::cerr << "[GpuBuffer] Cannot allocate: invalid buffer\n";
		return false;
	}
	gpu.ReallocateBuffer(m_buffer, sizeBytes, data, m_usage);
	if (const char* err = gpu.PollError()) {
		std::cerr << "[GpuBuffer] Error in Allocate (size=" << sizeBytes << "): " << err << std::endl;
		return false;
	}
	m_size = sizeBytes;
	return true;
}

bool GpuBuffer::AllocateImmutable(size_t sizeBytes, rhi::BufferStorage flags, const void* data) {
	if (m_isImmutable) {
		std::cerr << "[GpuBuffer] Buffer already has immutable storage\n";
		return false;
	}
	if (sizeBytes == 0) {
		m_size = 0;
		return true;
	}
	rhi::Device& gpu = rhi::Gpu();
	if (m_buffer) gpu.DestroyBuffer(m_buffer);   // fixed storage needs a fresh buffer object
	rhi::BufferDesc desc;
	desc.size = sizeBytes;
	desc.initialData = data;
	desc.immutable = true;
	desc.storage = flags;
	m_buffer = gpu.CreateBuffer(desc);
	if (!m_buffer) {
		std::cerr << "[GpuBuffer] AllocateImmutable failed (size=" << sizeBytes << ")\n";
		return false;
	}
	m_size = sizeBytes;
	m_isImmutable = true;
	return true;
}

bool GpuBuffer::Reallocate(size_t sizeBytes, const void* data) {
	if (m_isImmutable) {
		std::cerr << "[GpuBuffer] Cannot reallocate immutable buffer - create new buffer\n";
		return false;
	}
	return Allocate(sizeBytes, data);
}

void GpuBuffer::SubData(size_t offsetBytes, size_t sizeBytes, const void* data) {
	if (!m_buffer || data == nullptr) return;
	if (offsetBytes + sizeBytes > m_size) {
		std::cerr << "[GpuBuffer] SubData: range exceeds buffer size\n";
		return;
	}
	rhi::Gpu().UpdateBuffer(m_buffer, offsetBytes, sizeBytes, data);
}

void GpuBuffer::BindBase(uint32_t index) const {
	switch (m_type) {
	case BufferType::Uniform: rhi::Gpu().BindUniformBuffer(index, m_buffer); break;
	case BufferType::AtomicCounter: rhi::Gpu().BindAtomicCounterBuffer(index, m_buffer); break;
	default: rhi::Gpu().BindStorageBuffer(index, m_buffer); break;
	}
}

void GpuBuffer::BindRange(uint32_t index, size_t offsetBytes, size_t sizeBytes) const {
	switch (m_type) {
	case BufferType::Uniform: rhi::Gpu().BindUniformBuffer(index, m_buffer, offsetBytes, sizeBytes); break;
	case BufferType::AtomicCounter: rhi::Gpu().BindAtomicCounterBuffer(index, m_buffer, offsetBytes, sizeBytes); break;
	default: rhi::Gpu().BindStorageBuffer(index, m_buffer, offsetBytes, sizeBytes); break;
	}
}

void* GpuBuffer::MapRangeRaw(size_t offsetBytes, size_t sizeBytes, rhi::MapAccess access) {
	if (!m_buffer || m_isMapped) return nullptr;
	void* ptr = rhi::Gpu().MapBuffer(m_buffer, offsetBytes, sizeBytes, access);
	m_isMapped = ptr != nullptr;
	return ptr;
}

bool GpuBuffer::Unmap() {
	if (!m_buffer || !m_isMapped) return false;
	rhi::Gpu().UnmapBuffer(m_buffer);
	m_isMapped = false;
	return true;
}

void GpuBuffer::FlushMappedRange(size_t offsetBytes, size_t sizeBytes) {
	if (m_buffer && m_isMapped) rhi::Gpu().FlushMappedRange(m_buffer, offsetBytes, sizeBytes);
}

void GpuBuffer::CopyFrom(const GpuBuffer& source, size_t readOffset, size_t writeOffset, size_t sizeBytes) {
	if (!m_buffer || !source.m_buffer) return;
	rhi::Gpu().CopyBuffer(source.m_buffer, readOffset, m_buffer, writeOffset, sizeBytes);
}

void GpuBuffer::Read(size_t offsetBytes, size_t sizeBytes, void* out) const {
	if (m_buffer) rhi::Gpu().ReadBuffer(m_buffer, offsetBytes, sizeBytes, out);
}

void GpuBuffer::SetLabel(const std::string& label) {
	if (m_buffer) rhi::Gpu().SetDebugName(m_buffer, label.c_str());
}
