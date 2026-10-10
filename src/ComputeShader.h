#pragma once

#include "rhi/RHI.h"
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

// Forward declarations
class Texture;
using TexturePtr = std::shared_ptr<Texture>;

/**
 * @brief Compute program wrapper on rhi::Device.
 *
 * - Compilation from file (with #include resolution) or source, optional injected defines
 * - Dispatch configuration (explicit or auto-calculated work groups)
 * - Storage-buffer and image binding helpers
 * - Cached uniform locations (setters bind the program and leave it bound)
 * - Optional GPU timing per dispatch
 */
class ComputeShader {
public:
    ComputeShader();
    ~ComputeShader();

    //  Core Compute Shader Management

    // Create and compile a compute shader from a .glsl file; defines are injected after #version.
    bool CreateFromFile(const std::string& computeShaderPath,
                       const std::vector<std::string>& defines = {});

    // Create and compile a compute shader from source.
    bool CreateFromSource(const std::string& source,
                         const std::vector<std::string>& defines = {});

    bool IsValid() const;
    rhi::ProgramHandle GetProgramID() const;
    void Cleanup();

    //  Dispatch Configuration
    struct DispatchConfig {
        uint32_t numGroupsX = 1;
        uint32_t numGroupsY = 1;
        uint32_t numGroupsZ = 1;

        // Automatically calculate groups based on problem size and local work group size
        bool autoCalculateGroups = false;
        uint32_t problemSizeX = 1;
        uint32_t problemSizeY = 1;
        uint32_t problemSizeZ = 1;
        uint32_t localWorkGroupX = 8;
        uint32_t localWorkGroupY = 8;
        uint32_t localWorkGroupZ = 1;
    };

    void SetDispatchConfig(const DispatchConfig& config);
    void Dispatch();
    void Dispatch(uint32_t numGroupsX, uint32_t numGroupsY = 1, uint32_t numGroupsZ = 1);
    void WaitForCompletion(uint32_t barriers = rhi::BarrierAll);

    //  Resource Binding Utilities
    void BindBuffer(rhi::BufferHandle buffer, uint32_t bindingPoint);
    void BindTexture(rhi::TextureHandle texture, uint32_t unit, int level = 0,
                    bool layered = false, int layer = 0,
                    rhi::ImageAccess access = rhi::ImageAccess::ReadWrite, rhi::Format format = rhi::Format::RGBA8);
    // Binds the texture for sampling on `unit` and as a read-only image (its own format).
    void BindTextureRead(rhi::TextureHandle texture, uint32_t unit, int level = 0);
    void BindTextureWrite(rhi::TextureHandle texture, uint32_t unit, int level = 0, rhi::Format format = rhi::Format::RGBA8);
    void UnbindBuffer(uint32_t bindingPoint);
    void UnbindTexture(uint32_t unit);

    //  Uniform Management (binds the program)
    void SetUniform(const std::string& name, int value);
    void SetUniform(const std::string& name, float value);
    void SetUniform(const std::string& name, const glm::vec2& value);
    void SetUniform(const std::string& name, const glm::vec3& value);
    void SetUniform(const std::string& name, const glm::vec4& value);
    void SetUniform(const std::string& name, const glm::mat4& value);
    void SetUniform(const std::string& name, const std::vector<float>& values);
    void SetUniform(const std::string& name, const std::vector<glm::vec3>& values);

    //  Performance and Debugging
    void EnableTiming(bool enable);
    float GetLastExecutionTime() const;

    struct WorkGroupInfo {
        int maxWorkGroupSizeX;
        int maxWorkGroupSizeY;
        int maxWorkGroupSizeZ;
        int maxWorkGroupInvocations;
        int maxComputeWorkGroupCount[3];
        int maxSharedMemorySize;
    };
    static WorkGroupInfo GetWorkGroupLimits();
    void PrintDebugInfo() const;

    //  Static Utility Methods
    static bool IsSupported();
    static uint32_t CalculateWorkGroups(uint32_t problemSize, uint32_t localWorkGroupSize);
    static rhi::BufferHandle CreateComputeBuffer(size_t size, const void* data = nullptr,
                                    rhi::BufferUsage usage = rhi::BufferUsage::DynamicDraw);
    static TexturePtr CreateComputeTexture2D(uint32_t width, uint32_t height,
      rhi::Format format = rhi::Format::RGBA32F, const void* data = nullptr);

private:
    rhi::ProgramHandle m_programID;
    bool m_isValid;
    DispatchConfig m_dispatchConfig;

    // Performance timing
    bool m_timingEnabled;
    rhi::QueryHandle m_timeQueries[2];
    float m_lastExecutionTime;

    // Cached uniform locations
    std::unordered_map<std::string, rhi::UniformLocation> m_uniformLocations;

    // Resource tracking for cleanup
    std::vector<uint32_t> m_boundBuffers;
    std::vector<uint32_t> m_boundTextures;

    // Helper methods
    rhi::UniformLocation GetUniformLocation(const std::string& name);
    bool CompileShader(const std::string& source, const std::vector<std::string>& defines);
    void SetupTiming();
    void CleanupTiming();
    std::string InjectDefines(const std::string& source, const std::vector<std::string>& defines);

    // Static state for tracking compute shader support
    static bool s_supportChecked;
    static bool s_isSupported;
};

class ComputeShaderManager {
public:
    static ComputeShaderManager& Instance();

    bool RegisterShader(const std::string& name, const std::string& shaderPath,
                       const std::vector<std::string>& defines = {});
    std::shared_ptr<ComputeShader> GetShader(const std::string& name);
    bool HasShader(const std::string& name) const;
    void RemoveShader(const std::string& name);
    void Clear();

    struct PerformanceStats {
        int totalShaders;
        float totalExecutionTime;
        float averageExecutionTime;
        std::string slowestShader;
        float slowestTime;
    };
    PerformanceStats GetPerformanceStats() const;
    void PrintPerformanceReport() const;

private:
    ComputeShaderManager() = default;
    ~ComputeShaderManager() = default;
    ComputeShaderManager(const ComputeShaderManager&) = delete;
    ComputeShaderManager& operator=(const ComputeShaderManager&) = delete;

    std::unordered_map<std::string, std::shared_ptr<ComputeShader>> m_shaders;
    std::unordered_map<std::string, std::string> m_shaderPaths;
    std::unordered_map<std::string, std::vector<std::string>> m_shaderDefines;
};

//  Convenience Macros for Common Compute Shader Patterns
#define COMPUTE_DISPATCH_2D(shader, width, height, localX, localY) \
    shader.Dispatch(ComputeShader::CalculateWorkGroups(width, localX), \
                   ComputeShader::CalculateWorkGroups(height, localY))

#define COMPUTE_BARRIER_TEXTURE() rhi::Gpu().PipelineBarrier(rhi::BarrierImageAccess)
#define COMPUTE_BARRIER_BUFFER() rhi::Gpu().PipelineBarrier(rhi::BarrierStorage)
