#include "GLDevice.h"

#include <GL/glew.h>
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace rhi::gl {
namespace {

	// ------------------------------------------------------------------------------------------------
	// Enum translation
	// ------------------------------------------------------------------------------------------------
	GLenum ToGL(Format f) {
		switch (f) {
		case Format::R8: return GL_R8; case Format::R8UI: return GL_R8UI; case Format::R16: return GL_R16;
		case Format::R16F: return GL_R16F; case Format::R16UI: return GL_R16UI; case Format::R32F: return GL_R32F;
		case Format::R32I: return GL_R32I; case Format::R32UI: return GL_R32UI;
		case Format::RG8: return GL_RG8; case Format::RG8UI: return GL_RG8UI; case Format::RG16: return GL_RG16;
		case Format::RG16F: return GL_RG16F; case Format::RG16UI: return GL_RG16UI; case Format::RG32F: return GL_RG32F;
		case Format::RG32UI: return GL_RG32UI;
		case Format::RGB8: return GL_RGB8; case Format::RGB8UI: return GL_RGB8UI; case Format::RGB16F: return GL_RGB16F;
		case Format::RGB16UI: return GL_RGB16UI; case Format::RGB32F: return GL_RGB32F; case Format::RGB32UI: return GL_RGB32UI;
		case Format::R11G11B10F: return GL_R11F_G11F_B10F;
		case Format::RGBA8: return GL_RGBA8; case Format::RGBA8UI: return GL_RGBA8UI; case Format::RGBA16: return GL_RGBA16;
		case Format::RGBA16F: return GL_RGBA16F; case Format::RGBA16UI: return GL_RGBA16UI; case Format::RGBA32F: return GL_RGBA32F;
		case Format::RGBA32UI: return GL_RGBA32UI; case Format::RGB10A2: return GL_RGB10_A2;
		case Format::SRGB8: return GL_SRGB8; case Format::SRGB8_A8: return GL_SRGB8_ALPHA8;
		case Format::D16: return GL_DEPTH_COMPONENT16; case Format::D24: return GL_DEPTH_COMPONENT24;
		case Format::D32F: return GL_DEPTH_COMPONENT32F; case Format::D24S8: return GL_DEPTH24_STENCIL8;
		case Format::D32FS8: return GL_DEPTH32F_STENCIL8;
		case Format::Unknown: break;
		}
		return GL_RGBA8;
	}

	Format FromGLInternal(GLint f) {
		switch (f) {
		case GL_R8: return Format::R8; case GL_R8UI: return Format::R8UI; case GL_R16: return Format::R16;
		case GL_R16F: return Format::R16F; case GL_R16UI: return Format::R16UI; case GL_R32F: return Format::R32F;
		case GL_R32I: return Format::R32I; case GL_R32UI: return Format::R32UI;
		case GL_RG8: return Format::RG8; case GL_RG8UI: return Format::RG8UI; case GL_RG16: return Format::RG16;
		case GL_RG16F: return Format::RG16F; case GL_RG16UI: return Format::RG16UI; case GL_RG32F: return Format::RG32F;
		case GL_RG32UI: return Format::RG32UI;
		case GL_RGB8: return Format::RGB8; case GL_RGB8UI: return Format::RGB8UI; case GL_RGB16F: return Format::RGB16F;
		case GL_RGB16UI: return Format::RGB16UI; case GL_RGB32F: return Format::RGB32F; case GL_RGB32UI: return Format::RGB32UI;
		case GL_R11F_G11F_B10F: return Format::R11G11B10F;
		case GL_RGBA8: return Format::RGBA8; case GL_RGBA8UI: return Format::RGBA8UI; case GL_RGBA16: return Format::RGBA16;
		case GL_RGBA16F: return Format::RGBA16F; case GL_RGBA16UI: return Format::RGBA16UI; case GL_RGBA32F: return Format::RGBA32F;
		case GL_RGBA32UI: return Format::RGBA32UI; case GL_RGB10_A2: return Format::RGB10A2;
		case GL_SRGB8: return Format::SRGB8; case GL_SRGB8_ALPHA8: return Format::SRGB8_A8;
		case GL_DEPTH_COMPONENT16: return Format::D16; case GL_DEPTH_COMPONENT24: return Format::D24;
		case GL_DEPTH_COMPONENT32F: return Format::D32F; case GL_DEPTH24_STENCIL8: return Format::D24S8;
		case GL_DEPTH32F_STENCIL8: return Format::D32FS8;
		default: return Format::Unknown;
		}
	}

	GLenum ToGL(PixelFormat f) {
		switch (f) {
		case PixelFormat::R: return GL_RED; case PixelFormat::RG: return GL_RG; case PixelFormat::RGB: return GL_RGB;
		case PixelFormat::RGBA: return GL_RGBA; case PixelFormat::BGRA: return GL_BGRA; case PixelFormat::RInt: return GL_RED_INTEGER;
		case PixelFormat::RGInt: return GL_RG_INTEGER; case PixelFormat::RGBInt: return GL_RGB_INTEGER;
		case PixelFormat::RGBAInt: return GL_RGBA_INTEGER; case PixelFormat::Depth: return GL_DEPTH_COMPONENT;
		case PixelFormat::DepthStencil: return GL_DEPTH_STENCIL;
		}
		return GL_RGBA;
	}

	GLenum ToGL(PixelType t) {
		switch (t) {
		case PixelType::UByte: return GL_UNSIGNED_BYTE; case PixelType::Byte: return GL_BYTE;
		case PixelType::UShort: return GL_UNSIGNED_SHORT; case PixelType::Short: return GL_SHORT;
		case PixelType::UInt: return GL_UNSIGNED_INT; case PixelType::Int: return GL_INT;
		case PixelType::HalfFloat: return GL_HALF_FLOAT; case PixelType::Float: return GL_FLOAT;
		case PixelType::UInt24_8: return GL_UNSIGNED_INT_24_8; case PixelType::Float32_UInt24_8: return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
		}
		return GL_UNSIGNED_BYTE;
	}

	GLenum ToGL(TextureType t) {
		switch (t) {
		case TextureType::Tex1D: return GL_TEXTURE_1D; case TextureType::Tex1DArray: return GL_TEXTURE_1D_ARRAY;
		case TextureType::Tex2D: return GL_TEXTURE_2D; case TextureType::Tex2DArray: return GL_TEXTURE_2D_ARRAY;
		case TextureType::Tex3D: return GL_TEXTURE_3D; case TextureType::Cube: return GL_TEXTURE_CUBE_MAP;
		case TextureType::CubeArray: return GL_TEXTURE_CUBE_MAP_ARRAY; case TextureType::Tex2DMS: return GL_TEXTURE_2D_MULTISAMPLE;
		case TextureType::Tex2DMSArray: return GL_TEXTURE_2D_MULTISAMPLE_ARRAY; case TextureType::Rectangle: return GL_TEXTURE_RECTANGLE;
		}
		return GL_TEXTURE_2D;
	}

	GLint ToGL(Filter f) {
		switch (f) {
		case Filter::Nearest: return GL_NEAREST; case Filter::Linear: return GL_LINEAR;
		case Filter::NearestMipNearest: return GL_NEAREST_MIPMAP_NEAREST; case Filter::LinearMipNearest: return GL_LINEAR_MIPMAP_NEAREST;
		case Filter::NearestMipLinear: return GL_NEAREST_MIPMAP_LINEAR; case Filter::LinearMipLinear: return GL_LINEAR_MIPMAP_LINEAR;
		}
		return GL_LINEAR;
	}

	GLint ToGL(Wrap w) {
		switch (w) {
		case Wrap::Repeat: return GL_REPEAT; case Wrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
		case Wrap::ClampToEdge: return GL_CLAMP_TO_EDGE; case Wrap::ClampToBorder: return GL_CLAMP_TO_BORDER;
		}
		return GL_REPEAT;
	}

	GLenum ToGL(CompareOp op) {
		switch (op) {
		case CompareOp::Never: return GL_NEVER; case CompareOp::Less: return GL_LESS; case CompareOp::Equal: return GL_EQUAL;
		case CompareOp::LessEqual: return GL_LEQUAL; case CompareOp::Greater: return GL_GREATER; case CompareOp::NotEqual: return GL_NOTEQUAL;
		case CompareOp::GreaterEqual: return GL_GEQUAL; case CompareOp::Always: return GL_ALWAYS;
		}
		return GL_LESS;
	}

	CompareOp FromGLCompare(GLint v) {
		switch (v) {
		case GL_NEVER: return CompareOp::Never; case GL_LESS: return CompareOp::Less; case GL_EQUAL: return CompareOp::Equal;
		case GL_LEQUAL: return CompareOp::LessEqual; case GL_GREATER: return CompareOp::Greater; case GL_NOTEQUAL: return CompareOp::NotEqual;
		case GL_GEQUAL: return CompareOp::GreaterEqual; default: return CompareOp::Always;
		}
	}

	GLenum ToGL(BlendFactor f) {
		switch (f) {
		case BlendFactor::Zero: return GL_ZERO; case BlendFactor::One: return GL_ONE;
		case BlendFactor::SrcColor: return GL_SRC_COLOR; case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
		case BlendFactor::DstColor: return GL_DST_COLOR; case BlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
		case BlendFactor::SrcAlpha: return GL_SRC_ALPHA; case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
		case BlendFactor::DstAlpha: return GL_DST_ALPHA; case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
		case BlendFactor::ConstantColor: return GL_CONSTANT_COLOR; case BlendFactor::OneMinusConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
		case BlendFactor::SrcAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
		case BlendFactor::Src1Color: return GL_SRC1_COLOR; case BlendFactor::OneMinusSrc1Color: return GL_ONE_MINUS_SRC1_COLOR;
		}
		return GL_ONE;
	}

	BlendFactor FromGLBlend(GLint v) {
		switch (v) {
		case GL_ZERO: return BlendFactor::Zero; case GL_ONE: return BlendFactor::One;
		case GL_SRC_COLOR: return BlendFactor::SrcColor; case GL_ONE_MINUS_SRC_COLOR: return BlendFactor::OneMinusSrcColor;
		case GL_DST_COLOR: return BlendFactor::DstColor; case GL_ONE_MINUS_DST_COLOR: return BlendFactor::OneMinusDstColor;
		case GL_SRC_ALPHA: return BlendFactor::SrcAlpha; case GL_ONE_MINUS_SRC_ALPHA: return BlendFactor::OneMinusSrcAlpha;
		case GL_DST_ALPHA: return BlendFactor::DstAlpha; case GL_ONE_MINUS_DST_ALPHA: return BlendFactor::OneMinusDstAlpha;
		case GL_CONSTANT_COLOR: return BlendFactor::ConstantColor; case GL_ONE_MINUS_CONSTANT_COLOR: return BlendFactor::OneMinusConstantColor;
		case GL_SRC_ALPHA_SATURATE: return BlendFactor::SrcAlphaSaturate;
		case GL_SRC1_COLOR: return BlendFactor::Src1Color; case GL_ONE_MINUS_SRC1_COLOR: return BlendFactor::OneMinusSrc1Color;
		default: return BlendFactor::One;
		}
	}

	GLenum ToGL(BlendOp op) {
		switch (op) {
		case BlendOp::Add: return GL_FUNC_ADD; case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
		case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT; case BlendOp::Min: return GL_MIN; case BlendOp::Max: return GL_MAX;
		}
		return GL_FUNC_ADD;
	}

	BlendOp FromGLBlendOp(GLint v) {
		switch (v) {
		case GL_FUNC_SUBTRACT: return BlendOp::Subtract; case GL_FUNC_REVERSE_SUBTRACT: return BlendOp::ReverseSubtract;
		case GL_MIN: return BlendOp::Min; case GL_MAX: return BlendOp::Max; default: return BlendOp::Add;
		}
	}

	GLenum ToGL(CullFace f) {
		switch (f) { case CullFace::Front: return GL_FRONT; case CullFace::Back: return GL_BACK; case CullFace::FrontAndBack: return GL_FRONT_AND_BACK; }
		return GL_BACK;
	}

	GLenum ToGL(PolygonMode m) {
		switch (m) { case PolygonMode::Fill: return GL_FILL; case PolygonMode::Line: return GL_LINE; case PolygonMode::Point: return GL_POINT; }
		return GL_FILL;
	}

	GLenum ToGL(Topology t) {
		switch (t) {
		case Topology::Points: return GL_POINTS; case Topology::Lines: return GL_LINES; case Topology::LineStrip: return GL_LINE_STRIP;
		case Topology::LineLoop: return GL_LINE_LOOP; case Topology::Triangles: return GL_TRIANGLES;
		case Topology::TriangleStrip: return GL_TRIANGLE_STRIP; case Topology::TriangleFan: return GL_TRIANGLE_FAN;
		}
		return GL_TRIANGLES;
	}

	GLenum ToGL(IndexType t) {
		switch (t) { case IndexType::UInt8: return GL_UNSIGNED_BYTE; case IndexType::UInt16: return GL_UNSIGNED_SHORT; case IndexType::UInt32: return GL_UNSIGNED_INT; }
		return GL_UNSIGNED_INT;
	}

	GLenum ToGL(ScalarType t) {
		switch (t) {
		case ScalarType::Float: return GL_FLOAT; case ScalarType::HalfFloat: return GL_HALF_FLOAT; case ScalarType::Int: return GL_INT;
		case ScalarType::UInt: return GL_UNSIGNED_INT; case ScalarType::Short: return GL_SHORT; case ScalarType::UShort: return GL_UNSIGNED_SHORT;
		case ScalarType::Byte: return GL_BYTE; case ScalarType::UByte: return GL_UNSIGNED_BYTE;
		}
		return GL_FLOAT;
	}

	uint32_t ScalarSize(ScalarType t) {
		switch (t) {
		case ScalarType::Float: case ScalarType::Int: case ScalarType::UInt: return 4;
		case ScalarType::HalfFloat: case ScalarType::Short: case ScalarType::UShort: return 2;
		case ScalarType::Byte: case ScalarType::UByte: return 1;
		}
		return 4;
	}

	GLenum ToGL(BufferUsage u) {
		switch (u) {
		case BufferUsage::StaticDraw: return GL_STATIC_DRAW; case BufferUsage::StaticRead: return GL_STATIC_READ;
		case BufferUsage::StaticCopy: return GL_STATIC_COPY; case BufferUsage::DynamicDraw: return GL_DYNAMIC_DRAW;
		case BufferUsage::DynamicRead: return GL_DYNAMIC_READ; case BufferUsage::DynamicCopy: return GL_DYNAMIC_COPY;
		case BufferUsage::StreamDraw: return GL_STREAM_DRAW; case BufferUsage::StreamRead: return GL_STREAM_READ;
		case BufferUsage::StreamCopy: return GL_STREAM_COPY;
		}
		return GL_STATIC_DRAW;
	}

	GLbitfield ToGL(BufferStorage s) {
		GLbitfield b = 0;
		if (s & BufferStorage::DynamicUpdate) b |= GL_DYNAMIC_STORAGE_BIT;
		if (s & BufferStorage::MapRead) b |= GL_MAP_READ_BIT;
		if (s & BufferStorage::MapWrite) b |= GL_MAP_WRITE_BIT;
		if (s & BufferStorage::MapPersistent) b |= GL_MAP_PERSISTENT_BIT;
		if (s & BufferStorage::MapCoherent) b |= GL_MAP_COHERENT_BIT;
		if (s & BufferStorage::ClientStorage) b |= GL_CLIENT_STORAGE_BIT;
		return b;
	}

	GLbitfield ToGL(MapAccess a) {
		GLbitfield b = 0;
		if (a & MapAccess::Read) b |= GL_MAP_READ_BIT;
		if (a & MapAccess::Write) b |= GL_MAP_WRITE_BIT;
		if (a & MapAccess::Persistent) b |= GL_MAP_PERSISTENT_BIT;
		if (a & MapAccess::Coherent) b |= GL_MAP_COHERENT_BIT;
		if (a & MapAccess::InvalidateRange) b |= GL_MAP_INVALIDATE_RANGE_BIT;
		if (a & MapAccess::InvalidateBuffer) b |= GL_MAP_INVALIDATE_BUFFER_BIT;
		if (a & MapAccess::FlushExplicit) b |= GL_MAP_FLUSH_EXPLICIT_BIT;
		if (a & MapAccess::Unsynchronized) b |= GL_MAP_UNSYNCHRONIZED_BIT;
		return b;
	}

	GLbitfield ToGLBarriers(uint32_t b) {
		if (b == BarrierAll) return GL_ALL_BARRIER_BITS;
		GLbitfield g = 0;
		if (b & BarrierVertexAttrib) g |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
		if (b & BarrierIndexBuffer) g |= GL_ELEMENT_ARRAY_BARRIER_BIT;
		if (b & BarrierUniform) g |= GL_UNIFORM_BARRIER_BIT;
		if (b & BarrierTextureFetch) g |= GL_TEXTURE_FETCH_BARRIER_BIT;
		if (b & BarrierImageAccess) g |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
		if (b & BarrierCommand) g |= GL_COMMAND_BARRIER_BIT;
		if (b & BarrierPixelBuffer) g |= GL_PIXEL_BUFFER_BARRIER_BIT;
		if (b & BarrierTextureUpdate) g |= GL_TEXTURE_UPDATE_BARRIER_BIT;
		if (b & BarrierBufferUpdate) g |= GL_BUFFER_UPDATE_BARRIER_BIT;
		if (b & BarrierFramebuffer) g |= GL_FRAMEBUFFER_BARRIER_BIT;
		if (b & BarrierStorage) g |= GL_SHADER_STORAGE_BARRIER_BIT;
		if (b & BarrierAtomicCounter) g |= GL_ATOMIC_COUNTER_BARRIER_BIT;
		return g;
	}

	GLenum ToGL(ShaderStage s) {
		switch (s) {
		case ShaderStage::Vertex: return GL_VERTEX_SHADER; case ShaderStage::Fragment: return GL_FRAGMENT_SHADER;
		case ShaderStage::Geometry: return GL_GEOMETRY_SHADER; case ShaderStage::TessControl: return GL_TESS_CONTROL_SHADER;
		case ShaderStage::TessEvaluation: return GL_TESS_EVALUATION_SHADER; case ShaderStage::Compute: return GL_COMPUTE_SHADER;
		}
		return GL_VERTEX_SHADER;
	}

	GLenum ToGL(ImageAccess a) {
		switch (a) { case ImageAccess::ReadOnly: return GL_READ_ONLY; case ImageAccess::WriteOnly: return GL_WRITE_ONLY; case ImageAccess::ReadWrite: return GL_READ_WRITE; }
		return GL_READ_WRITE;
	}

	GLenum ToGL(Feature f) {
		switch (f) {
		case Feature::ProgramPointSize: return GL_PROGRAM_POINT_SIZE; case Feature::DepthClamp: return GL_DEPTH_CLAMP;
		case Feature::FramebufferSRGB: return GL_FRAMEBUFFER_SRGB; case Feature::SeamlessCubemaps: return GL_TEXTURE_CUBE_MAP_SEAMLESS;
		case Feature::StencilTest: return GL_STENCIL_TEST; case Feature::Multisample: return GL_MULTISAMPLE;
		}
		return GL_PROGRAM_POINT_SIZE;
	}

	GLenum ToGLAttachment(AttachmentPoint p) {
		switch (p) {
		case AttachmentPoint::Depth: return GL_DEPTH_ATTACHMENT;
		case AttachmentPoint::Stencil: return GL_STENCIL_ATTACHMENT;
		case AttachmentPoint::DepthStencil: return GL_DEPTH_STENCIL_ATTACHMENT;
		default: return GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(p);
		}
	}

	GLbitfield ToGLClear(uint32_t flags) {
		GLbitfield b = 0;
		if (flags & ClearColor) b |= GL_COLOR_BUFFER_BIT;
		if (flags & ClearDepth) b |= GL_DEPTH_BUFFER_BIT;
		if (flags & ClearStencil) b |= GL_STENCIL_BUFFER_BIT;
		return b;
	}

	int MipCount(int w, int h, int d) {
		int levels = 1;
		int m = std::max({ w, h, d, 1 });
		while (m > 1) { m >>= 1; ++levels; }
		return levels;
	}

	bool IsMultisample(TextureType t) { return t == TextureType::Tex2DMS || t == TextureType::Tex2DMSArray; }

	// Tightly packed bytes per pixel of CPU-side data (row padding is handled by the unpack alignment).
	size_t BytesPerPixel(PixelFormat f, PixelType t) {
		size_t channels = 4;
		switch (f) {
		case PixelFormat::R: case PixelFormat::RInt: case PixelFormat::Depth: channels = 1; break;
		case PixelFormat::RG: case PixelFormat::RGInt: case PixelFormat::DepthStencil: channels = 2; break;
		case PixelFormat::RGB: case PixelFormat::RGBInt: channels = 3; break;
		default: channels = 4; break;
		}
		switch (t) {
		case PixelType::UByte: case PixelType::Byte: return channels;
		case PixelType::UShort: case PixelType::Short: case PixelType::HalfFloat: return channels * 2;
		case PixelType::UInt24_8: return 4;
		case PixelType::Float32_UInt24_8: return 8;
		default: return channels * 4;
		}
	}

	GLenum BindingQuery(GLenum target) {
		switch (target) {
		case GL_TEXTURE_1D: return GL_TEXTURE_BINDING_1D; case GL_TEXTURE_1D_ARRAY: return GL_TEXTURE_BINDING_1D_ARRAY;
		case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D; case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
		case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D; case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
		case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
		case GL_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
		case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
		case GL_TEXTURE_RECTANGLE: return GL_TEXTURE_BINDING_RECTANGLE;
		default: return GL_TEXTURE_BINDING_2D;
		}
	}

	// Pixel transfer format/type compatible with an internal format (allocation without data).
	void TransferFor(Format f, GLenum& fmt, GLenum& type) {
		if (f == Format::D24S8) { fmt = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; return; }
		if (f == Format::D32FS8) { fmt = GL_DEPTH_STENCIL; type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; return; }
		if (IsDepthFormat(f)) { fmt = GL_DEPTH_COMPONENT; type = f == Format::D32F ? GL_FLOAT : GL_UNSIGNED_INT; return; }
		const uint32_t n = FormatChannelCount(f);
		if (IsIntegerFormat(f)) {
			fmt = n == 1 ? GL_RED_INTEGER : n == 2 ? GL_RG_INTEGER : n == 3 ? GL_RGB_INTEGER : GL_RGBA_INTEGER;
			type = f == Format::R32I ? GL_INT : GL_UNSIGNED_INT;
			return;
		}
		fmt = n == 1 ? GL_RED : n == 2 ? GL_RG : n == 3 ? GL_RGB : GL_RGBA;
		type = GL_FLOAT;
	}

	// ------------------------------------------------------------------------------------------------
	// Device
	// ------------------------------------------------------------------------------------------------
	class GLDevice final : public Device {
		struct TextureRecord { GLenum target = GL_TEXTURE_2D; bool immutable = false; TextureInfo info; };
		struct VertexInputRecord { VertexInputDesc desc; };

	public:
		GLDevice(SDL_Window* window, SDL_GLContext context) : m_window(window), m_context(context) {}
		~GLDevice() override {
			if (m_context) SDL_GL_DeleteContext(m_context);
		}

		bool InitAPI(bool vsync) {
			SDL_GL_SetSwapInterval(vsync ? 1 : 0);
			glewExperimental = GL_TRUE;
			const GLenum err = glewInit();
			glGetError(); // glewInit can leave a spurious GL_INVALID_ENUM
			if (err != GLEW_OK) {
				std::cerr << "[RHI/GL] glewInit failed: " << glewGetErrorString(err) << std::endl;
				return false;
			}
			auto str = [](GLenum name) { const GLubyte* s = glGetString(name); return s ? std::string(reinterpret_cast<const char*>(s)) : std::string(); };
			m_info.vendor = str(GL_VENDOR);
			m_info.renderer = str(GL_RENDERER);
			m_info.version = str(GL_VERSION);
			m_info.shadingLanguage = str(GL_SHADING_LANGUAGE_VERSION);
			glGetIntegerv(GL_MAJOR_VERSION, &m_info.apiMajor);
			glGetIntegerv(GL_MINOR_VERSION, &m_info.apiMinor);
			m_info.backend = "OpenGL " + std::to_string(m_info.apiMajor) + "." + std::to_string(m_info.apiMinor);

			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_limits.maxTextureSize);
			glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &m_limits.maxTextureUnits);
			glGetIntegerv(GL_MAX_IMAGE_UNITS, &m_limits.maxImageUnits);
			glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &m_limits.maxColorAttachments);
			glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &m_limits.maxStorageBufferBindings);
			for (int i = 0; i < 3; ++i) {
				glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &m_limits.maxComputeWorkGroupCount[i]);
				glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &m_limits.maxComputeWorkGroupSize[i]);
			}
			glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &m_limits.maxComputeWorkGroupInvocations);
			glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &m_limits.maxComputeSharedMemory);
			m_limits.anisotropicFiltering = GLEW_EXT_texture_filter_anisotropic || GLEW_ARB_texture_filter_anisotropic;
			if (m_limits.anisotropicFiltering) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m_limits.maxAnisotropy);
			m_limits.pipelineStatisticsQueries = GLEW_ARB_pipeline_statistics_query != 0;
			m_debugLabels = GLEW_KHR_debug != 0;
			// NOX_GL_DEBUG=1: synchronous driver messages (errors, undefined behaviour) on stderr.
			if (const char* dbg = std::getenv("NOX_GL_DEBUG"); dbg && dbg[0] == '1' && m_debugLabels) {
				glEnable(GL_DEBUG_OUTPUT);
				glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
				glDebugMessageCallback(&GLDevice::DebugCallback, nullptr);
				glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
				std::cout << "[RHI/GL] Debug output enabled (NOX_GL_DEBUG)" << std::endl;
			}
			glGetError();
			return true;
		}

		static void GLAPIENTRY DebugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
			static int count = 0;
			if (++count > 400) return;   // a broken frame repeats the same messages every frame
			const char* sev = severity == GL_DEBUG_SEVERITY_HIGH ? "HIGH" : severity == GL_DEBUG_SEVERITY_MEDIUM ? "MEDIUM" : "LOW";
			const char* kind = type == GL_DEBUG_TYPE_ERROR ? "error" : type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR ? "undefined" :
				type == GL_DEBUG_TYPE_PERFORMANCE ? "performance" : "other";
			std::cerr << "[RHI/GL debug] " << sev << " " << kind << " (" << id << "): " << message << std::endl;
		}

		// --- Device / window -------------------------------------------------------------------------
		const DeviceInfo& Info() const override { return m_info; }
		const DeviceLimits& Limits() const override { return m_limits; }
		void Present() override { SDL_GL_SwapWindow(m_window); }
		void SetVSync(bool enabled) override { SDL_GL_SetSwapInterval(enabled ? 1 : 0); }
		const char* PollError() override {
			switch (glGetError()) {
			case GL_NO_ERROR: return nullptr;
			case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
			case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
			case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
			case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
			case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
			case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
			case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
			default: return "GL_UNKNOWN_ERROR";
			}
		}
		void WaitIdle() override { glFinish(); }
		void Flush() override { glFlush(); }

		// --- Textures ---------------------------------------------------------------------------------
		TextureHandle CreateTexture(const TextureDesc& d) override {
			const GLenum target = ToGL(d.type);
			GLuint id = 0;
			glCreateTextures(target, 1, &id);
			if (!id) return {};
			const GLenum fmt = ToGL(d.format);
			const int w = std::max(d.width, 1), h = std::max(d.height, 1), dep = std::max(d.depth, 1);
			int levels = d.mipLevels;
			if (levels <= 0) levels = d.type == TextureType::Tex3D ? MipCount(w, h, dep) : MipCount(w, h, 1);
			if (IsMultisample(d.type) || d.type == TextureType::Rectangle) levels = 1;
			TextureRecord rec;
			rec.target = target;
			rec.immutable = d.immutable;
			rec.info = { d.type, d.format, w, h, dep, levels, std::max(d.samples, 1) };
			m_textures[id] = rec;
			if (!d.immutable) {
				AllocateMutable(id, rec, 0, levels);
				if (d.debugName) SetDebugName(TextureHandle(id), d.debugName);
				return TextureHandle(id);
			}
			switch (d.type) {
			case TextureType::Tex1D: glTextureStorage1D(id, levels, fmt, w); break;
			case TextureType::Tex1DArray: case TextureType::Tex2D: case TextureType::Rectangle: case TextureType::Cube:
				glTextureStorage2D(id, levels, fmt, w, d.type == TextureType::Tex1DArray ? dep : h); break;
			case TextureType::Tex2DArray: case TextureType::Tex3D: case TextureType::CubeArray:
				glTextureStorage3D(id, levels, fmt, w, h, dep); break;
			case TextureType::Tex2DMS:
				glTextureStorage2DMultisample(id, std::max(d.samples, 1), fmt, w, h, d.fixedSampleLocations ? GL_TRUE : GL_FALSE); break;
			case TextureType::Tex2DMSArray:
				glTextureStorage3DMultisample(id, std::max(d.samples, 1), fmt, w, h, dep, d.fixedSampleLocations ? GL_TRUE : GL_FALSE); break;
			}
			if (d.debugName) SetDebugName(TextureHandle(id), d.debugName);
			return TextureHandle(id);
		}

		// Resizable (glTexImage) storage: levels [firstLevel, firstLevel + count) are specified without data.
		// For explicit multi-level textures the max level is set so mip-filtered sampling stays complete.
		void AllocateMutable(GLuint id, const TextureRecord& rec, int firstLevel, int count) {
			const TextureInfo& in = rec.info;
			const GLenum target = rec.target;
			const GLenum ifmt = ToGL(in.format);
			GLenum fmt = GL_RGBA, type = GL_FLOAT;
			TransferFor(in.format, fmt, type);
			GLint previous = 0;
			glGetIntegerv(BindingQuery(target), &previous);
			glBindTexture(target, id);
			for (int l = firstLevel; l < firstLevel + count; ++l) {
				const int w = std::max(in.width >> l, 1);
				const int h = std::max(in.height >> l, 1);
				const int dz = in.type == TextureType::Tex3D ? std::max(in.depth >> l, 1) : in.depth;
				switch (in.type) {
				case TextureType::Tex1D: glTexImage1D(target, l, ifmt, w, 0, fmt, type, nullptr); break;
				case TextureType::Tex1DArray: glTexImage2D(target, l, ifmt, w, in.depth, 0, fmt, type, nullptr); break;
				case TextureType::Tex2D: case TextureType::Rectangle: glTexImage2D(target, l, ifmt, w, h, 0, fmt, type, nullptr); break;
				case TextureType::Cube:
					for (int f = 0; f < 6; ++f) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, ifmt, w, h, 0, fmt, type, nullptr);
					break;
				case TextureType::Tex2DArray: case TextureType::Tex3D: case TextureType::CubeArray:
					glTexImage3D(target, l, ifmt, w, h, dz, 0, fmt, type, nullptr); break;
				case TextureType::Tex2DMS:
					glTexImage2DMultisample(target, in.samples, ifmt, w, h, GL_TRUE); break;
				case TextureType::Tex2DMSArray:
					glTexImage3DMultisample(target, in.samples, ifmt, w, h, in.depth, GL_TRUE); break;
				}
			}
			if (firstLevel == 0 && count > 1 && !IsMultisample(in.type)) glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, count - 1);
			glBindTexture(target, static_cast<GLuint>(previous));
		}

		void ResizeTexture(TextureHandle t, int width, int height, int depth, int mipLevels) override {
			auto it = m_textures.find(t.id);
			if (it == m_textures.end()) return;
			TextureRecord& rec = it->second;
			rec.info.width = std::max(width, 1);
			rec.info.height = std::max(height, 1);
			rec.info.depth = std::max(depth, 1);
			rec.info.mipLevels = std::max(mipLevels, 1);
			if (rec.immutable) {
				std::cerr << "[RHI/GL] ResizeTexture on immutable texture " << t.id << " ignored" << std::endl;
				return;
			}
			AllocateMutable(t.id, rec, 0, rec.info.mipLevels);
		}

		void DestroyTexture(TextureHandle& t) override {
			if (t.id) { glDeleteTextures(1, &t.id); m_textures.erase(t.id); }
			t = {};
		}
		bool IsTexture(TextureHandle t) const override { return t.id && glIsTexture(t.id) == GL_TRUE; }

		TextureInfo GetTextureInfo(TextureHandle t) const override {
			auto it = m_textures.find(t.id);
			if (it != m_textures.end()) return it->second.info;
			return {};
		}

		void SetSampler(TextureHandle t, const SamplerDesc& s) override {
			if (!t.id || IsMultisample(GetTextureInfo(t).type)) return;
			glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, ToGL(s.minFilter));
			glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, ToGL(s.magFilter));
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, ToGL(s.wrapS));
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, ToGL(s.wrapT));
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, ToGL(s.wrapR));
			glTextureParameterfv(t.id, GL_TEXTURE_BORDER_COLOR, glm::value_ptr(s.borderColor));
			glTextureParameteri(t.id, GL_TEXTURE_COMPARE_MODE, s.compareEnabled ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
			glTextureParameteri(t.id, GL_TEXTURE_COMPARE_FUNC, ToGL(s.compareOp));
			if (m_limits.anisotropicFiltering) glTextureParameterf(t.id, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::max(s.maxAnisotropy, 1.0f));
		}
		void SetTextureFilter(TextureHandle t, Filter minF, Filter magF) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (!t.id) return;
			glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, ToGL(minF));
			glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, ToGL(magF));
		}
		void SetTextureMinFilter(TextureHandle t, Filter f) override {
			if (!t.id || IsMultisample(GetTextureInfo(t).type)) return;
			glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, ToGL(f));
		}
		void SetTextureMagFilter(TextureHandle t, Filter f) override {
			if (!t.id || IsMultisample(GetTextureInfo(t).type)) return;
			glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, ToGL(f));
		}
		void SetTextureWrapAxis(TextureHandle t, int axis, Wrap w) override {
			if (!t.id || IsMultisample(GetTextureInfo(t).type)) return;
			const GLenum pname = axis == 0 ? GL_TEXTURE_WRAP_S : (axis == 1 ? GL_TEXTURE_WRAP_T : GL_TEXTURE_WRAP_R);
			glTextureParameteri(t.id, pname, ToGL(w));
		}
		void SetTextureWrap(TextureHandle t, Wrap s, Wrap tt, Wrap r) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (!t.id) return;
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, ToGL(s));
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, ToGL(tt));
			glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, ToGL(r));
		}
		void SetTextureBorderColor(TextureHandle t, const glm::vec4& c) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (t.id) glTextureParameterfv(t.id, GL_TEXTURE_BORDER_COLOR, glm::value_ptr(c));
		}
		void SetTextureCompare(TextureHandle t, bool enabled, CompareOp op) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (!t.id) return;
			glTextureParameteri(t.id, GL_TEXTURE_COMPARE_MODE, enabled ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
			glTextureParameteri(t.id, GL_TEXTURE_COMPARE_FUNC, ToGL(op));
		}
		void SetTextureAnisotropy(TextureHandle t, float a) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (t.id && m_limits.anisotropicFiltering) glTextureParameterf(t.id, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::max(a, 1.0f));
		}
		void SetTextureMipRange(TextureHandle t, int baseLevel, int maxLevel) override {
			if (IsMultisample(GetTextureInfo(t).type)) return;
			if (!t.id) return;
			glTextureParameteri(t.id, GL_TEXTURE_BASE_LEVEL, baseLevel);
			glTextureParameteri(t.id, GL_TEXTURE_MAX_LEVEL, maxLevel);
		}

		glm::ivec3 LevelExtent(const TextureInfo& info, int level) const {
			const int w = std::max(info.width >> level, 1);
			const int h = info.type == TextureType::Tex1D || info.type == TextureType::Tex1DArray ? info.height : std::max(info.height >> level, 1);
			int d = info.depth;
			if (info.type == TextureType::Tex3D) d = std::max(info.depth >> level, 1);
			if (info.type == TextureType::Cube) d = 6;
			return { w, info.type == TextureType::Tex1DArray ? info.depth : h, d };
		}

		void UpdateTexture(TextureHandle t, const TextureRegion& r, PixelFormat pf, PixelType pt, const void* data, int align) override {
			if (!t.id) return;
			const TextureInfo info = GetTextureInfo(t);
			const glm::ivec3 full = LevelExtent(info, r.level);
			const glm::ivec3 e(r.extent.x > 0 ? r.extent.x : full.x - r.offset.x,
				r.extent.y > 0 ? r.extent.y : full.y - r.offset.y,
				r.extent.z > 0 ? r.extent.z : full.z - r.offset.z);
			glPixelStorei(GL_UNPACK_ALIGNMENT, align);
			if (info.type == TextureType::Cube && !m_textures[t.id].immutable) {
				// Resizable cube maps are per-face images: upload face by face (DSA 3D uploads into
				// face-specified cube maps are unreliable on some drivers).
				GLint previous = 0;
				glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &previous);
				glBindTexture(GL_TEXTURE_CUBE_MAP, t.id);
				const size_t faceStride = static_cast<size_t>(e.x) * e.y * BytesPerPixel(pf, pt);
				for (int f = 0; f < e.z; ++f) {
					const auto* faceData = data ? static_cast<const unsigned char*>(data) + faceStride * f : nullptr;
					glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + r.offset.z + f, r.level, r.offset.x, r.offset.y, e.x, e.y,
						ToGL(pf), ToGL(pt), faceData);
				}
				glBindTexture(GL_TEXTURE_CUBE_MAP, static_cast<GLuint>(previous));
				if (align != 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				return;
			}
			switch (info.type) {
			case TextureType::Tex1D:
				glTextureSubImage1D(t.id, r.level, r.offset.x, e.x, ToGL(pf), ToGL(pt), data); break;
			case TextureType::Tex1DArray: case TextureType::Tex2D: case TextureType::Rectangle:
				glTextureSubImage2D(t.id, r.level, r.offset.x, r.offset.y, e.x, e.y, ToGL(pf), ToGL(pt), data); break;
			default:
				glTextureSubImage3D(t.id, r.level, r.offset.x, r.offset.y, r.offset.z, e.x, e.y, e.z, ToGL(pf), ToGL(pt), data); break;
			}
			if (align != 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		}

		void GenerateMipmaps(TextureHandle t) override {
			if (!t.id) return;
			glGenerateTextureMipmap(t.id);
			auto it = m_textures.find(t.id);
			if (it != m_textures.end()) {
				const TextureInfo& in = it->second.info;
				it->second.info.mipLevels = in.type == TextureType::Tex3D ? MipCount(in.width, in.height, in.depth) : MipCount(in.width, in.height, 1);
			}
		}

		void ClearTexture(TextureHandle t, int level, PixelFormat pf, PixelType pt, const void* value) override {
			if (t.id) glClearTexImage(t.id, level, ToGL(pf), ToGL(pt), value);
		}

		void CopyTexture(TextureHandle src, const TextureRegion& r, TextureHandle dst, int dstLevel, const glm::ivec3& dstOffset) override {
			if (!src.id || !dst.id) return;
			const TextureInfo si = GetTextureInfo(src);
			const glm::ivec3 full = LevelExtent(si, r.level);
			const glm::ivec3 e(r.extent.x > 0 ? r.extent.x : full.x, r.extent.y > 0 ? r.extent.y : full.y, r.extent.z > 0 ? r.extent.z : full.z);
			glCopyImageSubData(src.id, m_textures[src.id].target, r.level, r.offset.x, r.offset.y, r.offset.z,
				dst.id, m_textures[dst.id].target, dstLevel, dstOffset.x, dstOffset.y, dstOffset.z, e.x, e.y, e.z);
		}

		void ReadTexture(TextureHandle t, int level, PixelFormat pf, PixelType pt, size_t size, void* out, int align) override {
			if (!t.id) return;
			glPixelStorei(GL_PACK_ALIGNMENT, align);
			glGetTextureImage(t.id, level, ToGL(pf), ToGL(pt), static_cast<GLsizei>(size), out);
			if (align != 4) glPixelStorei(GL_PACK_ALIGNMENT, 4);
		}

		void ReadTextureRegion(TextureHandle t, const TextureRegion& r, PixelFormat pf, PixelType pt, size_t size, void* out, int align) override {
			if (!t.id) return;
			glPixelStorei(GL_PACK_ALIGNMENT, align);
			glGetTextureSubImage(t.id, r.level, r.offset.x, r.offset.y, r.offset.z, std::max(r.extent.x, 1), std::max(r.extent.y, 1),
				std::max(r.extent.z, 1), ToGL(pf), ToGL(pt), static_cast<GLsizei>(size), out);
			if (align != 4) glPixelStorei(GL_PACK_ALIGNMENT, 4);
		}

		// --- Buffers ----------------------------------------------------------------------------------
		BufferHandle CreateBuffer(const BufferDesc& d) override {
			GLuint id = 0;
			glCreateBuffers(1, &id);
			if (!id) return {};
			if (d.immutable) glNamedBufferStorage(id, static_cast<GLsizeiptr>(d.size), d.initialData, ToGL(d.storage));
			else glNamedBufferData(id, static_cast<GLsizeiptr>(d.size), d.initialData, ToGL(d.usage));
			m_bufferSizes[id] = d.size;
			if (d.debugName) SetDebugName(BufferHandle(id), d.debugName);
			return BufferHandle(id);
		}
		void DestroyBuffer(BufferHandle& b) override {
			if (b.id) { glDeleteBuffers(1, &b.id); m_bufferSizes.erase(b.id); }
			b = {};
		}
		bool IsBuffer(BufferHandle b) const override { return b.id && glIsBuffer(b.id) == GL_TRUE; }
		size_t GetBufferSize(BufferHandle b) const override {
			auto it = m_bufferSizes.find(b.id);
			return it != m_bufferSizes.end() ? it->second : 0;
		}
		void ReallocateBuffer(BufferHandle b, size_t size, const void* data, BufferUsage usage) override {
			if (!b.id) return;
			glNamedBufferData(b.id, static_cast<GLsizeiptr>(size), data, ToGL(usage));
			m_bufferSizes[b.id] = size;
		}
		void UpdateBuffer(BufferHandle b, size_t offset, size_t size, const void* data) override {
			if (b.id && size) glNamedBufferSubData(b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
		}
		void ReadBuffer(BufferHandle b, size_t offset, size_t size, void* out) override {
			if (b.id && size) glGetNamedBufferSubData(b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), out);
		}
		void CopyBuffer(BufferHandle src, size_t so, BufferHandle dst, size_t dOff, size_t size) override {
			if (src.id && dst.id && size) glCopyNamedBufferSubData(src.id, dst.id, static_cast<GLintptr>(so), static_cast<GLintptr>(dOff), static_cast<GLsizeiptr>(size));
		}
		void ClearBuffer(BufferHandle b, Format elementFormat, PixelFormat pf, PixelType pt, const void* value) override {
			if (b.id) glClearNamedBufferData(b.id, ToGL(elementFormat), ToGL(pf), ToGL(pt), value);
		}
		void* MapBuffer(BufferHandle b, size_t offset, size_t size, MapAccess access) override {
			if (!b.id) return nullptr;
			return glMapNamedBufferRange(b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), ToGL(access));
		}
		void FlushMappedRange(BufferHandle b, size_t offset, size_t size) override {
			if (b.id) glFlushMappedNamedBufferRange(b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
		}
		void UnmapBuffer(BufferHandle b) override { if (b.id) glUnmapNamedBuffer(b.id); }

		// --- Programs ---------------------------------------------------------------------------------
		ProgramHandle CreateProgram(const ProgramDesc& d) override {
			std::vector<GLuint> shaders;
			bool ok = true;
			for (const ShaderSource& s : d.stages) {
				const GLuint sh = glCreateShader(ToGL(s.stage));
				const char* src = s.source.c_str();
				glShaderSource(sh, 1, &src, nullptr);
				glCompileShader(sh);
				GLint status = GL_FALSE;
				glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
				if (status != GL_TRUE) {
					GLint len = 0;
					glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
					std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
					glGetShaderInfoLog(sh, len, nullptr, log.data());
					std::cerr << "[RHI/GL] Shader compile failed: " << s.debugPath << "\n" << log << std::endl;
					ok = false;
				}
				shaders.push_back(sh);
			}
			GLuint program = 0;
			if (ok) {
				program = glCreateProgram();
				for (GLuint sh : shaders) glAttachShader(program, sh);
				glLinkProgram(program);
				GLint status = GL_FALSE;
				glGetProgramiv(program, GL_LINK_STATUS, &status);
				if (status != GL_TRUE) {
					GLint len = 0;
					glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
					std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
					glGetProgramInfoLog(program, len, nullptr, log.data());
					std::cerr << "[RHI/GL] Program link failed: " << d.debugName << "\n" << log << std::endl;
					glDeleteProgram(program);
					program = 0;
				}
				else {
					for (GLuint sh : shaders) glDetachShader(program, sh);
				}
			}
			for (GLuint sh : shaders) glDeleteShader(sh);
			if (program && !d.debugName.empty()) SetDebugName(ProgramHandle(program), d.debugName.c_str());
			return ProgramHandle(program);
		}
		void DestroyProgram(ProgramHandle& p) override {
			if (p.id) glDeleteProgram(p.id);
			p = {};
		}
		bool IsProgram(ProgramHandle p) const override { return p.id && glIsProgram(p.id) == GL_TRUE; }
		void UseProgram(ProgramHandle p) override { glUseProgram(p.id); }
		UniformLocation GetUniformLocation(ProgramHandle p, const char* name) const override {
			return p.id ? glGetUniformLocation(p.id, name) : -1;
		}
		void SetUniform(UniformLocation l, int v) override { glUniform1i(l, v); }
		void SetUniform(UniformLocation l, uint32_t v) override { glUniform1ui(l, v); }
		void SetUniform(UniformLocation l, float v) override { glUniform1f(l, v); }
		void SetUniform(UniformLocation l, const glm::vec2& v) override { glUniform2fv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::vec3& v) override { glUniform3fv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::vec4& v) override { glUniform4fv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::ivec2& v) override { glUniform2iv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::ivec3& v) override { glUniform3iv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::ivec4& v) override { glUniform4iv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::uvec2& v) override { glUniform2uiv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::uvec3& v) override { glUniform3uiv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::uvec4& v) override { glUniform4uiv(l, 1, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::mat3& v) override { glUniformMatrix3fv(l, 1, GL_FALSE, glm::value_ptr(v)); }
		void SetUniform(UniformLocation l, const glm::mat4& v) override { glUniformMatrix4fv(l, 1, GL_FALSE, glm::value_ptr(v)); }
		void SetUniformArray(UniformLocation l, const int* v, int n) override { glUniform1iv(l, n, v); }
		void SetUniformArray(UniformLocation l, const float* v, int n) override { glUniform1fv(l, n, v); }
		void SetUniformArray(UniformLocation l, const glm::vec2* v, int n) override { glUniform2fv(l, n, glm::value_ptr(v[0])); }
		void SetUniformArray(UniformLocation l, const glm::vec3* v, int n) override { glUniform3fv(l, n, glm::value_ptr(v[0])); }
		void SetUniformArray(UniformLocation l, const glm::vec4* v, int n) override { glUniform4fv(l, n, glm::value_ptr(v[0])); }
		void SetUniformArray(UniformLocation l, const glm::mat3* v, int n) override { glUniformMatrix3fv(l, n, GL_FALSE, glm::value_ptr(v[0])); }
		void SetUniformArray(UniformLocation l, const glm::mat4* v, int n) override { glUniformMatrix4fv(l, n, GL_FALSE, glm::value_ptr(v[0])); }

		// --- Vertex input -----------------------------------------------------------------------------
		VertexInputHandle CreateVertexInput(const VertexInputDesc& d) override {
			GLuint vao = 0;
			glCreateVertexArrays(1, &vao);
			if (!vao) return {};
			for (const VertexAttribute& a : d.attributes) {
				glEnableVertexArrayAttrib(vao, a.location);
				if (a.integer) glVertexArrayAttribIFormat(vao, a.location, static_cast<GLint>(a.components), ToGL(a.type), a.offset);
				else glVertexArrayAttribFormat(vao, a.location, static_cast<GLint>(a.components), ToGL(a.type), a.normalized ? GL_TRUE : GL_FALSE, a.offset);
				glVertexArrayAttribBinding(vao, a.location, a.bufferIndex);
			}
			VertexInputRecord rec;
			rec.desc = d;
			for (uint32_t i = 0; i < d.buffers.size(); ++i) ApplyBinding(vao, i, d.buffers[i], d);
			if (d.indexBuffer.id) glVertexArrayElementBuffer(vao, d.indexBuffer.id);
			m_vertexInputs[vao] = std::move(rec);
			return VertexInputHandle(vao);
		}
		void ApplyBinding(GLuint vao, uint32_t index, const VertexBufferBinding& b, const VertexInputDesc& d) {
			GLsizei stride = static_cast<GLsizei>(b.stride);
			if (stride == 0) {
				// Tightly packed: the size of the (single) attribute sourced from this binding.
				for (const VertexAttribute& a : d.attributes) {
					if (a.bufferIndex == index) { stride = static_cast<GLsizei>(a.components * ScalarSize(a.type)); break; }
				}
			}
			glVertexArrayVertexBuffer(vao, index, b.buffer.id, static_cast<GLintptr>(b.offset), stride);
			glVertexArrayBindingDivisor(vao, index, b.instanceDivisor);
		}
		void DestroyVertexInput(VertexInputHandle& v) override {
			if (v.id) { glDeleteVertexArrays(1, &v.id); m_vertexInputs.erase(v.id); }
			v = {};
		}
		void SetVertexInputBuffer(VertexInputHandle v, uint32_t index, const VertexBufferBinding& b) override {
			auto it = m_vertexInputs.find(v.id);
			if (it == m_vertexInputs.end()) return;
			if (index >= it->second.desc.buffers.size()) it->second.desc.buffers.resize(index + 1);
			it->second.desc.buffers[index] = b;
			ApplyBinding(v.id, index, b, it->second.desc);
		}
		void SetVertexInputIndexBuffer(VertexInputHandle v, BufferHandle ib) override {
			if (v.id) glVertexArrayElementBuffer(v.id, ib.id);
		}
		void BindVertexInput(VertexInputHandle v) override {
			if (!v.id) {
				if (!m_emptyVao) glCreateVertexArrays(1, &m_emptyVao);
				glBindVertexArray(m_emptyVao);
				return;
			}
			glBindVertexArray(v.id);
		}

		// --- Framebuffers -----------------------------------------------------------------------------
		FramebufferHandle CreateFramebuffer(const FramebufferDesc& d) override {
			GLuint fb = 0;
			glCreateFramebuffers(1, &fb);
			if (!fb) return {};
			const FramebufferHandle h(fb);
			for (size_t i = 0; i < d.colors.size(); ++i) {
				if (d.colors[i].texture.id) SetAttachment(h, ColorAttachment(static_cast<int>(i)), d.colors[i]);
			}
			if (d.depth.texture.id) {
				const Format df = GetTextureInfo(d.depth.texture).format;
				SetAttachment(h, HasStencil(df) ? AttachmentPoint::DepthStencil : AttachmentPoint::Depth, d.depth);
			}
			// With colour targets: draw to all of them. Without: keep the API default (colour attachment 0
			// for draw and read), so targets attached later with SetAttachment receive output; depth-only
			// users call SetDrawBuffers(fb, nullptr, 0) / SetReadBuffer(fb, -1) explicitly.
			if (!d.colors.empty()) {
				std::vector<GLenum> bufs;
				for (size_t i = 0; i < d.colors.size(); ++i) bufs.push_back(d.colors[i].texture.id ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i) : GL_NONE);
				glNamedFramebufferDrawBuffers(fb, static_cast<GLsizei>(bufs.size()), bufs.data());
				glNamedFramebufferReadBuffer(fb, GL_COLOR_ATTACHMENT0);
			}
			if (d.debugName) SetDebugName(h, d.debugName);
			return h;
		}
		void DestroyFramebuffer(FramebufferHandle& f) override {
			if (f.id) glDeleteFramebuffers(1, &f.id);
			f = {};
		}
		void SetAttachment(FramebufferHandle f, AttachmentPoint p, const Attachment& a) override {
			if (!f.id) return;
			const GLenum attach = ToGLAttachment(p);
			if (!a.texture.id) { glNamedFramebufferTexture(f.id, attach, 0, 0); return; }
			if (a.layer == kAllLayers) glNamedFramebufferTexture(f.id, attach, a.texture.id, a.level);
			else glNamedFramebufferTextureLayer(f.id, attach, a.texture.id, a.level, a.layer);
		}
		bool IsFramebufferComplete(FramebufferHandle f, std::string* reason) override {
			const GLenum s = glCheckNamedFramebufferStatus(f.id, GL_FRAMEBUFFER);
			if (s == GL_FRAMEBUFFER_COMPLETE) return true;
			if (reason) {
				switch (s) {
				case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: *reason = "incomplete attachment"; break;
				case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: *reason = "missing attachment"; break;
				case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: *reason = "incomplete draw buffer"; break;
				case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: *reason = "incomplete read buffer"; break;
				case GL_FRAMEBUFFER_UNSUPPORTED: *reason = "unsupported"; break;
				case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: *reason = "incomplete multisample"; break;
				case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: *reason = "incomplete layer targets"; break;
				default: *reason = "status 0x" + std::to_string(s); break;
				}
			}
			return false;
		}
		void SetDrawBuffers(FramebufferHandle f, const int* a, int n) override {
			if (!f.id) { glNamedFramebufferDrawBuffer(0, n > 0 && a[0] >= 0 ? GL_BACK : GL_NONE); return; }
			std::vector<GLenum> bufs(static_cast<size_t>(std::max(n, 0)));
			for (int i = 0; i < n; ++i) bufs[i] = a[i] >= 0 ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(a[i]) : GL_NONE;
			if (n == 0) glNamedFramebufferDrawBuffer(f.id, GL_NONE);
			else glNamedFramebufferDrawBuffers(f.id, n, bufs.data());
		}
		void SetReadBuffer(FramebufferHandle f, int a) override {
			glNamedFramebufferReadBuffer(f.id, a < 0 ? GL_NONE : (f.id ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(a) : GL_BACK));
		}
		void BindFramebuffer(FramebufferHandle f) override { glBindFramebuffer(GL_FRAMEBUFFER, f.id); }
		void BindDrawFramebuffer(FramebufferHandle f) override { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.id); }
		void BindReadFramebuffer(FramebufferHandle f) override { glBindFramebuffer(GL_READ_FRAMEBUFFER, f.id); }
		FramebufferHandle CurrentDrawFramebuffer() const override {
			GLint id = 0;
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &id);
			return FramebufferHandle(static_cast<uint32_t>(id));
		}
		void SetClearColor(const glm::vec4& c) override { glClearColor(c.r, c.g, c.b, c.a); }
		void SetClearDepth(float d) override { glClearDepth(d); }
		void Clear(uint32_t flags) override { glClear(ToGLClear(flags)); }
		void ClearColorAttachment(int a, const glm::vec4& v) override { glClearBufferfv(GL_COLOR, a, glm::value_ptr(v)); }
		void ClearColorAttachment(int a, const glm::uvec4& v) override { glClearBufferuiv(GL_COLOR, a, glm::value_ptr(v)); }
		void ClearDepthStencil(float d, int s) override { glClearBufferfi(GL_DEPTH_STENCIL, 0, d, s); }
		void BlitFramebuffer(FramebufferHandle src, int srcAttachment, const Rect& sr, FramebufferHandle dst, const Rect& dr,
			uint32_t mask, BlitFilter filter) override {
			if (mask & ClearColor) glNamedFramebufferReadBuffer(src.id, src.id ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(std::max(srcAttachment, 0)) : GL_BACK);
			glBlitNamedFramebuffer(src.id, dst.id, sr.x, sr.y, sr.x + sr.width, sr.y + sr.height, dr.x, dr.y, dr.x + dr.width, dr.y + dr.height,
				ToGLClear(mask), filter == BlitFilter::Linear ? GL_LINEAR : GL_NEAREST);
		}
		void ReadPixels(FramebufferHandle f, int attachment, const Rect& r, PixelFormat pf, PixelType pt, void* out, int align) override {
			GLint prevRead = 0;
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, f.id);
			if (pf != PixelFormat::Depth && pf != PixelFormat::DepthStencil) {
				glReadBuffer(f.id ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(std::max(attachment, 0)) : GL_BACK);
			}
			glPixelStorei(GL_PACK_ALIGNMENT, align);
			glReadPixels(r.x, r.y, r.width, r.height, ToGL(pf), ToGL(pt), out);
			if (align != 4) glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
		}

		// --- State ------------------------------------------------------------------------------------
		void SetViewport(const Rect& v) override { glViewport(v.x, v.y, v.width, v.height); }
		Rect GetViewport() const override {
			GLint v[4] = {};
			glGetIntegerv(GL_VIEWPORT, v);
			return { v[0], v[1], v[2], v[3] };
		}
		void SetScissor(bool enabled, const Rect& r) override {
			if (enabled) { glEnable(GL_SCISSOR_TEST); glScissor(r.x, r.y, r.width, r.height); }
			else glDisable(GL_SCISSOR_TEST);
		}
		void SetScissorEnabled(bool e) override { e ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST); }
		void SetScissorRect(const Rect& r) override { glScissor(r.x, r.y, r.width, r.height); }
		void SetDepthTest(bool e) override { e ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST); }
		void SetDepthWrite(bool e) override { glDepthMask(e ? GL_TRUE : GL_FALSE); }
		void SetDepthFunc(CompareOp op) override { glDepthFunc(ToGL(op)); }
		void SetCullEnabled(bool e) override { e ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); }
		void SetCullFace(CullFace f) override { glCullFace(ToGL(f)); }
		void SetFrontFace(FrontFace f) override { glFrontFace(f == FrontFace::CounterClockwise ? GL_CCW : GL_CW); }
		void SetPolygonMode(PolygonMode m) override { glPolygonMode(GL_FRONT_AND_BACK, ToGL(m)); }
		void SetPolygonOffset(bool e, float factor, float units) override {
			if (e) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(factor, units); }
			else glDisable(GL_POLYGON_OFFSET_FILL);
		}
		void SetPolygonOffsetEnabled(bool e) override { e ? glEnable(GL_POLYGON_OFFSET_FILL) : glDisable(GL_POLYGON_OFFSET_FILL); }
		void SetPolygonOffsetFactors(float factor, float units) override { glPolygonOffset(factor, units); }
		void SetLineWidth(float w) override { glLineWidth(w); }
		void SetBlend(const BlendState& s) override {
			s.enabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
			glBlendFuncSeparate(ToGL(s.srcColor), ToGL(s.dstColor), ToGL(s.srcAlpha), ToGL(s.dstAlpha));
			glBlendEquationSeparate(ToGL(s.colorOp), ToGL(s.alphaOp));
		}
		void SetBlend(int rt, const BlendState& s) override {
			const GLuint i = static_cast<GLuint>(rt);
			s.enabled ? glEnablei(GL_BLEND, i) : glDisablei(GL_BLEND, i);
			glBlendFuncSeparatei(i, ToGL(s.srcColor), ToGL(s.dstColor), ToGL(s.srcAlpha), ToGL(s.dstAlpha));
			glBlendEquationSeparatei(i, ToGL(s.colorOp), ToGL(s.alphaOp));
		}
		void SetBlendEnabled(bool e) override { e ? glEnable(GL_BLEND) : glDisable(GL_BLEND); }
		void SetBlendEnabled(int rt, bool e) override { e ? glEnablei(GL_BLEND, static_cast<GLuint>(rt)) : glDisablei(GL_BLEND, static_cast<GLuint>(rt)); }
		void SetBlendFunc(BlendFactor src, BlendFactor dst) override { glBlendFunc(ToGL(src), ToGL(dst)); }
		void SetBlendFuncSeparate(BlendFactor sc, BlendFactor dc, BlendFactor sa, BlendFactor da) override {
			glBlendFuncSeparate(ToGL(sc), ToGL(dc), ToGL(sa), ToGL(da));
		}
		void SetBlendFunc(int rt, BlendFactor src, BlendFactor dst) override { glBlendFunci(static_cast<GLuint>(rt), ToGL(src), ToGL(dst)); }
		void SetBlendEquation(BlendOp op) override { glBlendEquation(ToGL(op)); }
		void SetBlendEquationSeparate(BlendOp c, BlendOp a) override { glBlendEquationSeparate(ToGL(c), ToGL(a)); }
		void SetColorMask(bool r, bool g, bool b, bool a) override { glColorMask(r, g, b, a); }
		void SetColorMask(int rt, bool r, bool g, bool b, bool a) override { glColorMaski(static_cast<GLuint>(rt), r, g, b, a); }
		void SetFeature(Feature f, bool e) override { e ? glEnable(ToGL(f)) : glDisable(ToGL(f)); }

		bool IsDepthTestEnabled() const override { return glIsEnabled(GL_DEPTH_TEST) == GL_TRUE; }
		bool IsCullEnabled() const override { return glIsEnabled(GL_CULL_FACE) == GL_TRUE; }
		bool IsBlendEnabled() const override { return glIsEnabled(GL_BLEND) == GL_TRUE; }
		ProgramHandle CurrentProgram() const override {
			GLint p = 0;
			glGetIntegerv(GL_CURRENT_PROGRAM, &p);
			return ProgramHandle(static_cast<uint32_t>(p));
		}
		TextureHandle BoundTexture(uint32_t unit, TextureType type) const override {
			GLint active = 0, bound = 0;
			glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
			glActiveTexture(GL_TEXTURE0 + unit);
			glGetIntegerv(BindingQuery(ToGL(type)), &bound);
			glActiveTexture(static_cast<GLenum>(active));
			return TextureHandle(static_cast<uint32_t>(bound));
		}

		StateSnapshot CaptureState() const override {
			StateSnapshot s;
			GLint v = 0;
			GLboolean b = GL_FALSE;
			s.depthTest = glIsEnabled(GL_DEPTH_TEST);
			glGetBooleanv(GL_DEPTH_WRITEMASK, &b); s.depthWrite = b == GL_TRUE;
			glGetIntegerv(GL_DEPTH_FUNC, &v); s.depthFunc = FromGLCompare(v);
			s.cullEnabled = glIsEnabled(GL_CULL_FACE);
			glGetIntegerv(GL_CULL_FACE_MODE, &v); s.cullFace = v == GL_FRONT ? CullFace::Front : (v == GL_FRONT_AND_BACK ? CullFace::FrontAndBack : CullFace::Back);
			glGetIntegerv(GL_FRONT_FACE, &v); s.frontFace = v == GL_CW ? FrontFace::Clockwise : FrontFace::CounterClockwise;
			GLint pm[2] = {};
			glGetIntegerv(GL_POLYGON_MODE, pm); s.polygonMode = pm[0] == GL_LINE ? PolygonMode::Line : (pm[0] == GL_POINT ? PolygonMode::Point : PolygonMode::Fill);
			s.polygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
			glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s.polygonOffsetFactor);
			glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s.polygonOffsetUnits);
			s.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
			GLint r[4] = {};
			glGetIntegerv(GL_SCISSOR_BOX, r); s.scissor = { r[0], r[1], r[2], r[3] };
			glGetIntegerv(GL_VIEWPORT, r); s.viewport = { r[0], r[1], r[2], r[3] };
			s.blend.enabled = glIsEnabled(GL_BLEND);
			glGetIntegerv(GL_BLEND_SRC_RGB, &v); s.blend.srcColor = FromGLBlend(v);
			glGetIntegerv(GL_BLEND_DST_RGB, &v); s.blend.dstColor = FromGLBlend(v);
			glGetIntegerv(GL_BLEND_SRC_ALPHA, &v); s.blend.srcAlpha = FromGLBlend(v);
			glGetIntegerv(GL_BLEND_DST_ALPHA, &v); s.blend.dstAlpha = FromGLBlend(v);
			glGetIntegerv(GL_BLEND_EQUATION_RGB, &v); s.blend.colorOp = FromGLBlendOp(v);
			glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v); s.blend.alphaOp = FromGLBlendOp(v);
			GLboolean cm[4] = {};
			glGetBooleanv(GL_COLOR_WRITEMASK, cm);
			for (int i = 0; i < 4; ++i) s.colorMask[i] = cm[i] == GL_TRUE;
			s.stencilTest = glIsEnabled(GL_STENCIL_TEST);
			s.depthClamp = glIsEnabled(GL_DEPTH_CLAMP);
			s.programPointSize = glIsEnabled(GL_PROGRAM_POINT_SIZE);
			s.framebufferSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
			glGetFloatv(GL_COLOR_CLEAR_VALUE, glm::value_ptr(s.clearColor));
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v); s.drawFramebuffer = FramebufferHandle(static_cast<uint32_t>(v));
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &v); s.readFramebuffer = FramebufferHandle(static_cast<uint32_t>(v));
			glGetIntegerv(GL_CURRENT_PROGRAM, &v); s.program = ProgramHandle(static_cast<uint32_t>(v));
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &v); s.vertexInput = VertexInputHandle(static_cast<uint32_t>(v));
			return s;
		}

		void RestoreState(const StateSnapshot& s) override {
			SetDepthTest(s.depthTest);
			SetDepthWrite(s.depthWrite);
			SetDepthFunc(s.depthFunc);
			SetCullEnabled(s.cullEnabled);
			SetCullFace(s.cullFace);
			SetFrontFace(s.frontFace);
			SetPolygonMode(s.polygonMode);
			s.polygonOffset ? glEnable(GL_POLYGON_OFFSET_FILL) : glDisable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(s.polygonOffsetFactor, s.polygonOffsetUnits);
			s.scissorTest ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
			glScissor(s.scissor.x, s.scissor.y, s.scissor.width, s.scissor.height);
			SetViewport(s.viewport);
			SetBlend(s.blend);
			glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
			s.stencilTest ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
			s.depthClamp ? glEnable(GL_DEPTH_CLAMP) : glDisable(GL_DEPTH_CLAMP);
			s.programPointSize ? glEnable(GL_PROGRAM_POINT_SIZE) : glDisable(GL_PROGRAM_POINT_SIZE);
			s.framebufferSRGB ? glEnable(GL_FRAMEBUFFER_SRGB) : glDisable(GL_FRAMEBUFFER_SRGB);
			SetClearColor(s.clearColor);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFramebuffer.id);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, s.readFramebuffer.id);
			glUseProgram(s.program.id);
			glBindVertexArray(s.vertexInput.id);
		}

		// --- Resource binding -------------------------------------------------------------------------
		void BindTexture(uint32_t unit, TextureHandle t) override { glBindTextureUnit(unit, t.id); }
		void BindImage(uint32_t unit, TextureHandle t, int level, bool layered, int layer, ImageAccess access, Format format) override {
			glBindImageTexture(unit, t.id, level, layered ? GL_TRUE : GL_FALSE, layer, ToGL(access),
				format == Format::Unknown ? GL_R8 : ToGL(format));
		}
		void BindStorageBuffer(uint32_t slot, BufferHandle b, size_t offset, size_t size) override {
			if (size == 0) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, b.id);
			else glBindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
		}
		void BindUniformBuffer(uint32_t slot, BufferHandle b, size_t offset, size_t size) override {
			if (size == 0) glBindBufferBase(GL_UNIFORM_BUFFER, slot, b.id);
			else glBindBufferRange(GL_UNIFORM_BUFFER, slot, b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
		}
		void BindAtomicCounterBuffer(uint32_t slot, BufferHandle b, size_t offset, size_t size) override {
			if (size == 0) glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, slot, b.id);
			else glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, slot, b.id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
		}

		// --- Draw / dispatch --------------------------------------------------------------------------
		void Draw(Topology t, uint32_t first, uint32_t count, uint32_t instances, uint32_t firstInstance) override {
			if (instances == 1 && firstInstance == 0) glDrawArrays(ToGL(t), static_cast<GLint>(first), static_cast<GLsizei>(count));
			else glDrawArraysInstancedBaseInstance(ToGL(t), static_cast<GLint>(first), static_cast<GLsizei>(count), static_cast<GLsizei>(instances), firstInstance);
		}
		void DrawIndexed(Topology t, uint32_t count, IndexType it, size_t offset, uint32_t instances, int32_t baseVertex, uint32_t firstInstance) override {
			const void* ptr = reinterpret_cast<const void*>(offset);
			if (instances == 1 && baseVertex == 0 && firstInstance == 0) glDrawElements(ToGL(t), static_cast<GLsizei>(count), ToGL(it), ptr);
			else glDrawElementsInstancedBaseVertexBaseInstance(ToGL(t), static_cast<GLsizei>(count), ToGL(it), ptr,
				static_cast<GLsizei>(instances), baseVertex, firstInstance);
		}
		void DrawIndexedIndirect(Topology t, IndexType it, BufferHandle args, size_t offset, uint32_t drawCount, uint32_t stride) override {
			glBindBuffer(GL_DRAW_INDIRECT_BUFFER, args.id);
			const void* ptr = reinterpret_cast<const void*>(offset);
			if (drawCount == 1) glDrawElementsIndirect(ToGL(t), ToGL(it), ptr);
			else glMultiDrawElementsIndirect(ToGL(t), ToGL(it), ptr, static_cast<GLsizei>(drawCount), static_cast<GLsizei>(stride));
		}
		void Dispatch(uint32_t x, uint32_t y, uint32_t z) override { glDispatchCompute(x, y, z); }
		void DispatchIndirect(BufferHandle args, size_t offset) override {
			glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args.id);
			glDispatchComputeIndirect(static_cast<GLintptr>(offset));
		}
		void PipelineBarrier(uint32_t barriers) override { glMemoryBarrier(ToGLBarriers(barriers)); }

		// --- Queries ----------------------------------------------------------------------------------
		QueryHandle CreateQuery(QueryType type) override {
			GLuint q = 0;
			glGenQueries(1, &q);
			if (q) m_queries[q] = type;
			return QueryHandle(q);
		}
		void DestroyQuery(QueryHandle& q) override {
			if (q.id) { glDeleteQueries(1, &q.id); m_queries.erase(q.id); }
			q = {};
		}
		GLenum QueryTarget(QueryHandle q) const {
			auto it = m_queries.find(q.id);
			const QueryType t = it != m_queries.end() ? it->second : QueryType::TimeElapsed;
			switch (t) {
			case QueryType::Timestamp: return GL_TIMESTAMP;
			case QueryType::TimeElapsed: return GL_TIME_ELAPSED;
			case QueryType::PrimitivesSubmitted: return GL_PRIMITIVES_SUBMITTED_ARB;
			case QueryType::ComputeInvocations: return GL_COMPUTE_SHADER_INVOCATIONS_ARB;
			case QueryType::AnySamplesPassed: return GL_ANY_SAMPLES_PASSED;
			}
			return GL_TIME_ELAPSED;
		}
		void WriteTimestamp(QueryHandle q) override { if (q.id) glQueryCounter(q.id, GL_TIMESTAMP); }
		void BeginQuery(QueryHandle q) override { if (q.id) glBeginQuery(QueryTarget(q), q.id); }
		void EndQuery(QueryHandle q) override { if (q.id) glEndQuery(QueryTarget(q)); }
		bool IsQueryResultAvailable(QueryHandle q) override {
			if (!q.id) return false;
			GLint available = 0;
			glGetQueryObjectiv(q.id, GL_QUERY_RESULT_AVAILABLE, &available);
			return available != 0;
		}
		uint64_t GetQueryResult(QueryHandle q) override {
			if (!q.id) return 0;
			GLuint64 v = 0;
			glGetQueryObjectui64v(q.id, GL_QUERY_RESULT, &v);
			return v;
		}

		// --- Debug ------------------------------------------------------------------------------------
		void Label(GLenum identifier, GLuint name, const char* label) {
			if (m_debugLabels && name && label) glObjectLabel(identifier, name, -1, label);
		}
		void SetDebugName(TextureHandle t, const char* n) override { Label(GL_TEXTURE, t.id, n); }
		void SetDebugName(BufferHandle b, const char* n) override { Label(GL_BUFFER, b.id, n); }
		void SetDebugName(ProgramHandle p, const char* n) override { Label(GL_PROGRAM, p.id, n); }
		void SetDebugName(FramebufferHandle f, const char* n) override { Label(GL_FRAMEBUFFER, f.id, n); }
		void PushDebugGroup(const char* name) override { if (m_debugLabels) glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name); }
		void PopDebugGroup() override { if (m_debugLabels) glPopDebugGroup(); }

		// --- Editor UI ---------------------------------------------------------------------------------
		bool ImGuiInit() override {
			if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_context)) return false;
			return ImGui_ImplOpenGL3_Init("#version 460 core");
		}
		void ImGuiShutdown() override {
			ImGui_ImplOpenGL3_Shutdown();
			ImGui_ImplSDL2_Shutdown();
		}
		void ImGuiNewFrame() override {
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplSDL2_NewFrame();
		}
		void ImGuiRender(ImDrawData* drawData) override { ImGui_ImplOpenGL3_RenderDrawData(drawData); }
		void ImGuiRebuildFonts() override {
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImGui_ImplOpenGL3_CreateFontsTexture();
		}
		void* ImGuiTextureId(TextureHandle t) const override { return reinterpret_cast<void*>(static_cast<intptr_t>(t.id)); }

	private:

		SDL_Window* m_window = nullptr;
		SDL_GLContext m_context = nullptr;
		DeviceInfo m_info;
		DeviceLimits m_limits;
		bool m_debugLabels = false;
		GLuint m_emptyVao = 0;
		std::unordered_map<GLuint, TextureRecord> m_textures;
		std::unordered_map<GLuint, size_t> m_bufferSizes;
		std::unordered_map<GLuint, VertexInputRecord> m_vertexInputs;
		std::unordered_map<GLuint, QueryType> m_queries;
	};

} // namespace

uint32_t PrepareWindow(bool debug) {
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	if (debug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
	return SDL_WINDOW_OPENGL;
}

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info) {
	if (!info.window) return nullptr;
	SDL_GLContext context = SDL_GL_CreateContext(info.window);
	if (!context) {
		std::cerr << "[RHI/GL] OpenGL context could not be created: " << SDL_GetError() << std::endl;
		return nullptr;
	}
	auto device = std::make_unique<GLDevice>(info.window, context);
	if (!device->InitAPI(info.vsync)) return nullptr;
	std::cout << "[RHI/GL] " << device->Info().backend << " | " << device->Info().renderer << std::endl;
	return device;
}

} // namespace rhi::gl
