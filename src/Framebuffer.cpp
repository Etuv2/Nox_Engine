// FrameBuffer.cpp
#include "FrameBuffer.h"
#include <iostream>

FrameBuffer::FrameBuffer(int width,
	int height,
	const std::vector<GLenum>& colorFormats,
	bool useDepthAsTexture,
	bool useDepthAsTextureArray,
	int arrayLayers,
	bool useStencil,
	GLenum internalDepthFormat)
	: fboID(0)
	, colorTextures()
	, internalColorFormats(colorFormats)
	, depthTextureID(0)
	, depthRenderbuffer(0)
	, depthArrayID(0)
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

FrameBuffer::~FrameBuffer() {
	glDeleteFramebuffers(1, &fboID);
	if (!colorTextures.empty())
		glDeleteTextures((GLsizei)colorTextures.size(), colorTextures.data());
	if (depthTextureID)    glDeleteTextures(1, &depthTextureID);
	if (depthArrayID)      glDeleteTextures(1, &depthArrayID);
	if (depthRenderbuffer) glDeleteRenderbuffers(1, &depthRenderbuffer);
}

FrameBuffer::FrameBuffer(FrameBuffer&& o) noexcept { *this = std::move(o); }
FrameBuffer& FrameBuffer::operator=(FrameBuffer&& o) noexcept {
	if (this != &o) {
		glDeleteFramebuffers(1, &fboID);
		glDeleteTextures((GLsizei)colorTextures.size(), colorTextures.data());
		glDeleteTextures(1, &depthTextureID);
		glDeleteTextures(1, &depthArrayID);
		glDeleteRenderbuffers(1, &depthRenderbuffer);

		fboID = o.fboID;
		colorTextures = std::move(o.colorTextures);
		internalColorFormats = std::move(o.internalColorFormats);
		depthTextureID = o.depthTextureID;
		depthRenderbuffer = o.depthRenderbuffer;
		depthArrayID = o.depthArrayID;
		width = o.width;
		height = o.height;
		useDepthAsTexture = o.useDepthAsTexture;
		useDepthAsTextureArray = o.useDepthAsTextureArray;
		arrayLayers = o.arrayLayers;
		useStencil = o.useStencil;
		internalDepthFormat = o.internalDepthFormat;

		o.fboID = o.depthTextureID = o.depthArrayID = o.depthRenderbuffer = 0;
	}
	return *this;
}

static inline void SelectDepthExternalFormatAndType(GLenum internalFmt, bool useStencil,
	GLenum& outFormat, GLenum& outType)
{
	if (useStencil) {
		outFormat = GL_DEPTH_STENCIL;
		outType = GL_UNSIGNED_INT_24_8;
		return;
	}
	outFormat = GL_DEPTH_COMPONENT;
	switch (internalFmt) {
	case GL_DEPTH_COMPONENT32F:
		outType = GL_FLOAT;
		break;
	case GL_DEPTH_COMPONENT24:
		outType = GL_UNSIGNED_INT;
		break;
	case GL_DEPTH_COMPONENT16:
		outType = GL_UNSIGNED_SHORT;
		break;
	default:
		// Fallback to 24-bit compatible type
		outType = GL_UNSIGNED_INT;
		break;
	}
}

void FrameBuffer::Init() {
	glGenFramebuffers(1, &fboID);
	glBindFramebuffer(GL_FRAMEBUFFER, fboID);

	// ---- Color attachments ----
	int nColor = (int)internalColorFormats.size();
	if (nColor > 0) {
		colorTextures.resize(nColor);
		glGenTextures(nColor, colorTextures.data());
		for (int i = 0; i < nColor; ++i) {
			GLenum ifmt = internalColorFormats[i];
			GLenum fmt, type;
			// Select upload format + type based on internal format
			switch (ifmt) {
				// 16-bit float types
			case GL_RGBA16F:
			case GL_RGB16F:
			case GL_RG16F:
			case GL_R16F:
				fmt = (ifmt == GL_R16F ? GL_RED
					: ifmt == GL_RG16F ? GL_RG
					: ifmt == GL_RGB16F ? GL_RGB
					: GL_RGBA);
				type = GL_HALF_FLOAT;
				break;
				// 32-bit float types
			case GL_RGBA32F:
			case GL_RGB32F:
			case GL_RG32F:
			case GL_R32F:
				fmt = (ifmt == GL_R32F ? GL_RED
					: ifmt == GL_RG32F ? GL_RG
					: ifmt == GL_RGB32F ? GL_RGB
					: GL_RGBA);
				type = GL_FLOAT;
				break;
				// Integer types (Support for material ID)
			case GL_R8UI:
			case GL_RG8UI:
			case GL_RGB8UI:
			case GL_RGBA8UI:
				fmt = (ifmt == GL_R8UI ? GL_RED_INTEGER
					: ifmt == GL_RG8UI ? GL_RG_INTEGER
					: ifmt == GL_RGB8UI ? GL_RGB_INTEGER
					: GL_RGBA_INTEGER);
				type = GL_UNSIGNED_BYTE;
				break;
			case GL_R16UI:
			case GL_RG16UI:
			case GL_RGB16UI:
			case GL_RGBA16UI:
				fmt = (ifmt == GL_R16UI ? GL_RED_INTEGER
					: ifmt == GL_RG16UI ? GL_RG_INTEGER
					: ifmt == GL_RGB16UI ? GL_RGB_INTEGER
					: GL_RGBA_INTEGER);
				type = GL_UNSIGNED_SHORT;
				break;
			case GL_R32UI:
			case GL_RG32UI:
			case GL_RGB32UI:
			case GL_RGBA32UI:
				fmt = (ifmt == GL_R32UI ? GL_RED_INTEGER
					: ifmt == GL_RG32UI ? GL_RG_INTEGER
					: ifmt == GL_RGB32UI ? GL_RGB_INTEGER
					: GL_RGBA_INTEGER);
				type = GL_UNSIGNED_INT;
				break;
				// default to normalized unsigned byte
			default:
				fmt = (ifmt == GL_R8 ? GL_RED
					: ifmt == GL_RG8 ? GL_RG
					: ifmt == GL_RGB8 ? GL_RGB
					: GL_RGBA);
				type = GL_UNSIGNED_BYTE;
			}

			glBindTexture(GL_TEXTURE_2D, colorTextures[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, ifmt, width, height, 0, fmt, type, nullptr);

			// Filtering and wrapping
			// Integer textures MUST use NEAREST filtering (not LINEAR)
			if (ifmt >= GL_R8UI && ifmt <= GL_RGBA32UI) {
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			} else {
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			}
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glFramebufferTexture2D(
				GL_FRAMEBUFFER,
				GL_COLOR_ATTACHMENT0 + i,
				GL_TEXTURE_2D,
				colorTextures[i], 0
			);
		}
		std::vector<GLenum> drawBufs(nColor);
		for (int i = 0; i < nColor; ++i)
			drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
		glDrawBuffers(nColor, drawBufs.data());
	}
	else {
		glDrawBuffer(GL_NONE);
		glReadBuffer(GL_NONE);
	}

	// ---- Depth / Stencil attachments ----
	if (useDepthAsTextureArray) {
		glGenTextures(1, &depthArrayID);
		glBindTexture(GL_TEXTURE_2D_ARRAY, depthArrayID);
		GLenum dfmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		GLenum fmt, type;
		SelectDepthExternalFormatAndType(dfmt, useStencil, fmt, type);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, dfmt,
			width, height, arrayLayers, 0,
			fmt, type, nullptr);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		float border[4] = { 1,1,1,1 };
		glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);

		glFramebufferTexture(
			GL_FRAMEBUFFER,
			useStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
			depthArrayID, 0
		);
	}
	else if (useDepthAsTexture) {
		glGenTextures(1, &depthTextureID);
		glBindTexture(GL_TEXTURE_2D, depthTextureID);
		GLenum dfmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		GLenum atch = useStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
		GLenum fmt, type;
		SelectDepthExternalFormatAndType(dfmt, useStencil, fmt, type);
		glTexImage2D(GL_TEXTURE_2D, 0, dfmt, width, height, 0, fmt, type, nullptr);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (!useStencil) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
			float border[4] = { 1,1,1,1 };
			glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
		}
		glFramebufferTexture2D(GL_FRAMEBUFFER, atch, GL_TEXTURE_2D, depthTextureID, 0);
	}
	else {
		glGenRenderbuffers(1, &depthRenderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
		GLenum fmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		glRenderbufferStorage(GL_RENDERBUFFER, fmt, width, height);
		glFramebufferRenderbuffer(
			GL_FRAMEBUFFER,
			useStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
			GL_RENDERBUFFER,
			depthRenderbuffer
		);
	}

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		std::cerr << "[FrameBuffer] Incomplete FBO!\n";

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FrameBuffer::Bind() const {
	glBindFramebuffer(GL_FRAMEBUFFER, fboID);
}
void FrameBuffer::Unbind() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
bool FrameBuffer::IsComplete() const {
	glBindFramebuffer(GL_FRAMEBUFFER, fboID);
	bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return ok;
}

void FrameBuffer::Resize(int newW, int newH) {
	if (newW == width && newH == height) return;
	width = newW;
	height = newH;
	glBindFramebuffer(GL_FRAMEBUFFER, fboID);

	// Resize color attachments
	for (size_t i = 0; i < colorTextures.size(); ++i) {
		GLenum ifmt = internalColorFormats[i];
		GLenum fmt, type;
		switch (ifmt) {
		case GL_RGBA16F: case GL_RGB16F: case GL_RG16F: case GL_R16F:
			fmt = (ifmt == GL_R16F ? GL_RED
				: ifmt == GL_RG16F ? GL_RG
				: ifmt == GL_RGB16F ? GL_RGB
				: GL_RGBA);
			type = GL_HALF_FLOAT;
			break;
		case GL_RGBA32F: case GL_RGB32F: case GL_RG32F: case GL_R32F:
			fmt = (ifmt == GL_R32F ? GL_RED
				: ifmt == GL_RG32F ? GL_RG
				: ifmt == GL_RGB32F ? GL_RGB
				: GL_RGBA);
			type = GL_FLOAT;
			break;
		// Integer types (Support for material ID)
		case GL_R8UI: case GL_RG8UI: case GL_RGB8UI: case GL_RGBA8UI:
			fmt = (ifmt == GL_R8UI ? GL_RED_INTEGER
				: ifmt == GL_RG8UI ? GL_RG_INTEGER
				: ifmt == GL_RGB8UI ? GL_RGB_INTEGER
				: GL_RGBA_INTEGER);
			type = GL_UNSIGNED_BYTE;
			break;
		case GL_R16UI: case GL_RG16UI: case GL_RGB16UI: case GL_RGBA16UI:
			fmt = (ifmt == GL_R16UI ? GL_RED_INTEGER
				: ifmt == GL_RG16UI ? GL_RG_INTEGER
				: ifmt == GL_RGB16UI ? GL_RGB_INTEGER
				: GL_RGBA_INTEGER);
			type = GL_UNSIGNED_SHORT;
			break;
		case GL_R32UI: case GL_RG32UI: case GL_RGB32UI: case GL_RGBA32UI:
			fmt = (ifmt == GL_R32UI ? GL_RED_INTEGER
				: ifmt == GL_RG32UI ? GL_RG_INTEGER
				: ifmt == GL_RGB32UI ? GL_RGB_INTEGER
				: GL_RGBA_INTEGER);
			type = GL_UNSIGNED_INT;
			break;
		default:
			fmt = (ifmt == GL_R8 ? GL_RED
				: ifmt == GL_RG8 ? GL_RG
				: ifmt == GL_RGB8 ? GL_RGB
				: GL_RGBA);
			type = GL_UNSIGNED_BYTE;
		}
		glBindTexture(GL_TEXTURE_2D, colorTextures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, ifmt, width, height, 0, fmt, type, nullptr);
	}

	// Resize depth-as-2D-array if present
	if (useDepthAsTextureArray && depthArrayID) {
		glBindTexture(GL_TEXTURE_2D_ARRAY, depthArrayID);
		GLenum dfmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		GLenum dataFmt, dataType;
		SelectDepthExternalFormatAndType(dfmt, useStencil, dataFmt, dataType);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, dfmt, width, height, arrayLayers, 0, dataFmt, dataType, nullptr);
	}
	// Resize depth-as-2D-texture if present
	else if (useDepthAsTexture && depthTextureID) {
		glBindTexture(GL_TEXTURE_2D, depthTextureID);
		GLenum dfmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		GLenum dataFmt, dataType;
		SelectDepthExternalFormatAndType(dfmt, useStencil, dataFmt, dataType);
		glTexImage2D(GL_TEXTURE_2D, 0, dfmt, width, height, 0, dataFmt, dataType, nullptr);
	}
	// Otherwise resize renderbuffer
	else if (depthRenderbuffer) {
		glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
		GLenum fmt = useStencil ? GL_DEPTH24_STENCIL8 : internalDepthFormat;
		glRenderbufferStorage(GL_RENDERBUFFER, fmt, width, height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


GLuint FrameBuffer::GetColorAttachment(unsigned int idx) const {
	return idx < colorTextures.size() ? colorTextures[idx] : 0;
}
