#include "Texture.h"
#include <iostream>
#include <algorithm>

std::array<bool, Texture::MaxTextures> Texture::usedSlots_ = { false };

GLuint Texture::AllocateTextureSlot() {
    for (size_t i = 1; i < MaxTextures; ++i) {
        if (!usedSlots_[i]) {
            usedSlots_[i] = true;
            return static_cast<GLuint>(i);
        }
    }
    std::cerr << "[Texture] Warning: All texture slots are used.\n";
    return 0;
}

void Texture::ReleaseTextureSlot(GLuint id) {
    if (id < MaxTextures) usedSlots_[id] = false;
}

Texture::Texture() : id_(0), width_(0), height_(0), type_(TextureType::Custom),
internalFormat_(GL_RGBA), format_(GL_RGBA), dataType_(GL_UNSIGNED_BYTE) {
    id_ = AllocateTextureSlot();
    glGenTextures(1, &id_);
}

Texture::Texture(const std::string& filepath,
    TextureType type,
    bool generateMipmaps,
    bool srgb)
    : type_(type) {
    id_ = AllocateTextureSlot();
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    int channels;
    unsigned char* data = stbi_load(filepath.c_str(), &width_, &height_, &channels, 0);
    if (!data) {
        std::cerr << "[Texture] Failed to load: " << filepath << std::endl;
        width_ = height_ = 0;
        return;
    }

    switch (channels) {
    case 1:
        internalFormat_ = format_ = GL_RED;
        break;
    case 3:
        format_ = GL_RGB;
        internalFormat_ = srgb ? GL_SRGB : GL_RGB;
        break;
    case 4:
        format_ = GL_RGBA;
        internalFormat_ = srgb ? GL_SRGB_ALPHA : GL_RGBA;
        break;
    }

    dataType_ = GL_UNSIGNED_BYTE;

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat_, width_, height_, 0, format_, dataType_, data);
    if (generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    SetParametersForType(generateMipmaps); // <== AUTOMATED PARAM SETUP

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture::Texture(int width,
    int height,
    GLenum internalFormat,
    GLenum format,
    GLenum dataType,
    const void* data,
    bool generateMipmaps)
    : width_(width), height_(height), internalFormat_(internalFormat),
    format_(format), dataType_(dataType), type_(TextureType::Custom) {
    id_ = AllocateTextureSlot();
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat_, width_, height_, 0, format_, dataType_, data);
    if (generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    SetParametersForType(generateMipmaps); // <== AUTOMATED PARAM SETUP

    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture::~Texture() {
    if (id_) {
        glDeleteTextures(1, &id_);
        ReleaseTextureSlot(id_);
    }
}

void Texture::Bind(GLenum unit) const {
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

void Texture::Unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::SetParameters(GLint wrapS, GLint wrapT,
    GLint minFilter, GLint magFilter) {
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::GenerateMipmaps() {
    glBindTexture(GL_TEXTURE_2D, id_);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::SetParametersForType(bool generateMipmaps) {
    GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    GLint minFilter = generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    GLint magFilter = GL_LINEAR;

    switch (type_) {
    case TextureType::Normal:
    case TextureType::MetallicRoughness:
    case TextureType::Specular:
    case TextureType::Occlusion:
        // Use sharper sampling for detail-preserving maps
        minFilter = generateMipmaps ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
        magFilter = GL_NEAREST;
        break;
    case TextureType::Depth:
        wrapS = wrapT = GL_CLAMP_TO_BORDER;
        break;
    default:
        // Diffuse, emissive, custom: use linear filtering
        break;
    }

    SetParameters(wrapS, wrapT, minFilter, magFilter);
}
