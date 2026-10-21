#pragma once

#include <cstdint>
#include <string>
#include "stb_image.h"
#include "rhi/RHI.h"
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

// Texture dimensionality (backend neutral; see rhi::TextureType).
enum class TextureTarget {
	Texture1D,
	Texture2D,
	Texture3D,
	Texture1DArray,
	Texture2DArray,
	TextureCubeMap,
	TextureCubeMapArray,
	Texture2DMultisample,
	Texture2DMultisampleArray,
	TextureRectangle
};
rhi::TextureType ToRhi(TextureTarget target);

/**
 * @brief Engine texture: an rhi::TextureHandle plus the metadata the renderer needs.
 *
 * Storage is resizable (rhi::TextureDesc::immutable = false): the base level is allocated on
 * creation and further mip levels by GenerateMipmaps(), so Resize() keeps the handle valid.
 */
class Texture {
public:
	// Builder Pattern for Flexible Texture Creation
	class Builder;

	// Constructors
	Texture();

	// Load from file (2D texture)
	Texture(const std::string& filepath,
		TextureType type = TextureType::Custom,
		bool generateMipmaps = true,
		bool srgb = false);

	// Create a 2D texture with specific dimensions and format
	Texture(int width,
		int height,
		rhi::Format internalFormat,
		rhi::PixelFormat format,
		rhi::PixelType dataType,
		const void* data = nullptr,
		bool generateMipmaps = true);

	~Texture();

	// Disable copy (expensive operation), allow move
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;
	Texture(Texture&& other) noexcept;
	Texture& operator=(Texture&& other) noexcept;

	// Binding: texture unit index (0, 1, 2, ...)
	void Bind(uint32_t unit) const;

	// Parameter Configuration
	void SetParameters(rhi::Wrap wrapS, rhi::Wrap wrapT, rhi::Filter minFilter, rhi::Filter magFilter);
	void SetWrapMode(rhi::Wrap wrapS, rhi::Wrap wrapT, rhi::Wrap wrapR = rhi::Wrap::Repeat);
	void SetFilterMode(rhi::Filter minFilter, rhi::Filter magFilter);
	void SetBorderColor(const glm::vec4& color);
	void SetCompareMode(bool enabled, rhi::CompareOp op = rhi::CompareOp::LessEqual);

	// Mipmap Management
	void GenerateMipmaps();
	void SetMipmapRange(int baseLevel, int maxLevel);

	// Data Upload
	void Upload2D(int level, int xoffset, int yoffset, int width, int height,
		rhi::PixelFormat format, rhi::PixelType type, const void* data);
	void Upload3D(int level, int xoffset, int yoffset, int zoffset,
		int width, int height, int depth,
		rhi::PixelFormat format, rhi::PixelType type, const void* data);

	// Getters
	rhi::TextureHandle ID() const { return id_; }
	TextureType Type() const { return type_; }
	TextureTarget Target() const { return target_; }
	int Width() const { return width_; }
	int Height() const { return height_; }
	int Depth() const { return depth_; }
	rhi::Format InternalFormat() const { return internalFormat_; }
	rhi::PixelFormat Format() const { return format_; }
	rhi::PixelType DataType() const { return dataType_; }
	bool HasMipmaps() const { return hasMipmaps_; }
	// File the texture was decoded from (empty for procedural textures).
	const std::string& SourcePath() const { return sourcePath_; }
	// True when the file identity (size, modification time) still matches the decoded pixels.
	bool SourceMatches(uint64_t size, int64_t mtime) const { return sourceSize_ == size && sourceMtime_ == mtime; }

	// Hot reload: decodes the source file again into this texture's GPU storage (the handle stays valid, so every
	// holder sees the new pixels). A new size resizes in place; a different channel count is refused because the
	// format would change. False (texture unchanged) when the file cannot be decoded.
	bool ReloadFromFile();

	// Utility
	bool IsValid() const;
	void Resize(int width, int height, int depth = 1);   // contents undefined, handle kept
	void Clear(const glm::vec4& color = glm::vec4(0.0f));

private:
	rhi::TextureHandle id_;
	TextureTarget target_ = TextureTarget::Texture2D;
	int width_ = 0;
	int height_ = 0;
	int depth_ = 1;
	TextureType type_ = TextureType::Custom;
	rhi::Format internalFormat_ = rhi::Format::RGBA8;
	rhi::PixelFormat format_ = rhi::PixelFormat::RGBA;
	rhi::PixelType dataType_ = rhi::PixelType::UByte;
	bool hasMipmaps_ = false;
	rhi::Wrap wrapR_ = rhi::Wrap::Repeat;   // last R wrap (only 3D and cube textures set it)
	// File-backed textures: where the pixels came from, whether they are sRGB, and the file identity they were decoded
	// from (size and modification time) so an edit on disk can be noticed.
	std::string sourcePath_;
	bool srgb_ = false;
	uint64_t sourceSize_ = 0;
	int64_t sourceMtime_ = -1;

	void Allocate();
	void RegisterFileTexture();
	void UnregisterFileTexture();
	// Helper to set default parameters based on type
	void SetParametersForType(bool generateMipmaps);

	friend class Builder;
};

// Editor hot reload: re-decodes every file-backed texture whose source file changed on disk since it was decoded.
// Checks at most once per minIntervalSeconds. Returns how many textures were reloaded. GL thread only.
int ReloadChangedTextureFiles(double minIntervalSeconds = 0.5);

/**
 * @brief Builder class for flexible texture creation
 *
 * Example usage:
 * @code
 * auto tex = Texture::Builder()
 *     .Target(TextureTarget::Texture2D)
 *     .Dimensions(1024, 1024)
 *     .InternalFormat(rhi::Format::RGBA16F)
 *     .FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
 *     .WrapMode(rhi::Wrap::ClampToEdge)
 *     .GenerateMipmaps(false)
 *     .Build();
 * @endcode
 */
class Texture::Builder {
public:
	Builder();

	Builder& Target(TextureTarget target);
	Builder& Dimensions(int width, int height = 1, int depth = 1);
	Builder& InternalFormat(rhi::Format format);     // also derives Format/DataType
	Builder& Format(rhi::PixelFormat format);
	Builder& DataType(rhi::PixelType type);
	Builder& Data(const void* data);
	Builder& FilterMode(rhi::Filter min, rhi::Filter mag);
	Builder& WrapMode(rhi::Wrap all);
	Builder& WrapMode(rhi::Wrap s, rhi::Wrap t, rhi::Wrap r);
	Builder& BorderColor(const glm::vec4& color);
	Builder& CompareMode(bool enabled, rhi::CompareOp op = rhi::CompareOp::LessEqual);
	Builder& GenerateMipmaps(bool generate);
	Builder& TextureType(enum TextureType type);
	Builder& Samples(int samples); // For multisample textures
	Builder& FixedSampleLocations(bool fixed); // For multisample textures

	// Specialized builders for common texture types
	static Builder Texture2D(int width, int height, rhi::Format internalFormat);
	static Builder Texture3D(int width, int height, int depth, rhi::Format internalFormat);
	static Builder TextureArray2D(int width, int height, int layers, rhi::Format internalFormat);
	static Builder TextureCube(int size, rhi::Format internalFormat);
	static Builder DepthTexture(int width, int height, bool shadow = false);
	static Builder HDRTexture(int width, int height);
	static Builder MultisampleTexture(int width, int height, int samples, rhi::Format internalFormat);

	std::shared_ptr<Texture> Build();

private:
	TextureTarget target_;
	int width_, height_, depth_;
	rhi::Format internalFormat_;
	rhi::PixelFormat format_;
	rhi::PixelType dataType_;
	const void* data_;
	rhi::Filter minFilter_, magFilter_;
	rhi::Wrap wrapS_, wrapT_, wrapR_;
	glm::vec4 borderColor_;
	bool compareEnabled_;
	rhi::CompareOp compareOp_;
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

// Factory Functions for Common Texture Types
namespace TextureFactory {
	// Create a standard 2D texture from file
	TexturePtr FromFile(const std::string& path, bool srgb = false, bool mipmaps = true);

	// Create empty 2D texture
	TexturePtr Create2D(int width, int height, rhi::Format internalFormat,
		rhi::PixelFormat format = rhi::PixelFormat::RGBA, rhi::PixelType type = rhi::PixelType::UByte);

	// Create 3D texture
	TexturePtr Create3D(int width, int height, int depth, rhi::Format internalFormat,
		rhi::PixelFormat format = rhi::PixelFormat::RGBA, rhi::PixelType type = rhi::PixelType::Float);

	// Create 2D array texture
	TexturePtr CreateArray2D(int width, int height, int layers, rhi::Format internalFormat,
		rhi::PixelFormat format = rhi::PixelFormat::RGBA, rhi::PixelType type = rhi::PixelType::UByte);

	// Create cubemap
	TexturePtr CreateCubemap(int size, rhi::Format internalFormat,
		rhi::PixelFormat format = rhi::PixelFormat::RGB, rhi::PixelType type = rhi::PixelType::Float);

	// Create depth texture
	TexturePtr CreateDepth(int width, int height, bool shadow = false);

	// Create HDR texture
	TexturePtr CreateHDR(int width, int height);

	// Create multisample texture
	TexturePtr CreateMultisample(int width, int height, int samples, rhi::Format internalFormat);

	// Create 1x1 solid color texture (useful for defaults)
	TexturePtr CreateSolidColor(const glm::vec4& color, rhi::Format internalFormat = rhi::Format::RGBA8);
}
