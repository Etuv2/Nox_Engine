// FrameBuffer.cpp
#include "FrameBuffer.h"
#include "PresentTarget.h"
#include <iostream>

FrameBuffer::FrameBuffer(int width,
	int height,
	const std::vector<rhi::Format>& colorFormats,
	bool useDepthAsTexture,
	bool useDepthAsTextureArray,
	int arrayLayers,
	bool useStencil,
	rhi::Format internalDepthFormat)
	: internalColorFormats(colorFormats)
	, width(width)
	, height(height)
	, useDepthAsTexture(useDepthAsTexture)
	, useDepthAsTextureArray(useDepthAsTextureArray)
	, arrayLayers(arrayLayers)
	, useStencil(useStencil)
	, internalDepthFormat(internalDepthFormat)
{
	Init();
}

void FrameBuffer::Release() {
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	gpu.DestroyFramebuffer(fbo);
	for (rhi::TextureHandle& t : colorTextures) gpu.DestroyTexture(t);
	colorTextures.clear();
	gpu.DestroyTexture(depthTexture);
	gpu.DestroyTexture(depthTarget);
	gpu.DestroyTexture(depthArray);
}

FrameBuffer::~FrameBuffer() {
	Release();
}

FrameBuffer::FrameBuffer(FrameBuffer&& o) noexcept { *this = std::move(o); }
FrameBuffer& FrameBuffer::operator=(FrameBuffer&& o) noexcept {
	if (this != &o) {
		Release();

		fbo = o.fbo;
		colorTextures = std::move(o.colorTextures);
		internalColorFormats = std::move(o.internalColorFormats);
		depthTexture = o.depthTexture;
		depthTarget = o.depthTarget;
		depthArray = o.depthArray;
		width = o.width;
		height = o.height;
		useDepthAsTexture = o.useDepthAsTexture;
		useDepthAsTextureArray = o.useDepthAsTextureArray;
		arrayLayers = o.arrayLayers;
		useStencil = o.useStencil;
		internalDepthFormat = o.internalDepthFormat;

		o.fbo = {};
		o.depthTexture = o.depthTarget = o.depthArray = {};
	}
	return *this;
}

void FrameBuffer::Init() {
	rhi::Device& gpu = rhi::Gpu();
	rhi::FramebufferDesc desc;

	// ---- Color attachments ----
	for (rhi::Format format : internalColorFormats) {
		rhi::TextureDesc td;
		td.type = rhi::TextureType::Tex2D;
		td.format = format;
		td.width = width;
		td.height = height;
		const rhi::TextureHandle tex = gpu.CreateTexture(td);
		// Integer textures must use nearest filtering.
		const rhi::Filter filter = rhi::IsIntegerFormat(format) ? rhi::Filter::Nearest : rhi::Filter::Linear;
		gpu.SetTextureFilter(tex, filter, filter);
		gpu.SetTextureWrap(tex, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Wrap::Repeat);
		colorTextures.push_back(tex);
		desc.colors.push_back({ tex });
	}

	// ---- Depth / Stencil attachment ----
	const rhi::Format depthFormat = useStencil ? rhi::Format::D24S8 : internalDepthFormat;
	if (useDepthAsTextureArray) {
		rhi::TextureDesc td;
		td.type = rhi::TextureType::Tex2DArray;
		td.format = depthFormat;
		td.width = width;
		td.height = height;
		td.depth = arrayLayers;
		depthArray = gpu.CreateTexture(td);
		gpu.SetTextureFilter(depthArray, rhi::Filter::Nearest, rhi::Filter::Nearest);
		gpu.SetTextureWrap(depthArray, rhi::Wrap::ClampToBorder, rhi::Wrap::ClampToBorder, rhi::Wrap::Repeat);
		gpu.SetTextureBorderColor(depthArray, glm::vec4(1.0f));
		desc.depth = { depthArray, 0, rhi::kAllLayers };
	}
	else if (useDepthAsTexture) {
		rhi::TextureDesc td;
		td.type = rhi::TextureType::Tex2D;
		td.format = depthFormat;
		td.width = width;
		td.height = height;
		depthTexture = gpu.CreateTexture(td);
		gpu.SetTextureFilter(depthTexture, rhi::Filter::Nearest, rhi::Filter::Nearest);
		if (!useStencil) {
			gpu.SetTextureWrap(depthTexture, rhi::Wrap::ClampToBorder, rhi::Wrap::ClampToBorder, rhi::Wrap::Repeat);
			gpu.SetTextureBorderColor(depthTexture, glm::vec4(1.0f));
		}
		desc.depth = { depthTexture };
	}
	else {
		rhi::TextureDesc td;
		td.type = rhi::TextureType::Tex2D;
		td.format = depthFormat;
		td.width = width;
		td.height = height;
		depthTarget = gpu.CreateTexture(td);
		desc.depth = { depthTarget };
	}

	fbo = gpu.CreateFramebuffer(desc);
	if (internalColorFormats.empty()) {
		// Depth-only target: no colour output or read source.
		gpu.SetDrawBuffers(fbo, nullptr, 0);
		gpu.SetReadBuffer(fbo, -1);
	}
	std::string reason;
	if (!gpu.IsFramebufferComplete(fbo, &reason))
		std::cerr << "[FrameBuffer] Incomplete FBO: " << reason << "\n";
}

void FrameBuffer::Bind() const {
	rhi::Gpu().BindFramebuffer(fbo);
}

void FrameBuffer::Unbind() {
	rhi::Gpu().BindFramebuffer(PresentTarget::Get());
}

bool FrameBuffer::IsComplete() const {
	return rhi::Gpu().IsFramebufferComplete(fbo);
}

void FrameBuffer::Resize(int newW, int newH) {
	if (newW == width && newH == height) return;
	width = newW;
	height = newH;
	rhi::Device& gpu = rhi::Gpu();

	// Re-allocate every target in place: handles and attachments stay valid.
	for (rhi::TextureHandle t : colorTextures) {
		gpu.ResizeTexture(t, width, height, 1);
	}
	if (useDepthAsTextureArray && depthArray) {
		gpu.ResizeTexture(depthArray, width, height, arrayLayers);
	}
	else if (useDepthAsTexture && depthTexture) {
		gpu.ResizeTexture(depthTexture, width, height, 1);
	}
	else if (depthTarget) {
		gpu.ResizeTexture(depthTarget, width, height, 1);
	}
}

rhi::TextureHandle FrameBuffer::GetColorAttachment(unsigned int idx) const {
	return idx < colorTextures.size() ? colorTextures[idx] : rhi::TextureHandle{};
}
