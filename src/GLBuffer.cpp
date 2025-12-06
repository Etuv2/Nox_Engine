#include "GLBuffer.h"
#include <cstring>

GLBuffer::GLBuffer(BufferType type, BufferUsage usage)
    : m_type(type)
    , m_usage(usage)
{
    glGenBuffers(1, &m_bufferID);
    if (m_bufferID == 0) {
        std::cerr << "[GLBuffer] Failed to generate buffer object\n";
    }
}

GLBuffer::GLBuffer(BufferType type, size_t sizeBytes, const void* data, BufferUsage usage)
    : m_type(type)
    , m_usage(usage)
{
    glGenBuffers(1, &m_bufferID);
    if (m_bufferID == 0) {
        std::cerr << "[GLBuffer] Failed to generate buffer object\n";
        return;
    }
    Allocate(sizeBytes, data);
}

GLBuffer::~GLBuffer() {
    Release();
}

GLBuffer::GLBuffer(GLBuffer&& other) noexcept
    : m_bufferID(other.m_bufferID)
    , m_type(other.m_type)
    , m_usage(other.m_usage)
    , m_size(other.m_size)
    , m_isImmutable(other.m_isImmutable)
    , m_isMapped(other.m_isMapped)
{
#ifdef _DEBUG
    m_debugLabel = std::move(other.m_debugLabel);
#endif
    other.m_bufferID = 0;
    other.m_size = 0;
    other.m_isMapped = false;
}

GLBuffer& GLBuffer::operator=(GLBuffer&& other) noexcept {
    if (this != &other) {
        Release();
        
        m_bufferID = other.m_bufferID;
        m_type = other.m_type;
        m_usage = other.m_usage;
        m_size = other.m_size;
        m_isImmutable = other.m_isImmutable;
        m_isMapped = other.m_isMapped;
        
#ifdef _DEBUG
        m_debugLabel = std::move(other.m_debugLabel);
#endif
        
        other.m_bufferID = 0;
        other.m_size = 0;
        other.m_isMapped = false;
    }
    return *this;
}

void GLBuffer::Release() {
    if (m_bufferID != 0) {
        if (m_isMapped) {
            Bind();
            glUnmapBuffer(static_cast<GLenum>(m_type));
            m_isMapped = false;
        }
        glDeleteBuffers(1, &m_bufferID);
        m_bufferID = 0;
    }
    m_size = 0;
}

bool GLBuffer::Allocate(size_t sizeBytes, const void* data) {
    if (m_bufferID == 0) {
        std::cerr << "[GLBuffer] Cannot allocate: invalid buffer ID\n";
        return false;
    }
    
    if (m_isImmutable) {
        std::cerr << "[GLBuffer] Cannot reallocate immutable buffer (use Reallocate or create new)\n";
        return false;
    }
    
    // Handle zero-size allocation gracefully - just reset size and return
    if (sizeBytes == 0) {
        m_size = 0;
        return true;
    }
    
    // Clear any pending GL errors before our operation
    while (glGetError() != GL_NO_ERROR) {}
    
    Bind();
    glBufferData(static_cast<GLenum>(m_type), sizeBytes, data, 
                 static_cast<GLenum>(m_usage));
    
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[GLBuffer] Error in Allocate (size=" << sizeBytes 
                  << ", type=" << static_cast<int>(m_type) 
                  << ", bufferID=" << m_bufferID << "): ";
        switch (err) {
            case GL_INVALID_ENUM: std::cerr << "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: 
                std::cerr << "GL_INVALID_VALUE";
                // This often means the buffer was created with immutable storage
                // Mark it as immutable to prevent future allocation attempts
                m_isImmutable = true;
                break;
            case GL_INVALID_OPERATION: std::cerr << "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: std::cerr << "GL_OUT_OF_MEMORY"; break;
            default: std::cerr << "0x" << std::hex << err; break;
        }
        std::cerr << std::endl;
        return false;
    }
    
    m_size = sizeBytes;
    return true;
}

bool GLBuffer::AllocateImmutable(size_t sizeBytes, BufferStorageFlags flags, 
                                  const void* data) {
    if (m_bufferID == 0) {
        std::cerr << "[GLBuffer] Cannot allocate: invalid buffer ID\n";
        return false;
    }
    
    if (m_isImmutable) {
        std::cerr << "[GLBuffer] Buffer already has immutable storage\n";
        return false;
    }
    
    // Handle zero-size allocation gracefully
    if (sizeBytes == 0) {
        m_size = 0;
        return true;
    }
    
    Bind();
    glBufferStorage(static_cast<GLenum>(m_type), sizeBytes, data, 
                    static_cast<GLbitfield>(flags));
    
    if (!CheckError("AllocateImmutable")) {
        return false;
    }
    
    m_size = sizeBytes;
    m_isImmutable = true;
    return true;
}

bool GLBuffer::Reallocate(size_t sizeBytes, const void* data) {
    if (m_isImmutable) {
        std::cerr << "[GLBuffer] Cannot reallocate immutable buffer - create new buffer\n";
        return false;
    }
    
    // Orphan the old buffer and allocate new storage
    return Allocate(sizeBytes, data);
}

void GLBuffer::SubData(size_t offsetBytes, size_t sizeBytes, const void* data) {
    if (m_bufferID == 0 || data == nullptr) return;
    
    if (offsetBytes + sizeBytes > m_size) {
        std::cerr << "[GLBuffer] SubData: range exceeds buffer size\n";
        return;
    }
    
    Bind();
    glBufferSubData(static_cast<GLenum>(m_type), offsetBytes, sizeBytes, data);
    CheckError("SubData");
}

void GLBuffer::Bind() const {
    if (m_bufferID != 0) {
        glBindBuffer(static_cast<GLenum>(m_type), m_bufferID);
    }
}

void GLBuffer::Unbind() const {
    glBindBuffer(static_cast<GLenum>(m_type), 0);
}

void GLBuffer::BindBase(GLuint index) const {
    if (m_bufferID == 0) return;
    
    GLenum target = static_cast<GLenum>(m_type);
    
    // Only certain buffer types support indexed binding
    switch (m_type) {
        case BufferType::Uniform:
        case BufferType::ShaderStorage:
        case BufferType::AtomicCounter:
        case BufferType::TransformFeedback:
            glBindBufferBase(target, index, m_bufferID);
            break;
        default:
            std::cerr << "[GLBuffer] BindBase not supported for this buffer type\n";
            break;
    }
}

void GLBuffer::BindRange(GLuint index, size_t offsetBytes, size_t sizeBytes) const {
    if (m_bufferID == 0) return;
    
    GLenum target = static_cast<GLenum>(m_type);
    
    switch (m_type) {
        case BufferType::Uniform:
        case BufferType::ShaderStorage:
        case BufferType::AtomicCounter:
        case BufferType::TransformFeedback:
            glBindBufferRange(target, index, m_bufferID, offsetBytes, sizeBytes);
            break;
        default:
            std::cerr << "[GLBuffer] BindRange not supported for this buffer type\n";
            break;
    }
}

void GLBuffer::Unbind(BufferType type) {
    glBindBuffer(static_cast<GLenum>(type), 0);
}

void* GLBuffer::MapRaw(BufferMapFlags flags) {
    if (m_bufferID == 0) return nullptr;
    
    if (m_isMapped) {
        std::cerr << "[GLBuffer] Buffer already mapped\n";
        return nullptr;
    }
    
    Bind();
    
    // For simple read/write, use glMapBuffer
    GLenum access = GL_READ_WRITE;
    if ((static_cast<GLbitfield>(flags) & GL_MAP_READ_BIT) && 
        !(static_cast<GLbitfield>(flags) & GL_MAP_WRITE_BIT)) {
        access = GL_READ_ONLY;
    } else if (!(static_cast<GLbitfield>(flags) & GL_MAP_READ_BIT) && 
               (static_cast<GLbitfield>(flags) & GL_MAP_WRITE_BIT)) {
        access = GL_WRITE_ONLY;
    }
    
    void* ptr = glMapBuffer(static_cast<GLenum>(m_type), access);
    
    if (ptr) {
        m_isMapped = true;
    } else {
        CheckError("Map");
    }
    
    return ptr;
}

void* GLBuffer::MapRangeRaw(size_t offsetBytes, size_t sizeBytes, BufferMapFlags flags) {
    if (m_bufferID == 0) return nullptr;
    
    if (m_isMapped) {
        std::cerr << "[GLBuffer] Buffer already mapped\n";
        return nullptr;
    }
    
    if (offsetBytes + sizeBytes > m_size) {
        std::cerr << "[GLBuffer] MapRange: range exceeds buffer size\n";
        return nullptr;
    }
    
    Bind();
    void* ptr = glMapBufferRange(static_cast<GLenum>(m_type), offsetBytes, 
                                  sizeBytes, static_cast<GLbitfield>(flags));
    
    if (ptr) {
        m_isMapped = true;
    } else {
        CheckError("MapRange");
    }
    
    return ptr;
}

bool GLBuffer::Unmap() {
    if (!m_isMapped) {
        return false;
    }
    
    Bind();
    GLboolean result = glUnmapBuffer(static_cast<GLenum>(m_type));
    m_isMapped = false;
    
    if (result == GL_FALSE) {
        std::cerr << "[GLBuffer] Unmap failed - data may be corrupted\n";
        return false;
    }
    
    return true;
}

void GLBuffer::FlushMappedRange(size_t offsetBytes, size_t sizeBytes) {
    if (!m_isMapped) {
        std::cerr << "[GLBuffer] Cannot flush: buffer not mapped\n";
        return;
    }
    
    Bind();
    glFlushMappedBufferRange(static_cast<GLenum>(m_type), offsetBytes, sizeBytes);
}

void GLBuffer::CopyFrom(const GLBuffer& source, size_t readOffset, 
                         size_t writeOffset, size_t sizeBytes) {
    if (m_bufferID == 0 || source.m_bufferID == 0) return;
    
    if (readOffset + sizeBytes > source.m_size) {
        std::cerr << "[GLBuffer] CopyFrom: source range exceeds buffer size\n";
        return;
    }
    
    if (writeOffset + sizeBytes > m_size) {
        std::cerr << "[GLBuffer] CopyFrom: destination range exceeds buffer size\n";
        return;
    }
    
    // Use GL_COPY_READ_BUFFER and GL_COPY_WRITE_BUFFER to avoid conflicts
    glBindBuffer(GL_COPY_READ_BUFFER, source.m_bufferID);
    glBindBuffer(GL_COPY_WRITE_BUFFER, m_bufferID);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 
                        readOffset, writeOffset, sizeBytes);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

void GLBuffer::SetLabel(const std::string& label) {
#ifdef _DEBUG
    m_debugLabel = label;
#endif
    
    if (m_bufferID != 0 && GLEW_KHR_debug) {
        glObjectLabel(GL_BUFFER, m_bufferID, static_cast<GLsizei>(label.length()), 
                      label.c_str());
    }
}

bool GLBuffer::CheckError(const char* operation) const {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[GLBuffer] Error in " << operation << ": ";
        switch (err) {
            case GL_INVALID_ENUM:
                std::cerr << "GL_INVALID_ENUM";
                break;
            case GL_INVALID_VALUE:
                std::cerr << "GL_INVALID_VALUE";
                break;
            case GL_INVALID_OPERATION:
                std::cerr << "GL_INVALID_OPERATION";
                break;
            case GL_OUT_OF_MEMORY:
                std::cerr << "GL_OUT_OF_MEMORY";
                break;
            default:
                std::cerr << "0x" << std::hex << err;
                break;
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

std::string GLBuffer::GetLastError() {
    GLenum err = glGetError();
    switch (err) {
        case GL_NO_ERROR:
            return "No error";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default:
            return "Unknown error";
    }
}
