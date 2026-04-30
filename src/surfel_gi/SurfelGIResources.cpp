#include "SurfelGIResources.h"

#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
bool CheckedBufferSize(uint64_t elementCount, uint64_t elementSize, GLsizeiptr& outSize)
{
    if (elementCount == 0 || elementSize == 0) {
        return false;
    }

    const uint64_t maxGLSize = static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max());
    if (elementCount > maxGLSize / elementSize) {
        return false;
    }

    outSize = static_cast<GLsizeiptr>(elementCount * elementSize);
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

void LabelBuffer(GLuint buffer, const char* label)
{
    if (buffer != 0 && label != nullptr && GLEW_KHR_debug) {
        glObjectLabel(GL_BUFFER, buffer, static_cast<GLsizei>(std::char_traits<char>::length(label)), label);
    }
}

void DeleteBuffer(GLuint& buffer)
{
    if (buffer != 0) {
        glDeleteBuffers(1, &buffer);
        buffer = 0;
    }
}

bool AllocateSSBO(GLuint& buffer, GLsizeiptr sizeBytes, const void* data, const char* label)
{
    if (sizeBytes <= 0) {
        return false;
    }

    if (buffer == 0) {
        glGenBuffers(1, &buffer);
        if (buffer == 0) {
            std::cerr << "[SurfelGIResources] Failed to generate SSBO for " << label << ".\n";
            return false;
        }
    }

    while (glGetError() != GL_NO_ERROR) {
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeBytes, data, GL_DYNAMIC_DRAW);
    const GLenum error = glGetError();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (error != GL_NO_ERROR) {
        std::cerr << "[SurfelGIResources] glBufferData failed for " << label
            << " (" << sizeBytes << " bytes, error 0x" << std::hex << error << std::dec << ").\n";
        DeleteBuffer(buffer);
        return false;
    }

    LabelBuffer(buffer, label);
    return true;
}

void UploadBuffer(GLuint buffer, GLsizeiptr sizeBytes, const void* data)
{
    if (buffer == 0 || sizeBytes <= 0 || data == nullptr) {
        return;
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeBytes, data);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

template <typename T>
void UploadStruct(GLuint buffer, const T& value)
{
    UploadBuffer(buffer, static_cast<GLsizeiptr>(sizeof(T)), &value);
}

void ClearBufferUInt(GLuint buffer, uint32_t value)
{
    if (buffer == 0) {
        return;
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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

    GLsizeiptr surfelBytes = 0;
    GLsizeiptr freeListBytes = 0;
    GLsizeiptr counterBytes = 0;
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
        static_cast<GLsizeiptr>(freeIndices.size() * sizeof(uint32_t)),
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

    GLsizeiptr headerBytes = 0;
    GLsizeiptr entryBytes = 0;
    GLsizeiptr averageBytes = 0;
    GLsizeiptr counterBytes = 0;
    if (!CheckedBufferSize(cellCount64, sizeof(SurfelCellHeader), headerBytes) ||
        !CheckedBufferSize(entryCount64, sizeof(SurfelCellEntry), entryBytes) ||
        !CheckedBufferSize(cellCount64, sizeof(SurfelGridCellAverage), averageBytes) ||
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
        AllocateSSBO(m_cellAverageBuffer, averageBytes, nullptr, "SurfelGrid.CellAverages") &&
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
    DeleteBuffer(m_cellAverageBuffer);
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
    ClearBufferUInt(m_cellAverageBuffer, 0u);

    SurfelGridCounters counters{};
    counters.cellCount = m_cellCount;
    counters.entryCapacity = m_entryCapacity;
    UploadStruct(m_countersBuffer, counters);
}

void SurfelGrid::Build(GLuint surfelBuffer, uint32_t maxSurfels)
{
    (void)surfelBuffer;
    (void)maxSurfels;
}

void SurfelGrid::BuildCellAverages()
{
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

    GLsizeiptr requestBytes = 0;
    GLsizeiptr rayBytes = 0;
    GLsizeiptr hitBytes = 0;
    GLsizeiptr binBytes = 0;
    GLsizeiptr counterBytes = 0;
    if (!CheckedBufferSize(maxSurfels, sizeof(SurfelRayRequest), requestBytes) ||
        !CheckedBufferSize(maxRays, sizeof(SurfelRay), rayBytes) ||
        !CheckedBufferSize(maxRays, sizeof(SurfelRayHit), hitBytes) ||
        !CheckedBufferSize(maxRays, sizeof(uint32_t), binBytes) ||
        !CheckedBufferSize(1u, sizeof(SurfelRayCounters), counterBytes)) {
        std::cerr << "[SurfelRayQueue] Invalid queue size: maxRays=" << maxRays
            << ", maxSurfels=" << maxSurfels << ".\n";
        return false;
    }

    m_maxRays = maxRays;
    m_maxSurfels = maxSurfels;

    const bool ok =
        AllocateSSBO(m_requestBuffer, requestBytes, nullptr, "SurfelRayQueue.Requests") &&
        AllocateSSBO(m_rayBuffer, rayBytes, nullptr, "SurfelRayQueue.Rays") &&
        AllocateSSBO(m_sortedRayBuffer, rayBytes, nullptr, "SurfelRayQueue.SortedRays") &&
        AllocateSSBO(m_rayHitBuffer, hitBytes, nullptr, "SurfelRayQueue.Hits") &&
        AllocateSSBO(m_rayBinBuffer, binBytes, nullptr, "SurfelRayQueue.Bins") &&
        AllocateSSBO(m_countersBuffer, counterBytes, nullptr, "SurfelRayQueue.Counters");

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
    DeleteBuffer(m_sortedRayBuffer);
    DeleteBuffer(m_rayHitBuffer);
    DeleteBuffer(m_rayBinBuffer);
    DeleteBuffer(m_countersBuffer);
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
    ClearBufferUInt(m_sortedRayBuffer, 0u);
    ClearBufferUInt(m_rayHitBuffer, 0u);
    ClearBufferUInt(m_rayBinBuffer, 0xffffffffu);

    SurfelRayCounters counters{};
    counters.maxRays = m_maxRays;
    counters.maxSurfels = m_maxSurfels;
    UploadStruct(m_countersBuffer, counters);
}

void SurfelRayQueue::RequestRays(GLuint surfelBuffer)
{
    (void)surfelBuffer;
}

void SurfelRayQueue::AllocateRays(GLuint surfelBuffer, uint32_t globalBudget)
{
    (void)surfelBuffer;

    if (!IsCreated()) {
        return;
    }

    SurfelRayCounters counters{};
    counters.maxRays = m_maxRays;
    counters.maxSurfels = m_maxSurfels;
    counters.globalBudget = globalBudget < m_maxRays ? globalBudget : m_maxRays;
    UploadStruct(m_countersBuffer, counters);
}

void SurfelRayQueue::GenerateRays(GLuint surfelBuffer, GLuint gridBuffer)
{
    (void)surfelBuffer;
    (void)gridBuffer;
}

void SurfelRayQueue::BinAndSortRays()
{
}

void SurfelRayQueue::TraceRays(const TraceResources& traceResources)
{
    (void)traceResources;
}

void SurfelRayQueue::IntegrateHits(GLuint surfelBuffer)
{
    (void)surfelBuffer;
}
