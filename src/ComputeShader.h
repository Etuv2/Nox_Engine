#pragma once

#include <GL/glew.h>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

// Forward declarations
class Texture;
using TexturePtr = std::shared_ptr<Texture>;

/**
 * @brief Comprehensive Compute Shader system for the NOX Engine
 * 
 * This class provides a complete compute shader implementation with:
 * - Easy shader compilation and management
 * - Flexible dispatch configuration
 * - Buffer and texture binding utilities  
 * - Performance monitoring and debugging
 * - Integration with existing engine systems
 */
class ComputeShader {
public:
    ComputeShader();
    ~ComputeShader();

    // === Core Compute Shader Management ===
    
    /**
     * @brief Create and compile a compute shader from file
     * @param computeShaderPath Path to the .glsl compute shader file
     * @param defines Optional preprocessor defines to inject
     * @return true if compilation successful
     */
    bool CreateFromFile(const std::string& computeShaderPath, 
                       const std::vector<std::string>& defines = {});

    /**
     * @brief Create and compile a compute shader from source string
     * @param source GLSL compute shader source code
     * @param defines Optional preprocessor defines to inject  
     * @return true if compilation successful
     */
    bool CreateFromSource(const std::string& source,
                         const std::vector<std::string>& defines = {});

    /**
     * @brief Check if the compute shader is valid and ready to use
     */
    bool IsValid() const;

    /**
     * @brief Get the OpenGL program ID
     */
    GLuint GetProgramID() const;

    /**
     * @brief Clean up and delete the compute shader
     */
    void Cleanup();

    // === Dispatch Configuration ===

    struct DispatchConfig {
        GLuint numGroupsX = 1;
        GLuint numGroupsY = 1; 
        GLuint numGroupsZ = 1;
        
        // Automatically calculate groups based on problem size and local work group size
        bool autoCalculateGroups = false;
        GLuint problemSizeX = 1;
        GLuint problemSizeY = 1;
        GLuint problemSizeZ = 1;
        GLuint localWorkGroupX = 8;
        GLuint localWorkGroupY = 8;
        GLuint localWorkGroupZ = 1;
    };

    /**
     * @brief Configure dispatch parameters for the compute shader
     */
    void SetDispatchConfig(const DispatchConfig& config);

    /**
     * @brief Dispatch the compute shader with current configuration
     */
    void Dispatch();

    /**
     * @brief Dispatch with explicit group counts (convenience method)
     */
    void Dispatch(GLuint numGroupsX, GLuint numGroupsY = 1, GLuint numGroupsZ = 1);

    /**
     * @brief Wait for compute shader completion (memory barrier)
     * @param barriers Specific memory barriers to wait for (default: all)
     */
    void WaitForCompletion(GLbitfield barriers = GL_ALL_BARRIER_BITS);

    // === Resource Binding Utilities ===

    /**
     * @brief Bind a buffer to a specific binding point
     * @param buffer OpenGL buffer ID
     * @param bindingPoint Binding point index
     * @param access Buffer access mode (GL_READ_ONLY, GL_WRITE_ONLY, GL_READ_WRITE)
     */
    void BindBuffer(GLuint buffer, GLuint bindingPoint, GLenum access = GL_READ_WRITE);

    /**
     * @brief Bind a texture to a specific image unit for compute shader access
     * @param texture OpenGL texture ID
     * @param unit Image unit index
     * @param level Mipmap level
     * @param layered Whether to bind as layered texture
     * @param layer Specific layer for non-layered binding
     * @param access Texture access mode
     * @param format Internal format for the texture
     */
    void BindTexture(GLuint texture, GLuint unit, GLint level = 0, 
                    GLboolean layered = GL_FALSE, GLint layer = 0,
                    GLenum access = GL_READ_WRITE, GLenum format = GL_RGBA8);

    /**
     * @brief Bind texture for read-only access (convenience method)
     */
    void BindTextureRead(GLuint texture, GLuint unit, GLint level = 0);

    /**
     * @brief Bind texture for write-only access (convenience method)  
     */
    void BindTextureWrite(GLuint texture, GLuint unit, GLint level = 0, GLenum format = GL_RGBA8);

    /**
     * @brief Unbind a buffer from a binding point
     */
    void UnbindBuffer(GLuint bindingPoint);

    /**
     * @brief Unbind a texture from an image unit
     */
    void UnbindTexture(GLuint unit);

    // === Uniform Management ===

    /**
     * @brief Set uniform values (similar to regular shaders)
     */
    void SetUniform(const std::string& name, int value);
    void SetUniform(const std::string& name, float value);
    void SetUniform(const std::string& name, const glm::vec2& value);
    void SetUniform(const std::string& name, const glm::vec3& value);
    void SetUniform(const std::string& name, const glm::vec4& value);
    void SetUniform(const std::string& name, const glm::mat4& value);
    void SetUniform(const std::string& name, const std::vector<float>& values);
    void SetUniform(const std::string& name, const std::vector<glm::vec3>& values);

    // === Performance and Debugging ===

    /**
     * @brief Enable/disable GPU timing for this compute shader
     */
    void EnableTiming(bool enable);

    /**
     * @brief Get the last recorded execution time in milliseconds
     */
    float GetLastExecutionTime() const;

    /**
     * @brief Get information about the compute shader's work group limits
     */
    struct WorkGroupInfo {
        GLint maxWorkGroupSizeX;
        GLint maxWorkGroupSizeY;
        GLint maxWorkGroupSizeZ;
        GLint maxWorkGroupInvocations;
        GLint maxComputeWorkGroupCount[3];
        GLint maxSharedMemorySize;
    };
    
    static WorkGroupInfo GetWorkGroupLimits();

    /**
     * @brief Print debug information about the compute shader
     */
    void PrintDebugInfo() const;

    // === Static Utility Methods ===

    /**
     * @brief Check if compute shaders are supported on this hardware
     */
    static bool IsSupported();

    /**
     * @brief Calculate optimal work group count for a given problem size
     */
    static GLuint CalculateWorkGroups(GLuint problemSize, GLuint localWorkGroupSize);

    /**
     * @brief Create a buffer suitable for compute shader usage
     * @param size Buffer size in bytes
     * @param data Optional initial data
     * @param usage Buffer usage hint (default: GL_DYNAMIC_DRAW)
     */
    static GLuint CreateComputeBuffer(GLsizeiptr size, const void* data = nullptr, 
                                    GLenum usage = GL_DYNAMIC_DRAW);

    /**
     * @brief Create a texture suitable for compute shader usage (REFACTORED)
     * @param width Texture width
     * @param height Texture height  
     * @param format Internal format (e.g., GL_RGBA32F, GL_R32F)
     * @param data Optional initial data
     * @return Shared pointer to Texture object (use tex->ID() for GLuint)
     */
    static TexturePtr CreateComputeTexture2D(GLuint width, GLuint height, 
      GLenum format = GL_RGBA32F, const void* data = nullptr);

private:
    GLuint m_programID;
    bool m_isValid;
    DispatchConfig m_dispatchConfig;
    
    // Performance timing
    bool m_timingEnabled;
    GLuint m_timeQueries[2];
    float m_lastExecutionTime;
    
    // Cached uniform locations
    std::unordered_map<std::string, GLint> m_uniformLocations;
    
    // Resource tracking for cleanup
    std::vector<GLuint> m_boundBuffers;
    std::vector<GLuint> m_boundTextures;
    
    // Helper methods
    GLint GetUniformLocation(const std::string& name);
    bool CompileShader(const std::string& source, const std::vector<std::string>& defines);
    void SetupTiming();
    void CleanupTiming();
    std::string InjectDefines(const std::string& source, const std::vector<std::string>& defines);
    
    // Static state for tracking compute shader support
    static bool s_supportChecked;
    static bool s_isSupported;
};

/**
 * @brief Compute Shader Manager for managing multiple compute shaders
 * 
 * This class provides centralized management of compute shaders with:
 * - Named shader registration and retrieval
 * - Automatic recompilation on file changes (in debug builds)
 * - Global performance monitoring
 * - Memory management and cleanup
 */
class ComputeShaderManager {
public:
    static ComputeShaderManager& Instance();

    /**
     * @brief Register a compute shader with a name
     */
    bool RegisterShader(const std::string& name, const std::string& shaderPath,
                       const std::vector<std::string>& defines = {});

    /**
     * @brief Get a registered compute shader by name
     */
    std::shared_ptr<ComputeShader> GetShader(const std::string& name);

    /**
     * @brief Check if a shader with the given name exists
     */
    bool HasShader(const std::string& name) const;

    /**
     * @brief Remove a shader from the manager
     */
    void RemoveShader(const std::string& name);

    /**
     * @brief Clear all registered shaders
     */
    void Clear();

    /**
     * @brief Get performance statistics for all registered shaders
     */
    struct PerformanceStats {
        int totalShaders;
        float totalExecutionTime;
        float averageExecutionTime;
        std::string slowestShader;
        float slowestTime;
    };
    
    PerformanceStats GetPerformanceStats() const;

    /**
     * @brief Print performance report for all shaders
     */
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

// === Convenience Macros for Common Compute Shader Patterns ===

#define COMPUTE_DISPATCH_2D(shader, width, height, localX, localY) \
    shader.Dispatch(ComputeShader::CalculateWorkGroups(width, localX), \
                   ComputeShader::CalculateWorkGroups(height, localY))

#define COMPUTE_BARRIER_TEXTURE() glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT)
#define COMPUTE_BARRIER_BUFFER() glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)
#define COMPUTE_BARRIER_ALL() glMemoryBarrier(GL_ALL_BARRIER_BITS)