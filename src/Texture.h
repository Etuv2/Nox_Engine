#pragma once

#include <string>
#include <GL/glew.h>
#include "stb_image.h"
#include <array>
#include <memory>
#include <functional>
#include <glm/glm.hpp>

// Types of texture for identification (extended for more texture uses)
enum class TextureType {
    Diffuse,
    Normal,
    MetallicRoughness,
    Specular,
    Emissive,
    Occlusion,
    Depth,
  Shadow,
    HDR,
    Cubemap,
    Custom
};

// Texture target types for different dimensions and use cases
enum class TextureTarget {
    Texture1D = GL_TEXTURE_1D,
    Texture2D = GL_TEXTURE_2D,
    Texture3D = GL_TEXTURE_3D,
    Texture1DArray = GL_TEXTURE_1D_ARRAY,
    Texture2DArray = GL_TEXTURE_2D_ARRAY,
    TextureCubeMap = GL_TEXTURE_CUBE_MAP,
    TextureCubeMapArray = GL_TEXTURE_CUBE_MAP_ARRAY,
    Texture2DMultisample = GL_TEXTURE_2D_MULTISAMPLE,
    Texture2DMultisampleArray = GL_TEXTURE_2D_MULTISAMPLE_ARRAY,
    TextureRectangle = GL_TEXTURE_RECTANGLE
};

/**
 * @brief Comprehensive OpenGL texture wrapper supporting all texture types
 * 
 * This class provides a unified interface for creating and managing OpenGL textures
 * of any dimension and format, eliminating code duplication across the codebase.
 */
class Texture {
public:
    // === Builder Pattern for Flexible Texture Creation ===
    class Builder;

    // === Constructors ===
    Texture();
    
    // Load from file (2D texture)
    Texture(const std::string& filepath,
        TextureType type = TextureType::Custom,
        bool generateMipmaps = true,
        bool srgb = false);

    // Create empty texture with specific dimensions and format
    Texture(int width,
     int height,
        GLenum internalFormat,
        GLenum format,
        GLenum dataType,
      const void* data = nullptr,
 bool generateMipmaps = true);

    ~Texture();

    // Disable copy (expensive operation), allow move
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // === Binding and Usage ===
    void Bind(GLenum unit) const;
    void Unbind() const;

    // === Parameter Configuration ===
    void SetParameters(GLint wrapS, GLint wrapT,
        GLint minFilter, GLint magFilter);
    void SetWrapMode(GLint wrapS, GLint wrapT, GLint wrapR = GL_REPEAT);
    void SetFilterMode(GLint minFilter, GLint magFilter);
    void SetBorderColor(const glm::vec4& color);
    void SetCompareMode(GLenum mode, GLenum func);
    
    // === Mipmap Management ===
    void GenerateMipmaps();
void SetMipmapRange(GLint baseLevel, GLint maxLevel);

    // === Data Upload ===
    void Upload2D(GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void* data);
    void Upload3D(GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
         GLsizei width, GLsizei height, GLsizei depth,
      GLenum format, GLenum type, const void* data);

    // === Getters ===
    GLuint ID() const { return id_; }
    TextureType Type() const { return type_; }
    TextureTarget Target() const { return target_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int Depth() const { return depth_; }
    GLenum InternalFormat() const { return internalFormat_; }
    GLenum Format() const { return format_; }
    GLenum DataType() const { return dataType_; }
    bool HasMipmaps() const { return hasMipmaps_; }

    // === Utility ===
    bool IsValid() const { return glIsTexture(id_) == GL_TRUE; }
 void Resize(int width, int height, int depth = 1);
    void Clear(const glm::vec4& color = glm::vec4(0.0f));

private:
    GLuint id_;
    TextureTarget target_;
    int width_;
    int height_;
    int depth_;
    TextureType type_;
    GLenum internalFormat_;
    GLenum format_;
    GLenum dataType_;
    bool hasMipmaps_;

    // Helper to set default parameters based on type
    void SetParametersForType(bool generateMipmaps);

    // Helper to bind to correct target
    void BindToTarget() const;
};

/**
 * @brief Builder class for flexible texture creation
 * 
 * Example usage:
 * @code
 * auto tex = Texture::Builder()
 *     .Target(TextureTarget::Texture2D)
 *  .Dimensions(1024, 1024)
 *     .InternalFormat(GL_RGBA16F)
 *     .Format(GL_RGBA)
 *     .DataType(GL_FLOAT)
 *     .FilterMode(GL_LINEAR, GL_LINEAR)
 *     .WrapMode(GL_CLAMP_TO_EDGE)
 *     .GenerateMipmaps(false)
 * .Build();
 * @endcode
 */
class Texture::Builder {
public:
    Builder();

    Builder& Target(TextureTarget target);
    Builder& Dimensions(int width, int height = 1, int depth = 1);
    Builder& InternalFormat(GLenum format);
    Builder& Format(GLenum format);
  Builder& DataType(GLenum type);
    Builder& Data(const void* data);
    Builder& FilterMode(GLint min, GLint mag);
    Builder& WrapMode(GLint s, GLint t = -1, GLint r = -1);
    Builder& BorderColor(const glm::vec4& color);
  Builder& CompareMode(GLenum mode, GLenum func);
    Builder& GenerateMipmaps(bool generate);
    Builder& TextureType(enum TextureType type);
    Builder& Samples(int samples); // For multisample textures
    Builder& FixedSampleLocations(bool fixed); // For multisample textures
    
    // Specialized builders for common texture types
    static Builder Texture2D(int width, int height, GLenum internalFormat);
    static Builder Texture3D(int width, int height, int depth, GLenum internalFormat);
    static Builder TextureArray2D(int width, int height, int layers, GLenum internalFormat);
    static Builder TextureCube(int size, GLenum internalFormat);
    static Builder DepthTexture(int width, int height, bool shadow = false);
    static Builder HDRTexture(int width, int height);
    static Builder MultisampleTexture(int width, int height, int samples, GLenum internalFormat);
    
    std::shared_ptr<Texture> Build();

private:
    TextureTarget target_;
    int width_, height_, depth_;
    GLenum internalFormat_;
    GLenum format_;
  GLenum dataType_;
    const void* data_;
    GLint minFilter_, magFilter_;
    GLint wrapS_, wrapT_, wrapR_;
    glm::vec4 borderColor_;
GLenum compareMode_, compareFunc_;
    bool generateMipmaps_;
    enum TextureType type_;
    int samples_;
    bool fixedSampleLocations_;
    bool hasBorderColor_;
    bool hasCompareMode_;

    // Determine appropriate format from internal format
    void DeriveFormats();
};

using TexturePtr = std::shared_ptr<Texture>;

// === Factory Functions for Common Texture Types ===
namespace TextureFactory {
    // Create a standard 2D texture from file
    TexturePtr FromFile(const std::string& path, bool srgb = false, bool mipmaps = true);
    
    // Create empty 2D texture
    TexturePtr Create2D(int width, int height, GLenum internalFormat, 
        GLenum format = GL_RGBA, GLenum type = GL_UNSIGNED_BYTE);
    
    // Create 3D texture
    TexturePtr Create3D(int width, int height, int depth, GLenum internalFormat,
               GLenum format = GL_RGBA, GLenum type = GL_FLOAT);
    
    // Create 2D array texture
    TexturePtr CreateArray2D(int width, int height, int layers, GLenum internalFormat,
          GLenum format = GL_RGBA, GLenum type = GL_UNSIGNED_BYTE);
    
    // Create cubemap
  TexturePtr CreateCubemap(int size, GLenum internalFormat,
    GLenum format = GL_RGB, GLenum type = GL_FLOAT);
  
    // Create depth texture
    TexturePtr CreateDepth(int width, int height, bool shadow = false);
    
    // Create HDR texture
    TexturePtr CreateHDR(int width, int height);
    
    // Create multisample texture
    TexturePtr CreateMultisample(int width, int height, int samples, GLenum internalFormat);
    
    // Create 1x1 solid color texture (useful for defaults)
    TexturePtr CreateSolidColor(const glm::vec4& color, GLenum internalFormat = GL_RGBA8);
}
