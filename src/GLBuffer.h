#pragma once

#include <GL/glew.h>
#include <vector>
#include <memory>
#include <string>
#include <iostream>
#include <type_traits>

/**
 * @brief Enumeration of OpenGL buffer types supported by GLBuffer
 */
enum class BufferType : GLenum {
    Vertex          = GL_ARRAY_BUFFER,
    Index           = GL_ELEMENT_ARRAY_BUFFER,
    Uniform         = GL_UNIFORM_BUFFER,
    ShaderStorage   = GL_SHADER_STORAGE_BUFFER,
    DrawIndirect    = GL_DRAW_INDIRECT_BUFFER,
    DispatchIndirect = GL_DISPATCH_INDIRECT_BUFFER,
    CopyRead        = GL_COPY_READ_BUFFER,
    CopyWrite       = GL_COPY_WRITE_BUFFER,
    PixelPack       = GL_PIXEL_PACK_BUFFER,
    PixelUnpack     = GL_PIXEL_UNPACK_BUFFER,
    AtomicCounter   = GL_ATOMIC_COUNTER_BUFFER,
    TransformFeedback = GL_TRANSFORM_FEEDBACK_BUFFER,
    Texture         = GL_TEXTURE_BUFFER
};

/**
 * @brief Usage hint for buffer allocation
 */
enum class BufferUsage : GLenum {
    StaticDraw  = GL_STATIC_DRAW,
    StaticRead  = GL_STATIC_READ,
    StaticCopy  = GL_STATIC_COPY,
    DynamicDraw = GL_DYNAMIC_DRAW,
    DynamicRead = GL_DYNAMIC_READ,
    DynamicCopy = GL_DYNAMIC_COPY,
    StreamDraw  = GL_STREAM_DRAW,
    StreamRead  = GL_STREAM_READ,
    StreamCopy  = GL_STREAM_COPY
};

/**
 * @brief Memory access flags for persistent/coherent mapping
 */
enum class BufferMapFlags : GLbitfield {
    None        = 0,
    Read        = GL_MAP_READ_BIT,
    Write       = GL_MAP_WRITE_BIT,
    Persistent  = GL_MAP_PERSISTENT_BIT,
    Coherent    = GL_MAP_COHERENT_BIT,
    InvalidateRange  = GL_MAP_INVALIDATE_RANGE_BIT,
    InvalidateBuffer = GL_MAP_INVALIDATE_BUFFER_BIT,
    FlushExplicit    = GL_MAP_FLUSH_EXPLICIT_BIT,
    Unsynchronized   = GL_MAP_UNSYNCHRONIZED_BIT
};

inline BufferMapFlags operator|(BufferMapFlags a, BufferMapFlags b) {
    return static_cast<BufferMapFlags>(static_cast<GLbitfield>(a) | static_cast<GLbitfield>(b));
}

inline BufferMapFlags operator&(BufferMapFlags a, BufferMapFlags b) {
    return static_cast<BufferMapFlags>(static_cast<GLbitfield>(a) & static_cast<GLbitfield>(b));
}

/**
 * @brief Storage flags for immutable buffer storage (glBufferStorage)
 */
enum class BufferStorageFlags : GLbitfield {
    None            = 0,
    DynamicStorage  = GL_DYNAMIC_STORAGE_BIT,
    MapRead         = GL_MAP_READ_BIT,
    MapWrite        = GL_MAP_WRITE_BIT,
    MapPersistent   = GL_MAP_PERSISTENT_BIT,
    MapCoherent     = GL_MAP_COHERENT_BIT,
    ClientStorage   = GL_CLIENT_STORAGE_BIT
};

inline BufferStorageFlags operator|(BufferStorageFlags a, BufferStorageFlags b) {
    return static_cast<BufferStorageFlags>(static_cast<GLbitfield>(a) | static_cast<GLbitfield>(b));
}

/**
 * @brief GLBuffer - A high-performance OpenGL buffer object wrapper
 * 
 * This class provides a type-safe, RAII-compliant abstraction over OpenGL buffer objects.
 * It supports all buffer types (VBO, IBO, UBO, SSBO, etc.) and provides clean APIs for:
 * - Buffer creation with mutable or immutable storage
 * - Data upload (full or partial)
 * - Buffer mapping (standard and persistent)
 * - Indexed binding for UBOs and SSBOs
 * - Debug labeling (via GL_KHR_debug)
 * 
 * Design follows the established FrameBuffer pattern in this codebase.
 * 
 * Usage examples:
 * @code
 *   // Create a vertex buffer
 *   GLBuffer vbo(BufferType::Vertex, BufferUsage::StaticDraw);
 *   vbo.SetData(vertices);
 *   
 *   // Create an SSBO with immutable storage
 *   GLBuffer ssbo(BufferType::ShaderStorage);
 *   ssbo.AllocateImmutable(dataSize, BufferStorageFlags::DynamicStorage);
 *   ssbo.BindBase(0);
 *   
 *   // Map buffer for writing
 *   auto* ptr = ssbo.Map<MyStruct>(BufferMapFlags::Write);
 *   // ... write data ...
 *   ssbo.Unmap();
 * @endcode
 */
class GLBuffer {
public:
    /**
     * @brief Construct a buffer without allocation
     * @param type Buffer target type
     * @param usage Usage hint for mutable storage (ignored for immutable)
     */
    explicit GLBuffer(BufferType type = BufferType::Vertex, 
                      BufferUsage usage = BufferUsage::StaticDraw);
    
    /**
     * @brief Construct and allocate mutable storage
     * @param type Buffer target type
     * @param sizeBytes Size in bytes to allocate
     * @param data Initial data (nullptr for uninitialized)
     * @param usage Usage hint
     */
    GLBuffer(BufferType type, size_t sizeBytes, const void* data, 
             BufferUsage usage = BufferUsage::StaticDraw);
    
    ~GLBuffer();
    
    // Move-only semantics (follows FrameBuffer pattern)
    GLBuffer(const GLBuffer&) = delete;
    GLBuffer& operator=(const GLBuffer&) = delete;
    GLBuffer(GLBuffer&& other) noexcept;
    GLBuffer& operator=(GLBuffer&& other) noexcept;
    
    // ============================================================
    // Buffer Creation & Allocation
    // ============================================================
    
    /**
     * @brief Allocate mutable buffer storage (glBufferData)
     * @param sizeBytes Size in bytes
     * @param data Initial data pointer (nullptr for uninitialized)
     * @return true if successful
     */
    bool Allocate(size_t sizeBytes, const void* data = nullptr);
    
    /**
     * @brief Allocate immutable buffer storage (glBufferStorage)
     * @param sizeBytes Size in bytes
     * @param flags Storage flags
     * @param data Initial data pointer (nullptr for uninitialized)
     * @return true if successful
     */
    bool AllocateImmutable(size_t sizeBytes, BufferStorageFlags flags, 
                           const void* data = nullptr);
    
    /**
     * @brief Reallocate buffer (orphaning for mutable, error for immutable)
     * @param sizeBytes New size in bytes
     * @param data New data pointer
     * @return true if successful
     */
    bool Reallocate(size_t sizeBytes, const void* data = nullptr);
    
    // ============================================================
    // Template helpers for typed data
    // ============================================================
    
    /**
     * @brief Set buffer data from a vector
     * @tparam T Element type
     * @param data Vector of elements
     * @return true if allocation succeeded
     */
    template<typename T>
    bool SetData(const std::vector<T>& data) {
        return Allocate(data.size() * sizeof(T), data.data());
    }
    
    /**
     * @brief Set buffer data from raw array
     * @tparam T Element type
     * @param data Pointer to data
     * @param count Number of elements
     * @return true if allocation succeeded
     */
    template<typename T>
    bool SetData(const T* data, size_t count) {
        return Allocate(count * sizeof(T), data);
    }
    
    /**
     * @brief Update a subregion of buffer data (glBufferSubData)
     * @tparam T Element type
     * @param data Vector of elements
     * @param offsetBytes Byte offset into buffer
     */
    template<typename T>
    void UpdateSubData(const std::vector<T>& data, size_t offsetBytes = 0) {
        SubData(offsetBytes, data.size() * sizeof(T), data.data());
    }
    
    /**
     * @brief Update a subregion of buffer data
     * @param offsetBytes Byte offset into buffer
     * @param sizeBytes Size of data to update
     * @param data Pointer to source data
     */
    void SubData(size_t offsetBytes, size_t sizeBytes, const void* data);
    
    // ============================================================
    // Buffer Binding
    // ============================================================
    
    /**
     * @brief Bind buffer to its target
     */
    void Bind() const;
    
    /**
     * @brief Unbind buffer from its target
     */
    void Unbind() const;
    
    /**
     * @brief Bind buffer to an indexed binding point (UBO/SSBO/AtomicCounter)
     * @param index Binding point index
     */
    void BindBase(GLuint index) const;
    
    /**
     * @brief Bind a range of buffer to an indexed binding point
     * @param index Binding point index
     * @param offsetBytes Offset into buffer
     * @param sizeBytes Size of range to bind
     */
    void BindRange(GLuint index, size_t offsetBytes, size_t sizeBytes) const;
    
    /**
     * @brief Static unbind for a specific buffer type
     * @param type Buffer target type
     */
    static void Unbind(BufferType type);
    
    // ============================================================
    // Buffer Mapping
    // ============================================================
    
    /**
     * @brief Map entire buffer for CPU access
     * @tparam T Pointer type to return
     * @param flags Map access flags
     * @return Typed pointer to mapped memory, nullptr on failure
     */
    template<typename T = void>
    T* Map(BufferMapFlags flags = BufferMapFlags::Write) {
        return static_cast<T*>(MapRaw(flags));
    }
    
    /**
     * @brief Map a range of buffer for CPU access
     * @tparam T Pointer type to return
     * @param offsetBytes Byte offset
     * @param sizeBytes Size to map
     * @param flags Map access flags
     * @return Typed pointer to mapped memory, nullptr on failure
     */
    template<typename T = void>
    T* MapRange(size_t offsetBytes, size_t sizeBytes, BufferMapFlags flags) {
        return static_cast<T*>(MapRangeRaw(offsetBytes, sizeBytes, flags));
    }
    
    /**
     * @brief Unmap the buffer
     * @return true if mapping was valid
     */
    bool Unmap();
    
    /**
     * @brief Flush a range of a mapped buffer
     * @param offsetBytes Byte offset from start of mapped region
     * @param sizeBytes Size to flush
     */
    void FlushMappedRange(size_t offsetBytes, size_t sizeBytes);
    
    /**
     * @brief Check if buffer is currently mapped
     */
    bool IsMapped() const { return m_isMapped; }
    
    // ============================================================
    // Buffer Copy
    // ============================================================
    
    /**
     * @brief Copy data from another buffer (glCopyBufferSubData)
     * @param source Source buffer
     * @param readOffset Offset in source buffer
     * @param writeOffset Offset in this buffer
     * @param sizeBytes Number of bytes to copy
     */
    void CopyFrom(const GLBuffer& source, size_t readOffset, 
                  size_t writeOffset, size_t sizeBytes);
    
    // ============================================================
    // Accessors
    // ============================================================
    
    GLuint GetID() const { return m_bufferID; }
    BufferType GetType() const { return m_type; }
    BufferUsage GetUsage() const { return m_usage; }
    size_t GetSize() const { return m_size; }
    bool IsValid() const { return m_bufferID != 0; }
    bool IsImmutable() const { return m_isImmutable; }
    
    /**
     * @brief Set debug label for this buffer (GL_KHR_debug)
     * @param label Debug label string
     */
    void SetLabel(const std::string& label);
    
    /**
     * @brief Get OpenGL error string for last operation
     */
    static std::string GetLastError();
    
private:
    void* MapRaw(BufferMapFlags flags);
    void* MapRangeRaw(size_t offsetBytes, size_t sizeBytes, BufferMapFlags flags);
    void Release();
    bool CheckError(const char* operation) const;
    
    GLuint m_bufferID = 0;
    BufferType m_type;
    BufferUsage m_usage;
    size_t m_size = 0;
    bool m_isImmutable = false;
    bool m_isMapped = false;
    
#ifdef _DEBUG
    std::string m_debugLabel;
#endif
};

// Type aliases for common buffer types
using VertexBuffer = GLBuffer;
using IndexBuffer = GLBuffer;
using UniformBuffer = GLBuffer;
using ShaderStorageBuffer = GLBuffer;
using IndirectBuffer = GLBuffer;

/**
 * @brief Smart pointer type for GLBuffer
 */
using GLBufferPtr = std::unique_ptr<GLBuffer>;

/**
 * @brief Factory function to create a buffer
 */
template<typename... Args>
inline GLBufferPtr MakeBuffer(Args&&... args) {
    return std::make_unique<GLBuffer>(std::forward<Args>(args)...);
}
