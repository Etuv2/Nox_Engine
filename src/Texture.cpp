#include "Texture.h"
#include "assets/AssetIO.h"
#include "assets/AssetIOImage.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <glm/glm.hpp>

rhi::TextureType ToRhi(TextureTarget target) {
	switch (target) {
	case TextureTarget::Texture1D: return rhi::TextureType::Tex1D;
	case TextureTarget::Texture2D: return rhi::TextureType::Tex2D;
	case TextureTarget::Texture3D: return rhi::TextureType::Tex3D;
	case TextureTarget::Texture1DArray: return rhi::TextureType::Tex1DArray;
	case TextureTarget::Texture2DArray: return rhi::TextureType::Tex2DArray;
	case TextureTarget::TextureCubeMap: return rhi::TextureType::Cube;
	case TextureTarget::TextureCubeMapArray: return rhi::TextureType::CubeArray;
	case TextureTarget::Texture2DMultisample: return rhi::TextureType::Tex2DMS;
	case TextureTarget::Texture2DMultisampleArray: return rhi::TextureType::Tex2DMSArray;
	case TextureTarget::TextureRectangle: return rhi::TextureType::Rectangle;
	}
	return rhi::TextureType::Tex2D;
}

namespace {
	bool IsMultisample(TextureTarget t) {
		return t == TextureTarget::Texture2DMultisample || t == TextureTarget::Texture2DMultisampleArray;
	}

	// Storage format for a decoded image with `channels` components.
	void ChannelFormats(int channels, bool srgb, rhi::Format& internalFormat, rhi::PixelFormat& format) {
		switch (channels) {
		case 1:
			internalFormat = rhi::Format::R8;
			format = rhi::PixelFormat::R;
			break;
		case 2:
			internalFormat = rhi::Format::RG8;
			format = rhi::PixelFormat::RG;
			break;
		case 3:
			format = rhi::PixelFormat::RGB;
			internalFormat = srgb ? rhi::Format::SRGB8 : rhi::Format::RGB8;
			break;
		default:
			format = rhi::PixelFormat::RGBA;
			internalFormat = srgb ? rhi::Format::SRGB8_A8 : rhi::Format::RGBA8;
			break;
		}
	}

	// File-backed textures alive now, for hot reload. Textures are created and destroyed on the GL thread; the lock
	// only guards the set against the poll reading it.
	// Deliberately never destroyed: a Texture can be released by another function-local static (the scene's shared
	// image cache) during exit, and its destructor unregisters here. Destroying these first crashed the exit path.
	std::mutex& FileTexturesMutex() {
		static std::mutex& m = *new std::mutex();
		return m;
	}
	std::set<Texture*>& FileTextures() {
		static std::set<Texture*>& s = *new std::set<Texture*>();
		return s;
	}
}

void Texture::RegisterFileTexture() {
	if (sourcePath_.empty()) return;
	std::lock_guard<std::mutex> lock(FileTexturesMutex());
	FileTextures().insert(this);
}

void Texture::UnregisterFileTexture() {
	std::lock_guard<std::mutex> lock(FileTexturesMutex());
	FileTextures().erase(this);
}

// ===== Texture Class Implementation =====

Texture::Texture() = default;

void Texture::Allocate() {
	rhi::TextureDesc desc;
	desc.type = ToRhi(target_);
	desc.format = internalFormat_;
	desc.width = width_;
	desc.height = height_;
	desc.depth = depth_;
	desc.mipLevels = 1;
	id_ = rhi::Gpu().CreateTexture(desc);
}

Texture::Texture(const std::string& filepath, TextureType type, bool generateMipmaps, bool srgb)
	: type_(type), target_(TextureTarget::Texture2D), depth_(1), hasMipmaps_(generateMipmaps)
{
	int channels;
	unsigned char* data = AssetIO::LoadImageFile(filepath, &width_, &height_, &channels, 0);
	if (!data) {
		std::cerr << "[Texture] Failed to load: " << filepath << std::endl;
		width_ = height_ = 0;
		return;
	}

	// Determine formats based on channels
	ChannelFormats(channels, srgb, internalFormat_, format_);
	dataType_ = rhi::PixelType::UByte;

	Allocate();
	rhi::Gpu().UpdateTexture(id_, {}, format_, dataType_, data);
	if (generateMipmaps) {
		rhi::Gpu().GenerateMipmaps(id_);
	}
	SetParametersForType(generateMipmaps);
	stbi_image_free(data);

	sourcePath_ = filepath;
	srgb_ = srgb;
	if (const auto info = AssetIO::Stat(filepath); info && !info->isDirectory) {
		sourceSize_ = info->size;
		sourceMtime_ = info->modifiedTime;
	}
	RegisterFileTexture();
}

bool Texture::ReloadFromFile() {
	if (sourcePath_.empty() || !id_) return false;
	int width = 0, height = 0, channels = 0;
	unsigned char* data = AssetIO::LoadImageFile(sourcePath_, &width, &height, &channels, 0);
	if (!data) {
		std::cerr << "[HotReload] could not decode " << sourcePath_ << "; keeping the previous pixels\n";
		return false;
	}
	rhi::Format internalFormat = internalFormat_;
	rhi::PixelFormat format = format_;
	ChannelFormats(channels, srgb_, internalFormat, format);
	if (internalFormat != internalFormat_) {
		stbi_image_free(data);
		std::cerr << "[HotReload] " << sourcePath_ << " changed its channel count; restart to pick it up\n";
		return false;
	}
	if (width != width_ || height != height_) {
		Resize(width, height, 1);
	}
	rhi::Gpu().UpdateTexture(id_, {}, format_, dataType_, data);
	if (hasMipmaps_) {
		rhi::Gpu().GenerateMipmaps(id_);
	}
	stbi_image_free(data);

	if (const auto info = AssetIO::Stat(sourcePath_); info && !info->isDirectory) {
		sourceSize_ = info->size;
		sourceMtime_ = info->modifiedTime;
	}
	std::cout << "[HotReload] reloaded texture " << sourcePath_ << " (" << width << "x" << height << ")\n";
	return true;
}

int ReloadChangedTextureFiles(double minIntervalSeconds) {
	using Clock = std::chrono::steady_clock;
	static Clock::time_point lastCheck{};
	const Clock::time_point now = Clock::now();
	if (lastCheck != Clock::time_point{} && std::chrono::duration<double>(now - lastCheck).count() < minIntervalSeconds) {
		return 0;
	}
	lastCheck = now;

	// Group by file: one stat per file, and the textures decoded from it are checked against that stat.
	std::map<std::string, std::vector<Texture*>> byPath;
	{
		std::lock_guard<std::mutex> lock(FileTexturesMutex());
		for (Texture* t : FileTextures()) byPath[t->SourcePath()].push_back(t);
	}
	int reloaded = 0;
	for (const auto& [path, textures] : byPath) {
		const auto info = AssetIO::Stat(path);
		if (!info || info->isDirectory) continue;
		for (Texture* t : textures) {
			if (t->SourceMatches(info->size, info->modifiedTime)) continue;
			if (t->ReloadFromFile()) ++reloaded;
		}
	}
	return reloaded;
}

Texture::Texture(int width, int height, rhi::Format internalFormat, rhi::PixelFormat format,
	rhi::PixelType dataType, const void* data, bool generateMipmaps)
	: width_(width), height_(height), depth_(1), internalFormat_(internalFormat),
	format_(format), dataType_(dataType), type_(TextureType::Custom),
	target_(TextureTarget::Texture2D), hasMipmaps_(generateMipmaps)
{
	Allocate();
	if (data) {
		rhi::Gpu().UpdateTexture(id_, {}, format_, dataType_, data);
	}
	if (generateMipmaps) {
		rhi::Gpu().GenerateMipmaps(id_);
	}
	SetParametersForType(generateMipmaps);
}

Texture::~Texture() {
	UnregisterFileTexture();
	if (id_ && rhi::HasGpu()) {
		rhi::Gpu().DestroyTexture(id_);
	}
}

Texture::Texture(Texture&& other) noexcept
	: id_(other.id_), target_(other.target_), width_(other.width_), height_(other.height_),
	depth_(other.depth_), type_(other.type_), internalFormat_(other.internalFormat_),
	format_(other.format_), dataType_(other.dataType_), hasMipmaps_(other.hasMipmaps_), wrapR_(other.wrapR_)
{
	other.UnregisterFileTexture();
	sourcePath_ = std::move(other.sourcePath_);
	srgb_ = other.srgb_;
	sourceSize_ = other.sourceSize_;
	sourceMtime_ = other.sourceMtime_;
	other.sourcePath_.clear();
	RegisterFileTexture();
	other.id_ = {};
}

Texture& Texture::operator=(Texture&& other) noexcept {
	if (this != &other) {
		if (id_) rhi::Gpu().DestroyTexture(id_);
		UnregisterFileTexture();
		other.UnregisterFileTexture();
		sourcePath_ = std::move(other.sourcePath_);
		srgb_ = other.srgb_;
		sourceSize_ = other.sourceSize_;
		sourceMtime_ = other.sourceMtime_;
		other.sourcePath_.clear();
		RegisterFileTexture();

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
		wrapR_ = other.wrapR_;

		other.id_ = {};
	}
	return *this;
}

void Texture::Bind(uint32_t unit) const {
	rhi::Gpu().BindTexture(unit, id_);
}

void Texture::SetParameters(rhi::Wrap wrapS, rhi::Wrap wrapT, rhi::Filter minFilter, rhi::Filter magFilter) {
	if (IsMultisample(target_)) return;
	rhi::Gpu().SetTextureWrap(id_, wrapS, wrapT, wrapR_);   // R unchanged
	rhi::Gpu().SetTextureFilter(id_, minFilter, magFilter);
}

void Texture::SetWrapMode(rhi::Wrap wrapS, rhi::Wrap wrapT, rhi::Wrap wrapR) {
	if (IsMultisample(target_)) return;
	// Only volume and cube textures sample with R; the others keep the default (Repeat).
	const bool usesR = target_ == TextureTarget::Texture3D || target_ == TextureTarget::TextureCubeMap;
	if (usesR) wrapR_ = wrapR;
	rhi::Gpu().SetTextureWrap(id_, wrapS, target_ == TextureTarget::Texture1D ? rhi::Wrap::Repeat : wrapT, wrapR_);
}

void Texture::SetFilterMode(rhi::Filter minFilter, rhi::Filter magFilter) {
	if (IsMultisample(target_)) return;
	rhi::Gpu().SetTextureFilter(id_, minFilter, magFilter);
}

void Texture::SetBorderColor(const glm::vec4& color) {
	if (IsMultisample(target_)) return;
	rhi::Gpu().SetTextureBorderColor(id_, color);
}

void Texture::SetCompareMode(bool enabled, rhi::CompareOp op) {
	if (IsMultisample(target_)) return;
	rhi::Gpu().SetTextureCompare(id_, enabled, op);
}

void Texture::GenerateMipmaps() {
	rhi::Gpu().GenerateMipmaps(id_);
	hasMipmaps_ = true;
}

void Texture::SetMipmapRange(int baseLevel, int maxLevel) {
	rhi::Gpu().SetTextureMipRange(id_, baseLevel, maxLevel);
}

void Texture::Upload2D(int level, int xoffset, int yoffset, int width, int height,
	rhi::PixelFormat format, rhi::PixelType type, const void* data) {
	rhi::TextureRegion region;
	region.level = level;
	region.offset = glm::ivec3(xoffset, yoffset, 0);
	region.extent = glm::ivec3(width, height, 1);
	rhi::Gpu().UpdateTexture(id_, region, format, type, data);
}

void Texture::Upload3D(int level, int xoffset, int yoffset, int zoffset,
	int width, int height, int depth, rhi::PixelFormat format, rhi::PixelType type, const void* data) {
	rhi::TextureRegion region;
	region.level = level;
	region.offset = glm::ivec3(xoffset, yoffset, zoffset);
	region.extent = glm::ivec3(width, height, depth);
	rhi::Gpu().UpdateTexture(id_, region, format, type, data);
}

bool Texture::IsValid() const {
	return id_ && rhi::Gpu().IsTexture(id_);
}

void Texture::Resize(int width, int height, int depth) {
	width_ = width;
	height_ = height;
	depth_ = depth;

	switch (target_) {
	case TextureTarget::Texture1D:
	case TextureTarget::Texture2D:
	case TextureTarget::Texture3D:
		rhi::Gpu().ResizeTexture(id_, width_, height_, depth_, 1);
		break;
	default:
		std::cerr << "[Texture] Resize not supported for this texture target" << std::endl;
		return;
	}

	if (hasMipmaps_) {
		rhi::Gpu().GenerateMipmaps(id_);
	}
}

void Texture::Clear(const glm::vec4& color) {
	if (!IsValid()) return;

	// Temporary buffer filled with the colour (float RGBA upload).
	const size_t pixelCount = static_cast<size_t>(width_) * height_ * depth_;
	std::vector<glm::vec4> clearData(pixelCount, color);

	switch (target_) {
	case TextureTarget::Texture2D:
		Upload2D(0, 0, 0, width_, height_, rhi::PixelFormat::RGBA, rhi::PixelType::Float, clearData.data());
		break;
	case TextureTarget::Texture3D:
		Upload3D(0, 0, 0, 0, width_, height_, depth_, rhi::PixelFormat::RGBA, rhi::PixelType::Float, clearData.data());
		break;
	default:
		std::cerr << "[Texture] Clear not implemented for this texture target" << std::endl;
	}
}

void Texture::SetParametersForType(bool generateMipmaps) {
	rhi::Wrap wrapS = rhi::Wrap::Repeat, wrapT = rhi::Wrap::Repeat;
	rhi::Filter minFilter = generateMipmaps ? rhi::Filter::LinearMipLinear : rhi::Filter::Linear;
	rhi::Filter magFilter = rhi::Filter::Linear;

	switch (type_) {
	case TextureType::Normal:
	case TextureType::MetallicRoughness:
	case TextureType::Occlusion:
		minFilter = generateMipmaps ? rhi::Filter::LinearMipLinear : rhi::Filter::Linear;
		magFilter = rhi::Filter::Linear;
		break;
	case TextureType::Depth:
	case TextureType::Shadow:
		wrapS = wrapT = rhi::Wrap::ClampToEdge;
		minFilter = rhi::Filter::Nearest;
		magFilter = rhi::Filter::Nearest;
		break;
	case TextureType::HDR:
	case TextureType::Cubemap:
		wrapS = wrapT = rhi::Wrap::ClampToEdge;
		minFilter = generateMipmaps ? rhi::Filter::LinearMipLinear : rhi::Filter::Linear;
		break;
	default:
		// Diffuse, emissive, custom: use linear filtering
		break;
	}

	SetParameters(wrapS, wrapT, minFilter, magFilter);
}

// ===== Builder Implementation =====

Texture::Builder::Builder()
	: target_(TextureTarget::Texture2D), width_(1), height_(1), depth_(1),
	internalFormat_(rhi::Format::RGBA8), format_(rhi::PixelFormat::RGBA), dataType_(rhi::PixelType::UByte),
	data_(nullptr), minFilter_(rhi::Filter::Linear), magFilter_(rhi::Filter::Linear),
	wrapS_(rhi::Wrap::Repeat), wrapT_(rhi::Wrap::Repeat), wrapR_(rhi::Wrap::Repeat),
	borderColor_(0.0f), compareEnabled_(false), compareOp_(rhi::CompareOp::LessEqual),
	generateMipmaps_(false), type_(TextureType::Custom), samples_(0),
	fixedSampleLocations_(true), hasBorderColor_(false), hasCompareMode_(false)
{
}

Texture::Builder& Texture::Builder::Target(TextureTarget target) { target_ = target; return *this; }

Texture::Builder& Texture::Builder::Dimensions(int width, int height, int depth) {
	width_ = width;
	height_ = height;
	depth_ = depth;
	return *this;
}

Texture::Builder& Texture::Builder::InternalFormat(rhi::Format format) {
	internalFormat_ = format;
	DeriveFormats();
	return *this;
}

Texture::Builder& Texture::Builder::Format(rhi::PixelFormat format) { format_ = format; return *this; }
Texture::Builder& Texture::Builder::DataType(rhi::PixelType type) { dataType_ = type; return *this; }
Texture::Builder& Texture::Builder::Data(const void* data) { data_ = data; return *this; }

Texture::Builder& Texture::Builder::FilterMode(rhi::Filter min, rhi::Filter mag) {
	minFilter_ = min;
	magFilter_ = mag;
	return *this;
}

Texture::Builder& Texture::Builder::WrapMode(rhi::Wrap all) {
	wrapS_ = wrapT_ = wrapR_ = all;
	return *this;
}

Texture::Builder& Texture::Builder::WrapMode(rhi::Wrap s, rhi::Wrap t, rhi::Wrap r) {
	wrapS_ = s;
	wrapT_ = t;
	wrapR_ = r;
	return *this;
}

Texture::Builder& Texture::Builder::BorderColor(const glm::vec4& color) {
	borderColor_ = color;
	hasBorderColor_ = true;
	return *this;
}

Texture::Builder& Texture::Builder::CompareMode(bool enabled, rhi::CompareOp op) {
	compareEnabled_ = enabled;
	compareOp_ = op;
	hasCompareMode_ = true;
	return *this;
}

Texture::Builder& Texture::Builder::GenerateMipmaps(bool generate) { generateMipmaps_ = generate; return *this; }
Texture::Builder& Texture::Builder::TextureType(enum TextureType type) { type_ = type; return *this; }
Texture::Builder& Texture::Builder::Samples(int samples) { samples_ = samples; return *this; }
Texture::Builder& Texture::Builder::FixedSampleLocations(bool fixed) { fixedSampleLocations_ = fixed; return *this; }

// Static builder helpers
Texture::Builder Texture::Builder::Texture2D(int width, int height, rhi::Format internalFormat) {
	return Builder().Target(TextureTarget::Texture2D).Dimensions(width, height).InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::Texture3D(int width, int height, int depth, rhi::Format internalFormat) {
	return Builder().Target(TextureTarget::Texture3D).Dimensions(width, height, depth).InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::TextureArray2D(int width, int height, int layers, rhi::Format internalFormat) {
	return Builder().Target(TextureTarget::Texture2DArray).Dimensions(width, height, layers).InternalFormat(internalFormat);
}

Texture::Builder Texture::Builder::TextureCube(int size, rhi::Format internalFormat) {
	return Builder().Target(TextureTarget::TextureCubeMap).Dimensions(size, size).InternalFormat(internalFormat)
		.WrapMode(rhi::Wrap::ClampToEdge);
}

Texture::Builder Texture::Builder::DepthTexture(int width, int height, bool shadow) {
	auto builder = Builder()
		.Target(TextureTarget::Texture2D)
		.Dimensions(width, height)
		.InternalFormat(rhi::Format::D24)
		.Format(rhi::PixelFormat::Depth)
		.DataType(rhi::PixelType::Float)
		.FilterMode(rhi::Filter::Nearest, rhi::Filter::Nearest)
		.WrapMode(rhi::Wrap::ClampToBorder)
		.BorderColor(glm::vec4(1.0f))
		.TextureType(TextureType::Depth);

	if (shadow) {
		builder.CompareMode(true, rhi::CompareOp::LessEqual);
	}

	return builder;
}

Texture::Builder Texture::Builder::HDRTexture(int width, int height) {
	return Builder()
		.Target(TextureTarget::Texture2D)
		.Dimensions(width, height)
		.InternalFormat(rhi::Format::RGBA16F)
		.Format(rhi::PixelFormat::RGBA)
		.DataType(rhi::PixelType::Float)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.WrapMode(rhi::Wrap::ClampToEdge)
		.TextureType(TextureType::HDR);
}

Texture::Builder Texture::Builder::MultisampleTexture(int width, int height, int samples, rhi::Format internalFormat) {
	return Builder().Target(TextureTarget::Texture2DMultisample).Dimensions(width, height).InternalFormat(internalFormat).Samples(samples);
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

	rhi::Device& gpu = rhi::Gpu();
	rhi::TextureDesc desc;
	desc.type = ToRhi(target_);
	desc.format = internalFormat_;
	desc.width = width_;
	desc.height = target_ == TextureTarget::Texture1D ? 1 : height_;
	desc.depth = (target_ == TextureTarget::Texture3D || target_ == TextureTarget::Texture2DArray ||
		target_ == TextureTarget::Texture2DMultisampleArray) ? depth_ : 1;
	desc.mipLevels = 1;   // base level; GenerateMipmaps adds the chain
	desc.samples = std::max(samples_, 1);
	desc.fixedSampleLocations = fixedSampleLocations_;
	texture->id_ = gpu.CreateTexture(desc);
	if (!texture->id_) {
		std::cerr << "[Texture::Builder] Texture creation failed" << std::endl;
		return texture;
	}

	// Initial data (cube maps are created empty).
	if (data_ && target_ != TextureTarget::TextureCubeMap && !IsMultisample(target_)) {
		gpu.UpdateTexture(texture->id_, {}, format_, dataType_, data_);
	}

	// Set texture parameters (skip for multisample)
	if (!IsMultisample(target_)) {
		gpu.SetTextureFilter(texture->id_, minFilter_, magFilter_);
		const bool usesT = target_ != TextureTarget::Texture1D;
		const bool usesR = target_ == TextureTarget::Texture3D || target_ == TextureTarget::TextureCubeMap;
		texture->wrapR_ = usesR ? wrapR_ : rhi::Wrap::Repeat;
		gpu.SetTextureWrap(texture->id_, wrapS_, usesT ? wrapT_ : rhi::Wrap::Repeat, texture->wrapR_);

		if (hasBorderColor_) {
			gpu.SetTextureBorderColor(texture->id_, borderColor_);
		}

		if (hasCompareMode_) {
			gpu.SetTextureCompare(texture->id_, compareEnabled_, compareOp_);
		}

		if (generateMipmaps_) {
			gpu.GenerateMipmaps(texture->id_);
		}
	}

	return texture;
}

void Texture::Builder::DeriveFormats() {
	using F = rhi::Format;
	using PF = rhi::PixelFormat;
	using PT = rhi::PixelType;
	// Auto-derive transfer format and data type from the internal format
	switch (internalFormat_) {
	case F::D16: case F::D24: case F::D32F:
		format_ = PF::Depth; dataType_ = PT::Float; break;
	case F::D24S8: case F::D32FS8:
		format_ = PF::DepthStencil; dataType_ = PT::Float32_UInt24_8; break;
	case F::R8: case F::R16: case F::R16F: case F::R32F:
		format_ = PF::R; dataType_ = internalFormat_ == F::R8 ? PT::UByte : PT::Float; break;
	case F::RG8: case F::RG16: case F::RG16F: case F::RG32F:
		format_ = PF::RG; dataType_ = internalFormat_ == F::RG8 ? PT::UByte : PT::Float; break;
	case F::RGB8: case F::RGB16F: case F::RGB32F: case F::SRGB8:
		format_ = PF::RGB; dataType_ = (internalFormat_ == F::RGB8 || internalFormat_ == F::SRGB8) ? PT::UByte : PT::Float; break;
	case F::RGBA8: case F::RGBA16F: case F::RGBA32F: case F::SRGB8_A8:
		format_ = PF::RGBA; dataType_ = (internalFormat_ == F::RGBA8 || internalFormat_ == F::SRGB8_A8) ? PT::UByte : PT::Float; break;
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

	TexturePtr Create2D(int width, int height, rhi::Format internalFormat, rhi::PixelFormat format, rhi::PixelType type) {
		return Texture::Builder::Texture2D(width, height, internalFormat).Format(format).DataType(type).Build();
	}

	TexturePtr Create3D(int width, int height, int depth, rhi::Format internalFormat, rhi::PixelFormat format, rhi::PixelType type) {
		return Texture::Builder::Texture3D(width, height, depth, internalFormat).Format(format).DataType(type).Build();
	}

	TexturePtr CreateArray2D(int width, int height, int layers, rhi::Format internalFormat, rhi::PixelFormat format, rhi::PixelType type) {
		return Texture::Builder::TextureArray2D(width, height, layers, internalFormat).Format(format).DataType(type).Build();
	}

	TexturePtr CreateCubemap(int size, rhi::Format internalFormat, rhi::PixelFormat format, rhi::PixelType type) {
		return Texture::Builder::TextureCube(size, internalFormat).Format(format).DataType(type).Build();
	}

	TexturePtr CreateDepth(int width, int height, bool shadow) {
		return Texture::Builder::DepthTexture(width, height, shadow).Build();
	}

	TexturePtr CreateHDR(int width, int height) {
		return Texture::Builder::HDRTexture(width, height).Build();
	}

	TexturePtr CreateMultisample(int width, int height, int samples, rhi::Format internalFormat) {
		return Texture::Builder::MultisampleTexture(width, height, samples, internalFormat).Build();
	}

	TexturePtr CreateSolidColor(const glm::vec4& color, rhi::Format internalFormat) {
		// 8-bit RGBA source data (the same bytes for RGB8 targets: only RGB is read).
		std::array<uint8_t, 4> pixelData = {
			static_cast<uint8_t>(color.r * 255.0f),
			static_cast<uint8_t>(color.g * 255.0f),
			static_cast<uint8_t>(color.b * 255.0f),
			static_cast<uint8_t>(color.a * 255.0f)
		};

		return Texture::Builder::Texture2D(1, 1, internalFormat)
			.Format(rhi::PixelFormat::RGBA)
			.DataType(rhi::PixelType::UByte)
			.Data(pixelData.data())
			.FilterMode(rhi::Filter::Nearest, rhi::Filter::Nearest)
			.Build();
	}
}
