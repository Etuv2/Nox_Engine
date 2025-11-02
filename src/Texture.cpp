#include "Texture.h"
#include <iostream>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// ===== Texture Class Implementation =====

Texture::Texture()
    : id_(0), target_(TextureTarget::Texture2D), width_(0), height_(0), depth_(1),
    type_(TextureType::Custom), internalFormat_(GL_RGBA), format_(GL_RGBA),
  dataType_(GL_UNSIGNED_BYTE), hasMipmaps_(false)
{
    glGenTextures(1, &id_);
}

Texture::Texture(const std::string& filepath, TextureType type, bool generateMipmaps, bool srgb)
    : type_(type), target_(TextureTarget::Texture2D), depth_(1), hasMipmaps_(generateMipmaps)
{
    id_ = 0;
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    int channels;
    unsigned char* data = stbi_load(filepath.c_str(), &width_, &height_, &channels, 0);
    if (!data) {
        std::cerr << "[Texture] Failed to load: " << filepath << std::endl;
        width_ = height_ = 0;
     return;
    }

// Determine formats based on channels
    switch (channels) {
        case 1:
      internalFormat_ = format_ = GL_RED;
         break;
        case 2:
            internalFormat_ = format_ = GL_RG;
            break;
        case 3:
    format_ = GL_RGB;
        internalFormat_ = srgb ? GL_SRGB8 : GL_RGB8;
  break;
   case 4:
            format_ = GL_RGBA;
       internalFormat_ = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
  break;
  default:
            internalFormat_ = format_ = GL_RGBA;
    }

    dataType_ = GL_UNSIGNED_BYTE;

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat_, width_, height_, 0, format_, dataType_, data);
    
    if (generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    SetParametersForType(generateMipmaps);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture::Texture(int width, int height, GLenum internalFormat, GLenum format,
    GLenum dataType, const void* data, bool generateMipmaps)
    : width_(width), height_(height), depth_(1), internalFormat_(internalFormat),
    format_(format), dataType_(dataType), type_(TextureType::Custom),
    target_(TextureTarget::Texture2D), hasMipmaps_(generateMipmaps)
{
    id_ = 0;
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat_, width_, height_, 0, format_, dataType_, data);
    
    if (generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    SetParametersForType(generateMipmaps);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture::~Texture() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

Texture::Texture(Texture&& other) noexcept
    : id_(other.id_), target_(other.target_), width_(other.width_), height_(other.height_),
      depth_(other.depth_), type_(other.type_), internalFormat_(other.internalFormat_),
      format_(other.format_), dataType_(other.dataType_), hasMipmaps_(other.hasMipmaps_)
{
    other.id_ = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (id_) glDeleteTextures(1, &id_);
        
        id_ = other.id_;
        target_ = other.target_;
        width_ = other.width_;
        height_ = other.height_;
 depth_ = other.depth_;
      type_ = other.type_;
      internalFormat_ = other.internalFormat_;
        format_ = other.format_;
        dataType_ = other.dataType_;
        hasMipmaps_ = other.hasMipmaps_;
        
        other.id_ = 0;
    }
    return *this;
}

void Texture::Bind(GLenum unit) const {
    glActiveTexture(unit);
    BindToTarget();
}

void Texture::Unbind() const {
    glBindTexture(static_cast<GLenum>(target_), 0);
}

void Texture::SetParameters(GLint wrapS, GLint wrapT, GLint minFilter, GLint magFilter) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrapT);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
    Unbind();
}

void Texture::SetWrapMode(GLint wrapS, GLint wrapT, GLint wrapR) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrapS);
    if (target_ != TextureTarget::Texture1D) {
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wrapT);
    }
    if (target_ == TextureTarget::Texture3D || target_ == TextureTarget::TextureCubeMap) {
   glTexParameteri(target, GL_TEXTURE_WRAP_R, wrapR);
    }
    Unbind();
}

void Texture::SetFilterMode(GLint minFilter, GLint magFilter) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
    Unbind();
}

void Texture::SetBorderColor(const glm::vec4& color) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, glm::value_ptr(color));
    Unbind();
}

void Texture::SetCompareMode(GLenum mode, GLenum func) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, mode);
    glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, func);
    Unbind();
}

void Texture::GenerateMipmaps() {
    BindToTarget();
    glGenerateMipmap(static_cast<GLenum>(target_));
  hasMipmaps_ = true;
    Unbind();
}

void Texture::SetMipmapRange(GLint baseLevel, GLint maxLevel) {
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, baseLevel);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, maxLevel);
    Unbind();
}

void Texture::Upload2D(GLint level, GLint xoffset, GLint yoffset, 
    GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data) {
    BindToTarget();
    glTexSubImage2D(static_cast<GLenum>(target_), level, xoffset, yoffset, 
   width, height, format, type, data);
    Unbind();
}

void Texture::Upload3D(GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
    GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* data) {
    BindToTarget();
    glTexSubImage3D(static_cast<GLenum>(target_), level, xoffset, yoffset, zoffset,
        width, height, depth, format, type, data);
    Unbind();
}

void Texture::Resize(int width, int height, int depth) {
    width_ = width;
    height_ = height;
    depth_ = depth;
    
    BindToTarget();
  GLenum target = static_cast<GLenum>(target_);
    
    switch (target_) {
        case TextureTarget::Texture1D:
            glTexImage1D(target, 0, internalFormat_, width_, 0, format_, dataType_, nullptr);
     break;
        case TextureTarget::Texture2D:
    glTexImage2D(target, 0, internalFormat_, width_, height_, 0, format_, dataType_, nullptr);
            break;
        case TextureTarget::Texture3D:
            glTexImage3D(target, 0, internalFormat_, width_, height_, depth_, 0, format_, dataType_, nullptr);
     break;
        default:
       std::cerr << "[Texture] Resize not supported for this texture target" << std::endl;
    }

    if (hasMipmaps_) {
        glGenerateMipmap(target);
    }
    
    Unbind();
}

void Texture::Clear(const glm::vec4& color) {
    if (!IsValid()) return;
    
    // Create temporary buffer filled with color
    size_t pixelCount = width_ * height_ * depth_;
    std::vector<glm::vec4> clearData(pixelCount, color);
    
    BindToTarget();
    GLenum target = static_cast<GLenum>(target_);
    
    switch (target_) {
        case TextureTarget::Texture2D:
            glTexSubImage2D(target, 0, 0, 0, width_, height_, GL_RGBA, GL_FLOAT, clearData.data());
            break;
        case TextureTarget::Texture3D:
 glTexSubImage3D(target, 0, 0, 0, 0, width_, height_, depth_, GL_RGBA, GL_FLOAT, clearData.data());
            break;
        default:
 std::cerr << "[Texture] Clear not implemented for this texture target" << std::endl;
    }
    
    Unbind();
}

void Texture::SetParametersForType(bool generateMipmaps) {
    GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    GLint minFilter = generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    GLint magFilter = GL_LINEAR;

    switch (type_) {
        case TextureType::Normal:
  case TextureType::MetallicRoughness:
        case TextureType::Occlusion:
minFilter = generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
         magFilter = GL_LINEAR;
            break;
        case TextureType::Depth:
     case TextureType::Shadow:
            wrapS = wrapT = GL_CLAMP_TO_EDGE;
         minFilter = GL_NEAREST;
  magFilter = GL_NEAREST;
            break;
        case TextureType::HDR:
        case TextureType::Cubemap:
      wrapS = wrapT = GL_CLAMP_TO_EDGE;
            minFilter = generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
         break;
        default:
     // Diffuse, emissive, custom: use linear filtering
            break;
    }

  SetParameters(wrapS, wrapT, minFilter, magFilter);
}

void Texture::BindToTarget() const {
  glBindTexture(static_cast<GLenum>(target_), id_);
}

// ===== Builder Implementation =====

Texture::Builder::Builder()
    : target_(TextureTarget::Texture2D), width_(1), height_(1), depth_(1),
      internalFormat_(GL_RGBA8), format_(GL_RGBA), dataType_(GL_UNSIGNED_BYTE),
      data_(nullptr), minFilter_(GL_LINEAR), magFilter_(GL_LINEAR),
      wrapS_(GL_REPEAT), wrapT_(GL_REPEAT), wrapR_(GL_REPEAT),
    borderColor_(0.0f), compareMode_(GL_NONE), compareFunc_(GL_LEQUAL),
      generateMipmaps_(false), type_(TextureType::Custom), samples_(0),
    fixedSampleLocations_(true), hasBorderColor_(false), hasCompareMode_(false)
{
}

Texture::Builder& Texture::Builder::Target(TextureTarget target) {
    target_ = target;
    return *this;
}

Texture::Builder& Texture::Builder::Dimensions(int width, int height, int depth) {
    width_ = width;
    height_ = height;
    depth_ = depth;
    return *this;
}

Texture::Builder& Texture::Builder::InternalFormat(GLenum format) {
    internalFormat_ = format;
    DeriveFormats();
    return *this;
}

Texture::Builder& Texture::Builder::Format(GLenum format) {
    format_ = format;
    return *this;
}

Texture::Builder& Texture::Builder::DataType(GLenum type) {
    dataType_ = type;
    return *this;
}

Texture::Builder& Texture::Builder::Data(const void* data) {
  data_ = data;
    return *this;
}

Texture::Builder& Texture::Builder::FilterMode(GLint min, GLint mag) {
    minFilter_ = min;
  magFilter_ = mag;
    return *this;
}

Texture::Builder& Texture::Builder::WrapMode(GLint s, GLint t, GLint r) {
 wrapS_ = s;
  wrapT_ = (t == -1) ? s : t;
    wrapR_ = (r == -1) ? s : r;
    return *this;
}

Texture::Builder& Texture::Builder::BorderColor(const glm::vec4& color) {
    borderColor_ = color;
    hasBorderColor_ = true;
    return *this;
}

Texture::Builder& Texture::Builder::CompareMode(GLenum mode, GLenum func) {
    compareMode_ = mode;
    compareFunc_ = func;
    hasCompareMode_ = true;
    return *this;
}

Texture::Builder& Texture::Builder::GenerateMipmaps(bool generate) {
    generateMipmaps_ = generate;
    return *this;
}

Texture::Builder& Texture::Builder::TextureType(enum TextureType type) {
    type_ = type;
    return *this;
}

Texture::Builder& Texture::Builder::Samples(int samples) {
    samples_ = samples;
return *this;
}

Texture::Builder& Texture::Builder::FixedSampleLocations(bool fixed) {
    fixedSampleLocations_ = fixed;
    return *this;
}

// Static builder helpers
Texture::Builder Texture::Builder::Texture2D(int width, int height, GLenum internalFormat) {
    return Builder()
        .Target(TextureTarget::Texture2D)
        .Dimensions(width, height)
        .InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::Texture3D(int width, int height, int depth, GLenum internalFormat) {
    return Builder()
        .Target(TextureTarget::Texture3D)
        .Dimensions(width, height, depth)
        .InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::TextureArray2D(int width, int height, int layers, GLenum internalFormat) {
    return Builder()
        .Target(TextureTarget::Texture2DArray)
        .Dimensions(width, height, layers)
        .InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::TextureCube(int size, GLenum internalFormat) {
    return Builder()
        .Target(TextureTarget::TextureCubeMap)
        .Dimensions(size, size)
        .InternalFormat(internalFormat)
        .WrapMode(GL_CLAMP_TO_EDGE);
}

Texture::Builder Texture::Builder::DepthTexture(int width, int height, bool shadow) {
    auto builder = Builder()
        .Target(TextureTarget::Texture2D)
        .Dimensions(width, height)
        .InternalFormat(GL_DEPTH_COMPONENT24)
      .Format(GL_DEPTH_COMPONENT)
  .DataType(GL_FLOAT)
      .FilterMode(GL_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_BORDER)
        .BorderColor(glm::vec4(1.0f))
        .TextureType(TextureType::Depth);
    
    if (shadow) {
        builder.CompareMode(GL_COMPARE_REF_TO_TEXTURE, GL_LEQUAL);
    }
    
    return builder;
}

Texture::Builder Texture::Builder::HDRTexture(int width, int height) {
    return Builder()
        .Target(TextureTarget::Texture2D)
        .Dimensions(width, height)
        .InternalFormat(GL_RGBA16F)
        .Format(GL_RGBA)
        .DataType(GL_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .TextureType(TextureType::HDR);
}

Texture::Builder Texture::Builder::MultisampleTexture(int width, int height, int samples, GLenum internalFormat) {
    return Builder()
        .Target(TextureTarget::Texture2DMultisample)
        .Dimensions(width, height)
        .InternalFormat(internalFormat)
  .Samples(samples);
}

std::shared_ptr<Texture> Texture::Builder::Build() {
    auto texture = std::make_shared<Texture>();
    texture->target_ = target_;
    texture->width_ = width_;
    texture->height_ = height_;
    texture->depth_ = depth_;
    texture->internalFormat_ = internalFormat_;
    texture->format_ = format_;
    texture->dataType_ = dataType_;
    texture->type_ = type_;
    texture->hasMipmaps_ = generateMipmaps_;
    
    GLenum target = static_cast<GLenum>(target_);
    glBindTexture(target, texture->id_);
 
    // Create texture storage based on target
    switch (target_) {
     case TextureTarget::Texture1D:
       glTexImage1D(target, 0, internalFormat_, width_, 0, format_, dataType_, data_);
          break;
            
        case TextureTarget::Texture2D:
            glTexImage2D(target, 0, internalFormat_, width_, height_, 0, format_, dataType_, data_);
     break;
            
        case TextureTarget::Texture3D:
        case TextureTarget::Texture2DArray:
    glTexImage3D(target, 0, internalFormat_, width_, height_, depth_, 0, format_, dataType_, data_);
      break;
            
case TextureTarget::TextureCubeMap:
        for (int i = 0; i < 6; ++i) {
             glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, internalFormat_,
          width_, height_, 0, format_, dataType_, nullptr);
            }
 break;
          
        case TextureTarget::Texture2DMultisample:
    glTexImage2DMultisample(target, samples_, internalFormat_, width_, height_, fixedSampleLocations_);
            break;
        
        case TextureTarget::Texture2DMultisampleArray:
          glTexImage3DMultisample(target, samples_, internalFormat_, width_, height_, depth_, fixedSampleLocations_);
            break;
 
        default:
    std::cerr << "[Texture::Builder] Unsupported texture target" << std::endl;
    }
    
    // Set texture parameters (skip for multisample)
    if (target_ != TextureTarget::Texture2DMultisample && 
        target_ != TextureTarget::Texture2DMultisampleArray) {
        
 glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter_);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter_);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, wrapS_);
        
        if (target_ != TextureTarget::Texture1D) {
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wrapT_);
        }

  if (target_ == TextureTarget::Texture3D || target_ == TextureTarget::TextureCubeMap) {
      glTexParameteri(target, GL_TEXTURE_WRAP_R, wrapR_);
        }
        
        if (hasBorderColor_) {
            glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, glm::value_ptr(borderColor_));
        }
    
        if (hasCompareMode_) {
       glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, compareMode_);
            glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, compareFunc_);
        }
        
    if (generateMipmaps_) {
            glGenerateMipmap(target);
        }
    }
    
    glBindTexture(target, 0);
    
    return texture;
}

void Texture::Builder::DeriveFormats() {
    // Auto-derive format and data type from internal format
    switch (internalFormat_) {
 // Depth formats
        case GL_DEPTH_COMPONENT16:
        case GL_DEPTH_COMPONENT24:
      case GL_DEPTH_COMPONENT32F:
       format_ = GL_DEPTH_COMPONENT;
       dataType_ = GL_FLOAT;
     break;
     
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
         format_ = GL_DEPTH_STENCIL;
 dataType_ = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
   break;
     
    // Single channel
        case GL_R8:
        case GL_R16:
        case GL_R16F:
        case GL_R32F:
        format_ = GL_RED;
            dataType_ = (internalFormat_ == GL_R8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
 break;
            
      // Two channel
   case GL_RG8:
        case GL_RG16:
 case GL_RG16F:
        case GL_RG32F:
   format_ = GL_RG;
          dataType_ = (internalFormat_ == GL_RG8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
      break;
     
        // RGB
   case GL_RGB8:
        case GL_RGB16F:
        case GL_RGB32F:
        case GL_SRGB8:
        format_ = GL_RGB;
    dataType_ = (internalFormat_ == GL_RGB8 || internalFormat_ == GL_SRGB8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
          break;
  
        // RGBA
    case GL_RGBA8:
        case GL_RGBA16F:
        case GL_RGBA32F:
        case GL_SRGB8_ALPHA8:
            format_ = GL_RGBA;
   dataType_ = (internalFormat_ == GL_RGBA8 || internalFormat_ == GL_SRGB8_ALPHA8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
  break;
    
        default:
    // Keep current format/type
   break;
    }
}

// ===== Factory Functions =====

namespace TextureFactory {
    TexturePtr FromFile(const std::string& path, bool srgb, bool mipmaps) {
  return std::make_shared<Texture>(path, TextureType::Custom, mipmaps, srgb);
    }
    
    TexturePtr Create2D(int width, int height, GLenum internalFormat, GLenum format, GLenum type) {
     return Texture::Builder::Texture2D(width, height, internalFormat)
            .Format(format)
       .DataType(type)
.Build();
    }
    
    TexturePtr Create3D(int width, int height, int depth, GLenum internalFormat, GLenum format, GLenum type) {
        return Texture::Builder::Texture3D(width, height, depth, internalFormat)
    .Format(format)
         .DataType(type)
  .Build();
}
    
    TexturePtr CreateArray2D(int width, int height, int layers, GLenum internalFormat, GLenum format, GLenum type) {
     return Texture::Builder::TextureArray2D(width, height, layers, internalFormat)
       .Format(format)
            .DataType(type)
    .Build();
    }
    
    TexturePtr CreateCubemap(int size, GLenum internalFormat, GLenum format, GLenum type) {
    return Texture::Builder::TextureCube(size, internalFormat)
.Format(format)
            .DataType(type)
         .Build();
    }
  
    TexturePtr CreateDepth(int width, int height, bool shadow) {
        return Texture::Builder::DepthTexture(width, height, shadow).Build();
    }
    
    TexturePtr CreateHDR(int width, int height) {
        return Texture::Builder::HDRTexture(width, height).Build();
    }
    
    TexturePtr CreateMultisample(int width, int height, int samples, GLenum internalFormat) {
    return Texture::Builder::MultisampleTexture(width, height, samples, internalFormat).Build();
    }
    
    TexturePtr CreateSolidColor(const glm::vec4& color, GLenum internalFormat) {
        // Convert color to appropriate format
        std::array<uint8_t, 4> pixelData = {
         static_cast<uint8_t>(color.r * 255.0f),
            static_cast<uint8_t>(color.g * 255.0f),
       static_cast<uint8_t>(color.b * 255.0f),
            static_cast<uint8_t>(color.a * 255.0f)
 };
        
        return Texture::Builder::Texture2D(1, 1, internalFormat)
        .Data(pixelData.data())
        .FilterMode(GL_NEAREST, GL_NEAREST)
          .Build();
    }
}
