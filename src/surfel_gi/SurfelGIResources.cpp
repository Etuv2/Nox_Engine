#include "SurfelGIResources.h"

#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
bool CheckedBufferSize(uint64_t elementCount, uint64_t elementSize, std::ptrdiff_t& outSize)
{
    if (elementCount == 0 || elementSize == 0) {
        return false;
    }

    const uint64_t maxGLSize = static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (elementCount > maxGLSize / elementSize) {
        return false;
    }

    outSize = static_cast<std::ptrdiff_t>(elementCount * elementSize);
    return true;
}

bool CheckedElementCount(uint64_t a, uint64_t b, uint64_t& outCount)
{
    if (a == 0 || b == 0) {
        return false;
    }

    if (a > std::numeric_limits<uint64_t>::max() / b) {
        return false;
    }

    outCount = a * b;
    return true;
}

void LabelBuffer(rhi::BufferHandle buffer, const char* label)
{
    if (buffer.IsValid() && label != nullptr) {
        rhi::Gpu().SetDebugName(buffer, label);
    }
}

void DeleteBuffer(rhi::BufferHandle& buffer)
{
    if (buffer.IsValid()) {
        rhi::Gpu().DestroyBuffer(buffer);
        buffer = {};
    }
}

bool AllocateSSBO(rhi::BufferHandle& buffer, std::ptrdiff_t sizeBytes, const void* data, const char* label)
{
    if (sizeBytes <= 0) {
        return false;
    }

    rhi::Device& gpu = rhi::Gpu();
    while (gpu.PollError() != nullptr) {
    }

    if (!buffer.IsValid()) {
        rhi::BufferDesc desc{};
        desc.size = static_cast<size_t>(sizeBytes);
        desc.initialData = data;
        desc.usage = rhi::BufferUsage::DynamicDraw;
        buffer = gpu.CreateBuffer(desc);
        if (!buffer.IsValid()) {
            std::cerr << "[SurfelGIResources] Failed to generate SSBO for " << label << ".\n";
            return false;
        }
    }
    else {
        gpu.ReallocateBuffer(buffer, static_cast<size_t>(sizeBytes), data, rhi::BufferUsage::DynamicDraw);
    }
    const char* error = gpu.PollError();

    if (error != nullptr) {
        std::cerr << "[SurfelGIResources] glBufferData failed for " << label
            << " (" << sizeBytes << " bytes, " << error << ").\n";
        DeleteBuffer(buffer);
        return false;
    }

    LabelBuffer(buffer, label);
    return true;
}

void UploadBuffer(rhi::BufferHandle buffer, std::ptrdiff_t sizeBytes, const void* data)
{
    if (!buffer.IsValid() || sizeBytes <= 0 || data == nullptr) {
        return;
    }

    rhi::Gpu().UpdateBuffer(buffer, 0, static_cast<size_t>(sizeBytes), data);
}

template <typename T>
void UploadStruct(rhi::BufferHandle buffer, const T& value)
{
    UploadBuffer(buffer, static_cast<std::ptrdiff_t>(sizeof(T)), &value);
}

void ClearBufferUInt(rhi::BufferHandle buffer, uint32_t value)
{
    if (!buffer.IsValid()) {
        return;
    }

    rhi::Gpu().ClearBuffer(buffer, rhi::Format::R32UI, rhi::PixelFormat::RInt, rhi::PixelType::UInt, &value);
}
}

SurfelPool::SurfelPool() = default;

SurfelPool::~SurfelPool()
{
    Destroy();
}

bool SurfelPool::Create(uint32_t maxSurfels)
{
    return ResizeReset(maxSurfels);
}

bool SurfelPool::ResizeReset(uint32_t maxSurfels)
{
    Destroy();

    std::ptrdiff_t surfelBytes = 0;
    std::ptrdiff_t freeListBytes = 0;
    std::ptrdiff_t counterBytes = 0;
    if (!CheckedBufferSize(maxSurfels, sizeof(Surfel), surfelBytes) ||
        !CheckedBufferSize(maxSurfels, sizeof(uint32_t), freeListBytes) ||
        !CheckedBufferSize(1u, sizeof(SurfelCounters), counterBytes)) {
        std::cerr << "[SurfelPool] Invalid surfel pool size: " << maxSurfels << ".\n";
        return false;
    }

    m_maxSurfels = maxSurfels;
    const bool ok =
        AllocateSSBO(m_surfelBuffer, surfelBytes, nullptr, "SurfelPool.Surfels") &&
        AllocateSSBO(m_freeListBuffer, freeListBytes, nullptr, "SurfelPool.FreeList") &&
        AllocateSSBO(m_countersBuffer, counterBytes, nullptr, "SurfelPool.Counters");

    if (!ok) {
        Destroy();
        return false;
    }

    ResetFreeList();
    return IsCreated();
}

void SurfelPool::Destroy()
{
    DeleteBuffer(m_surfelBuffer);
    DeleteBuffer(m_freeListBuffer);
    DeleteBuffer(m_countersBuffer);
    m_maxSurfels = 0;
}

void SurfelPool::ResetFreeList()
{
    if (!IsCreated() || m_maxSurfels == 0) {
        return;
    }

    ClearBufferUInt(m_surfelBuffer, 0u);

    std::vector<uint32_t> freeIndices(m_maxSurfels);
    for (uint32_t i = 0; i < m_maxSurfels; ++i) {
        freeIndices[i] = m_maxSurfels - 1u - i;
    }
    UploadBuffer(
        m_freeListBuffer,
        static_cast<std::ptrdiff_t>(freeIndices.size() * sizeof(uint32_t)),
        freeIndices.data());

    SurfelCounters counters{};
    counters.freeTop = m_maxSurfels;
    counters.liveCount = 0u;
    UploadStruct(m_countersBuffer, counters);
}

SurfelGrid::SurfelGrid() = default;

SurfelGrid::~SurfelGrid()
{
    Destroy();
}

bool SurfelGrid::Create(const SurfelGridSettings& settings)
{
    return ResizeReset(settings);
}

bool SurfelGrid::ResizeReset(const SurfelGridSettings& settings)
{
    Destroy();

    const uint64_t cellCount64 = settings.CellCount();
    uint64_t entryCount64 = 0;
    if (cellCount64 == 0 ||
        settings.maxSurfelsPerCell == 0 ||
        cellCount64 > std::numeric_limits<uint32_t>::max() ||
        !CheckedElementCount(cellCount64, settings.maxSurfelsPerCell, entryCount64) ||
        entryCount64 > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "[SurfelGrid] Invalid grid settings.\n";
        return false;
    }

    std::ptrdiff_t headerBytes = 0;
    std::ptrdiff_t entryBytes = 0;
    std::ptrdiff_t counterBytes = 0;
    if (!CheckedBufferSize(cellCount64, sizeof(SurfelCellHeader), headerBytes) ||
        !CheckedBufferSize(entryCount64, sizeof(SurfelCellEntry), entryBytes) ||
        !CheckedBufferSize(1u, sizeof(SurfelGridCounters), counterBytes)) {
        std::cerr << "[SurfelGrid] Grid buffer byte size overflow.\n";
        return false;
    }

    m_settings = settings;
    m_cellCount = static_cast<uint32_t>(cellCount64);
    m_entryCapacity = static_cast<uint32_t>(entryCount64);

    const bool ok =
        AllocateSSBO(m_cellHeaderBuffer, headerBytes, nullptr, "SurfelGrid.Headers") &&
        AllocateSSBO(m_cellEntryBuffer, entryBytes, nullptr, "SurfelGrid.Entries") &&
        AllocateSSBO(m_countersBuffer, counterBytes, nullptr, "SurfelGrid.Counters");

    if (!ok) {
        Destroy();
        return false;
    }

    Clear();
    return IsCreated();
}

void SurfelGrid::Destroy()
{
    DeleteBuffer(m_cellHeaderBuffer);
    DeleteBuffer(m_cellEntryBuffer);
    DeleteBuffer(m_countersBuffer);
    m_cellCount = 0;
    m_entryCapacity = 0;
    m_settings = SurfelGridSettings{};
}

void SurfelGrid::Clear()
{
    if (!IsCreated()) {
        return;
    }

    ClearBufferUInt(m_cellHeaderBuffer, 0u);
    ClearBufferUInt(m_cellEntryBuffer, 0xffffffffu);

    SurfelGridCounters counters{};
    counters.cellCount = m_cellCount;
    counters.entryCapacity = m_entryCapacity;
    UploadStruct(m_countersBuffer, counters);
}

SurfelRayQueue::SurfelRayQueue() = default;

SurfelRayQueue::~SurfelRayQueue()
{
    Destroy();
}

bool SurfelRayQueue::Create(uint32_t maxRays, uint32_t maxSurfels)
{
    return ResizeReset(maxRays, maxSurfels);
}

bool SurfelRayQueue::ResizeReset(uint32_t maxRays, uint32_t maxSurfels)
{
    Destroy();

    // Allocation scratch: 4 header words and 1024 remainder bins (SURFEL_RAY_ALLOC_BINS in allocate_rays.comp).
    constexpr uint32_t kRayAllocScratchWords = 4u + 1024u;
    std::ptrdiff_t requestBytes = 0;
    std::ptrdiff_t rayBytes = 0;
    std::ptrdiff_t hitBytes = 0;
    std::ptrdiff_t scratchBytes = 0;
    if (!CheckedBufferSize(maxSurfels, sizeof(SurfelRayRequest), requestBytes) ||
        !CheckedBufferSize(maxRays, sizeof(SurfelRay), rayBytes) ||
        !CheckedBufferSize(maxRays, sizeof(SurfelRayHit), hitBytes) ||
        !CheckedBufferSize(kRayAllocScratchWords, sizeof(uint32_t), scratchBytes)) {
        std::cerr << "[SurfelRayQueue] Invalid queue size: maxRays=" << maxRays
            << ", maxSurfels=" << maxSurfels << ".\n";
        return false;
    }

    m_maxRays = maxRays;
    m_maxSurfels = maxSurfels;

    const bool ok =
        AllocateSSBO(m_requestBuffer, requestBytes, nullptr, "SurfelRayQueue.Requests") &&
        AllocateSSBO(m_rayBuffer, rayBytes, nullptr, "SurfelRayQueue.Rays") &&
        AllocateSSBO(m_rayHitBuffer, hitBytes, nullptr, "SurfelRayQueue.Hits") &&
        AllocateSSBO(m_allocScratchBuffer, scratchBytes, nullptr, "SurfelRayQueue.AllocScratch");

    if (!ok) {
        Destroy();
        return false;
    }

    Clear();
    return IsCreated();
}

void SurfelRayQueue::Destroy()
{
    DeleteBuffer(m_requestBuffer);
    DeleteBuffer(m_rayBuffer);
    DeleteBuffer(m_rayHitBuffer);
    DeleteBuffer(m_allocScratchBuffer);
    m_maxRays = 0;
    m_maxSurfels = 0;
}

void SurfelRayQueue::Clear()
{
    if (!IsCreated()) {
        return;
    }

    ClearBufferUInt(m_requestBuffer, 0u);
    ClearBufferUInt(m_rayBuffer, 0u);
    ClearBufferUInt(m_rayHitBuffer, 0u);
    ClearBufferUInt(m_allocScratchBuffer, 0u);
}