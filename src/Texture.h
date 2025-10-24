#pragma once

#include <string>
#include <GL/glew.h>
#include "stb_image.h"
#include <array>
#include <memory>

// Types of texture for identification
enum class TextureType {
    Diffuse,
    Normal,
    MetallicRoughness,
    Specular,
    Emissive,
    Occlusion,
    Depth,
    Custom
};

class Texture {
public:
    Texture();

    Texture(const std::string& filepath,
        TextureType type = TextureType::Custom,
        bool generateMipmaps = true,
        bool srgb = false);

    Texture(int width,
        int height,
        GLenum internalFormat,
        GLenum format,
        GLenum dataType,
        const void* data = nullptr,
        bool generateMipmaps = true);

    ~Texture();

    void Bind(GLenum unit) const;
    void Unbind() const;

    void SetParameters(GLint wrapS, GLint wrapT,
        GLint minFilter, GLint magFilter);

    void GenerateMipmaps();

    GLuint ID() const { return id_; }
    TextureType Type() const { return type_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    GLuint id_;
    int width_;
    int height_;
    TextureType type_;
    GLenum internalFormat_;
    GLenum format_;
    GLenum dataType_;

    static constexpr size_t MaxTextures = 2048;
    static std::array<bool, MaxTextures> usedSlots_;
    static GLuint AllocateTextureSlot();
    static void ReleaseTextureSlot(GLuint id_);

    void SetParametersForType(bool generateMipmaps);
};

using TexturePtr = std::shared_ptr<Texture>;
