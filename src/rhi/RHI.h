#pragma once
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

struct SDL_Window;
struct ImDrawData;

/*
* NOX rendering hardware interface.
*
* Engine code (renderer, passes, scene, editor) talks to the GPU only through rhi::Device; the graphics
* API lives behind it (src/rhi/gl is the OpenGL 4.6 backend). tools/CheckNoRawGL.ps1 enforces that no
* OpenGL call, type or constant appears outside the backend.
*
* Model: an immediate context. Resources are created and updated through the device; state is set with
* explicit setters (raster, depth, blend per render target, viewport, scissor); resources are bound by slot
* (texture, image, storage buffer, uniform buffer); draws and dispatches consume the current state.
* A backend for an explicit API (Vulkan, D3D12) implements the same calls by recording into a command
* buffer, caching pipelines by the accumulated state and building descriptor sets from the bound slots.
*
* Handles are opaque. Engine code must not interpret their ids (they may be API object names or pool
* indices). Handle {0} is "none"; FramebufferHandle {0} is the window's back buffer.
*
* Shaders are GLSL 4.6. Loose uniforms are set by location on the current program; a Vulkan backend maps
* them to a default uniform block via reflection (glslang relaxed Vulkan rules).
*/
namespace rhi {

	// ------------------------------------------------------------------------------------------------
	// Handles
	// ------------------------------------------------------------------------------------------------
	template <typename Tag>
	struct Handle {
		uint32_t id = 0;
		constexpr Handle() = default;
		constexpr explicit Handle(uint32_t value) : id(value) {}
		constexpr bool IsValid() const { return id != 0; }
		constexpr explicit operator bool() const { return id != 0; }
		constexpr bool operator==(const Handle& o) const { return id == o.id; }
		constexpr bool operator!=(const Handle& o) const { return id != o.id; }
		constexpr bool operator<(const Handle& o) const { return id < o.id; }
	};
	// Handles print their id (diagnostics only).
	template <typename Tag>
	inline std::ostream& operator<<(std::ostream& os, const Handle<Tag>& h) { return os << h.id; }

	using TextureHandle = Handle<struct TextureTag>;
	using BufferHandle = Handle<struct BufferTag>;
	using ProgramHandle = Handle<struct ProgramTag>;
	using FramebufferHandle = Handle<struct FramebufferTag>;
	using VertexInputHandle = Handle<struct VertexInputTag>;
	using QueryHandle = Handle<struct QueryTag>;

	constexpr FramebufferHandle kBackBuffer{};    // the window's default framebuffer
	using UniformLocation = int32_t;              // -1 = not present (setters ignore it)

	// ------------------------------------------------------------------------------------------------
	// Formats
	// ------------------------------------------------------------------------------------------------
	enum class Format : uint16_t {
		Unknown = 0,
		R8, R8UI, R16, R16F, R16UI, R32F, R32I, R32UI,
		RG8, RG8UI, RG16, RG16F, RG16UI, RG32F, RG32UI,
		RGB8, RGB8UI, RGB16F, RGB16UI, RGB32F, RGB32UI, R11G11B10F,
		RGBA8, RGBA8UI, RGBA16, RGBA16F, RGBA16UI, RGBA32F, RGBA32UI, RGB10A2,
		SRGB8, SRGB8_A8,
		D16, D24, D32F, D24S8, D32FS8,
	};
	bool IsDepthFormat(Format f);
	bool HasStencil(Format f);
	bool IsIntegerFormat(Format f);
	uint32_t FormatChannelCount(Format f);
	const char* FormatName(Format f);

	// Layout of CPU-side pixel data for uploads, clears and readbacks.
	enum class PixelFormat : uint8_t { R, RG, RGB, RGBA, BGRA, RInt, RGInt, RGBInt, RGBAInt, Depth, DepthStencil };
	enum class PixelType : uint8_t { UByte, Byte, UShort, Short, UInt, Int, HalfFloat, Float, UInt24_8, Float32_UInt24_8 };

	// ------------------------------------------------------------------------------------------------
	// Textures and samplers
	// ------------------------------------------------------------------------------------------------
	enum class TextureType : uint8_t { Tex1D, Tex1DArray, Tex2D, Tex2DArray, Tex3D, Cube, CubeArray, Tex2DMS, Tex2DMSArray, Rectangle };

	struct TextureDesc {
		TextureType type = TextureType::Tex2D;
		Format format = Format::RGBA8;
		int width = 1;
		int height = 1;
		int depth = 1;           // 3D depth, or array layers (cube arrays: number of cubes * 6)
		int mipLevels = 1;       // 0 = full chain
		int samples = 1;         // multisample types
		bool fixedSampleLocations = true;
		// false (default): resizable storage; only mipLevels are allocated (more appear with
		//   GenerateMipmaps), and mip-filtered sampling needs every level up to the max level.
		// true: fixed-size storage with all mipLevels; sampling clamps to the allocated levels.
		bool immutable = false;
		const char* debugName = nullptr;
	};

	enum class Filter : uint8_t { Nearest, Linear, NearestMipNearest, LinearMipNearest, NearestMipLinear, LinearMipLinear };
	enum class Wrap : uint8_t { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
	enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

	// Sampling state stored with a texture (bound together with it).
	struct SamplerDesc {
		Filter minFilter = Filter::Linear;
		Filter magFilter = Filter::Linear;
		Wrap wrapS = Wrap::Repeat;
		Wrap wrapT = Wrap::Repeat;
		Wrap wrapR = Wrap::Repeat;
		glm::vec4 borderColor{ 0.0f };
		bool compareEnabled = false;       // depth comparison (shadow samplers)
		CompareOp compareOp = CompareOp::LessEqual;
		float maxAnisotropy = 1.0f;
	};

	struct TextureInfo {
		TextureType type = TextureType::Tex2D;
		Format format = Format::Unknown;
		int width = 0, height = 0, depth = 0;
		int mipLevels = 0;
		int samples = 1;
	};

	// Region of one mip level. For arrays, cubes and cube arrays z/depth address layers (cube face = layer).
	struct TextureRegion {
		int level = 0;
		glm::ivec3 offset{ 0 };
		glm::ivec3 extent{ 0 };   // 0 components = whole level
	};

	// ------------------------------------------------------------------------------------------------
	// Buffers
	// ------------------------------------------------------------------------------------------------
	enum class BufferUsage : uint8_t { StaticDraw, StaticRead, StaticCopy, DynamicDraw, DynamicRead, DynamicCopy, StreamDraw, StreamRead, StreamCopy };

	enum class BufferStorage : uint32_t {
		None = 0,
		DynamicUpdate = 1u << 0,   // UpdateBuffer allowed
		MapRead = 1u << 1,
		MapWrite = 1u << 2,
		MapPersistent = 1u << 3,
		MapCoherent = 1u << 4,
		ClientStorage = 1u << 5,
	};
	enum class MapAccess : uint32_t {
		None = 0,
		Read = 1u << 0,
		Write = 1u << 1,
		Persistent = 1u << 2,
		Coherent = 1u << 3,
		InvalidateRange = 1u << 4,
		InvalidateBuffer = 1u << 5,
		FlushExplicit = 1u << 6,
		Unsynchronized = 1u << 7,
	};
	constexpr BufferStorage operator|(BufferStorage a, BufferStorage b) { return static_cast<BufferStorage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
	constexpr bool operator&(BufferStorage a, BufferStorage b) { return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0; }
	constexpr MapAccess operator|(MapAccess a, MapAccess b) { return static_cast<MapAccess>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
	constexpr bool operator&(MapAccess a, MapAccess b) { return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0; }

	struct BufferDesc {
		size_t size = 0;
		const void* initialData = nullptr;
		BufferUsage usage = BufferUsage::StaticDraw;   // mutable storage hint (Reallocate keeps the handle)
		bool immutable = false;                        // fixed size; storage flags apply
		BufferStorage storage = BufferStorage::None;
		const char* debugName = nullptr;
	};

	// ------------------------------------------------------------------------------------------------
	// Shaders
	// ------------------------------------------------------------------------------------------------
	enum class ShaderStage : uint8_t { Vertex, Fragment, Geometry, TessControl, TessEvaluation, Compute };

	struct ShaderSource {
		ShaderStage stage = ShaderStage::Vertex;
		std::string source;      // complete GLSL (includes resolved)
		std::string debugPath;   // for logs
	};
	struct ProgramDesc {
		std::vector<ShaderSource> stages;
		std::string debugName;
	};

	// ------------------------------------------------------------------------------------------------
	// Vertex input
	// ------------------------------------------------------------------------------------------------
	enum class ScalarType : uint8_t { Float, HalfFloat, Int, UInt, Short, UShort, Byte, UByte };

	struct VertexAttribute {
		uint32_t location = 0;
		uint32_t bufferIndex = 0;    // index into VertexInputDesc::buffers
		uint32_t components = 4;     // 1..4
		ScalarType type = ScalarType::Float;
		bool normalized = false;
		bool integer = false;        // read as int/uint in the shader (no conversion)
		uint32_t offset = 0;         // bytes from the start of a vertex/instance
	};
	struct VertexBufferBinding {
		BufferHandle buffer;
		uint32_t stride = 0;
		size_t offset = 0;           // bytes into the buffer
		uint32_t instanceDivisor = 0; // 0 = per vertex
	};
	struct VertexInputDesc {
		std::vector<VertexBufferBinding> buffers;
		std::vector<VertexAttribute> attributes;
		BufferHandle indexBuffer;
	};

	// ------------------------------------------------------------------------------------------------
	// Framebuffers
	// ------------------------------------------------------------------------------------------------
	constexpr int kMaxColorAttachments = 8;
	constexpr int kAllLayers = -1;

	struct Attachment {
		TextureHandle texture;
		int level = 0;
		int layer = kAllLayers;      // array layer / cube face / 3D slice; kAllLayers = layered attachment (or plain 2D)
	};
	enum class AttachmentPoint : uint8_t { Color0, Color1, Color2, Color3, Color4, Color5, Color6, Color7, Depth, Stencil, DepthStencil };
	inline AttachmentPoint ColorAttachment(int index) { return static_cast<AttachmentPoint>(index); }

	// Draw/read buffers: with colours, all colour attachments are drawn and Color0 is read. Without
	// colours the defaults (Color0) are kept so later SetAttachment targets receive output; depth-only
	// framebuffers call SetDrawBuffers(fb, nullptr, 0) and SetReadBuffer(fb, -1).
	struct FramebufferDesc {
		std::vector<Attachment> colors;   // Color0.. in order
		Attachment depth;                 // depth or depth-stencil texture (type decided by format)
		const char* debugName = nullptr;
	};

	enum ClearFlags : uint32_t { ClearColor = 1u << 0, ClearDepth = 1u << 1, ClearStencil = 1u << 2 };
	enum class BlitFilter : uint8_t { Nearest, Linear };

	// ------------------------------------------------------------------------------------------------
	// Pipeline state
	// ------------------------------------------------------------------------------------------------
	enum class BlendFactor : uint8_t {
		Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor, SrcAlpha, OneMinusSrcAlpha,
		DstAlpha, OneMinusDstAlpha, ConstantColor, OneMinusConstantColor, SrcAlphaSaturate, Src1Color, OneMinusSrc1Color,
	};
	enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
	enum class CullFace : uint8_t { Front, Back, FrontAndBack };
	enum class FrontFace : uint8_t { CounterClockwise, Clockwise };
	enum class PolygonMode : uint8_t { Fill, Line, Point };
	enum class Topology : uint8_t { Points, Lines, LineStrip, LineLoop, Triangles, TriangleStrip, TriangleFan };
	enum class IndexType : uint8_t { UInt8, UInt16, UInt32 };

	// Fixed-function switches that are not part of the per-draw raster/depth/blend state.
	enum class Feature : uint8_t { ProgramPointSize, DepthClamp, FramebufferSRGB, SeamlessCubemaps, StencilTest, Multisample };

	struct Rect { int x = 0, y = 0, width = 0, height = 0; };

	struct BlendState {
		bool enabled = false;
		BlendFactor srcColor = BlendFactor::One, dstColor = BlendFactor::Zero;
		BlendFactor srcAlpha = BlendFactor::One, dstAlpha = BlendFactor::Zero;
		BlendOp colorOp = BlendOp::Add, alphaOp = BlendOp::Add;
	};

	// Snapshot of the current state for save/restore around helper work (captures, editor previews).
	struct StateSnapshot {
		bool depthTest = false, depthWrite = true;
		CompareOp depthFunc = CompareOp::Less;
		bool cullEnabled = false;
		CullFace cullFace = CullFace::Back;
		FrontFace frontFace = FrontFace::CounterClockwise;
		PolygonMode polygonMode = PolygonMode::Fill;
		bool polygonOffset = false;
		float polygonOffsetFactor = 0.0f, polygonOffsetUnits = 0.0f;
		bool scissorTest = false;
		Rect scissor;
		Rect viewport;
		BlendState blend;                  // render target 0 (and all targets when not set per target)
		bool colorMask[4] = { true, true, true, true };
		bool stencilTest = false;
		bool depthClamp = false;
		bool programPointSize = false;
		bool framebufferSRGB = false;
		glm::vec4 clearColor{ 0.0f };
		FramebufferHandle drawFramebuffer;
		FramebufferHandle readFramebuffer;
		ProgramHandle program;
		VertexInputHandle vertexInput;
	};

	// ------------------------------------------------------------------------------------------------
	// Synchronisation and queries
	// ------------------------------------------------------------------------------------------------
	enum Barrier : uint32_t {
		BarrierVertexAttrib = 1u << 0,
		BarrierIndexBuffer = 1u << 1,
		BarrierUniform = 1u << 2,
		BarrierTextureFetch = 1u << 3,
		BarrierImageAccess = 1u << 4,
		BarrierCommand = 1u << 5,           // indirect draw/dispatch arguments
		BarrierPixelBuffer = 1u << 6,
		BarrierTextureUpdate = 1u << 7,
		BarrierBufferUpdate = 1u << 8,
		BarrierFramebuffer = 1u << 9,
		BarrierStorage = 1u << 10,
		BarrierAtomicCounter = 1u << 11,
		BarrierAll = 0xFFFFFFFFu,
	};

	enum class ImageAccess : uint8_t { ReadOnly, WriteOnly, ReadWrite };
	enum class QueryType : uint8_t { Timestamp, TimeElapsed, PrimitivesSubmitted, ComputeInvocations, AnySamplesPassed };

	struct DeviceInfo {
		std::string backend;     // "OpenGL 4.6"
		std::string vendor, renderer, version, shadingLanguage;
		int apiMajor = 0, apiMinor = 0;
	};
	struct DeviceLimits {
		int maxTextureSize = 0;
		int maxTextureUnits = 0;
		int maxImageUnits = 0;
		int maxColorAttachments = 0;
		int maxStorageBufferBindings = 0;
		float maxAnisotropy = 1.0f;
		glm::ivec3 maxComputeWorkGroupCount{ 0 };
		glm::ivec3 maxComputeWorkGroupSize{ 0 };
		int maxComputeWorkGroupInvocations = 0;
		int maxComputeSharedMemory = 0;
		bool pipelineStatisticsQueries = false;
		bool anisotropicFiltering = false;
	};

	// ------------------------------------------------------------------------------------------------
	// Device
	// ------------------------------------------------------------------------------------------------
	class Device {
	public:
		virtual ~Device() = default;

		// --- Device / window -------------------------------------------------------------------------
		virtual const DeviceInfo& Info() const = 0;
		virtual const DeviceLimits& Limits() const = 0;
		// Live objects this device created and has not destroyed. Leak checks read them (engine.info "gpu_objects").
		struct LiveObjectCounts { size_t buffers = 0, textures = 0, vertexInputs = 0, programs = 0; };
		virtual LiveObjectCounts LiveObjects() const { return {}; }
		virtual void Present() = 0;                         // swap the window's back buffer
		virtual void SetVSync(bool enabled) = 0;
		virtual const char* PollError() = 0;                // nullptr when no error is pending
		virtual void WaitIdle() = 0;                        // CPU waits until the GPU finished all work
		virtual void Flush() = 0;                           // submit pending work without waiting

		// --- Textures ---------------------------------------------------------------------------------
		virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
		virtual void DestroyTexture(TextureHandle& texture) = 0;   // resets the handle
		// Re-allocates a resizable texture in place (contents undefined); the handle and framebuffer
		// attachments stay valid. `mipLevels` levels are allocated (1 = base level only).
		virtual void ResizeTexture(TextureHandle texture, int width, int height, int depth, int mipLevels = 1) = 0;
		virtual bool IsTexture(TextureHandle texture) const = 0;
		virtual TextureInfo GetTextureInfo(TextureHandle texture) const = 0;
		virtual void SetSampler(TextureHandle texture, const SamplerDesc& sampler) = 0;
		virtual void SetTextureFilter(TextureHandle texture, Filter minFilter, Filter magFilter) = 0;
		virtual void SetTextureMinFilter(TextureHandle texture, Filter minFilter) = 0;
		virtual void SetTextureMagFilter(TextureHandle texture, Filter magFilter) = 0;
		virtual void SetTextureWrapAxis(TextureHandle texture, int axis, Wrap wrap) = 0;   // 0 = S, 1 = T, 2 = R
		virtual void SetTextureWrap(TextureHandle texture, Wrap s, Wrap t, Wrap r) = 0;
		virtual void SetTextureBorderColor(TextureHandle texture, const glm::vec4& color) = 0;
		virtual void SetTextureCompare(TextureHandle texture, bool enabled, CompareOp op) = 0;
		virtual void SetTextureAnisotropy(TextureHandle texture, float maxAnisotropy) = 0;
		virtual void SetTextureMipRange(TextureHandle texture, int baseLevel, int maxLevel) = 0;
		virtual void UpdateTexture(TextureHandle texture, const TextureRegion& region, PixelFormat format, PixelType type,
			const void* data, int rowAlignment = 4) = 0;
		virtual void GenerateMipmaps(TextureHandle texture) = 0;
		virtual void ClearTexture(TextureHandle texture, int level, PixelFormat format, PixelType type, const void* value) = 0;
		virtual void CopyTexture(TextureHandle src, const TextureRegion& srcRegion, TextureHandle dst, int dstLevel, const glm::ivec3& dstOffset) = 0;
		virtual void ReadTexture(TextureHandle texture, int level, PixelFormat format, PixelType type, size_t bufferSize, void* out, int rowAlignment = 4) = 0;
		virtual void ReadTextureRegion(TextureHandle texture, const TextureRegion& region, PixelFormat format, PixelType type,
			size_t bufferSize, void* out, int rowAlignment = 4) = 0;

		// --- Buffers ----------------------------------------------------------------------------------
		virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
		virtual void DestroyBuffer(BufferHandle& buffer) = 0;
		virtual bool IsBuffer(BufferHandle buffer) const = 0;
		virtual size_t GetBufferSize(BufferHandle buffer) const = 0;
		// Replaces the storage of a mutable buffer (new size, optional data); the handle stays valid.
		virtual void ReallocateBuffer(BufferHandle buffer, size_t size, const void* data, BufferUsage usage) = 0;
		virtual void UpdateBuffer(BufferHandle buffer, size_t offset, size_t size, const void* data) = 0;
		virtual void ReadBuffer(BufferHandle buffer, size_t offset, size_t size, void* out) = 0;
		virtual void CopyBuffer(BufferHandle src, size_t srcOffset, BufferHandle dst, size_t dstOffset, size_t size) = 0;
		// Fills the buffer with one element (format describes the element; value may be null for zero).
		virtual void ClearBuffer(BufferHandle buffer, Format elementFormat, PixelFormat format, PixelType type, const void* value) = 0;
		virtual void* MapBuffer(BufferHandle buffer, size_t offset, size_t size, MapAccess access) = 0;
		virtual void FlushMappedRange(BufferHandle buffer, size_t offset, size_t size) = 0;
		virtual void UnmapBuffer(BufferHandle buffer) = 0;

		// --- Programs ---------------------------------------------------------------------------------
		// Returns an invalid handle (and logs the compiler/linker output) on failure.
		virtual ProgramHandle CreateProgram(const ProgramDesc& desc) = 0;
		virtual void DestroyProgram(ProgramHandle& program) = 0;
		// Recompiles the program from desc and swaps it in under the same handle. False (and the old program stays in
		// use) when a stage fails to compile or the link fails, or when the handle is unknown or was made from another
		// name. Backends without in-place reload keep the default and report false.
		virtual bool ReloadProgram(ProgramHandle program, const ProgramDesc& desc) { (void)program; (void)desc; return false; }
		virtual bool IsProgram(ProgramHandle program) const = 0;
		virtual void UseProgram(ProgramHandle program) = 0;
		virtual UniformLocation GetUniformLocation(ProgramHandle program, const char* name) const = 0;
		// Setters act on the program bound with UseProgram.
		virtual void SetUniform(UniformLocation loc, int v) = 0;
		virtual void SetUniform(UniformLocation loc, uint32_t v) = 0;
		virtual void SetUniform(UniformLocation loc, float v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::vec2& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::vec3& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::vec4& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::ivec2& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::ivec3& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::ivec4& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::uvec2& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::uvec3& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::uvec4& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::mat3& v) = 0;
		virtual void SetUniform(UniformLocation loc, const glm::mat4& v) = 0;
		virtual void SetUniformArray(UniformLocation loc, const int* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const float* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const glm::vec2* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const glm::vec3* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const glm::vec4* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const glm::mat3* v, int count) = 0;
		virtual void SetUniformArray(UniformLocation loc, const glm::mat4* v, int count) = 0;
		// By name on the bound program (looks the location up each call; cache locations in hot paths).
		template <typename T> void SetUniform(ProgramHandle program, const char* name, const T& v) { SetUniform(GetUniformLocation(program, name), v); }

		// --- Vertex input -----------------------------------------------------------------------------
		virtual VertexInputHandle CreateVertexInput(const VertexInputDesc& desc) = 0;
		virtual void DestroyVertexInput(VertexInputHandle& input) = 0;
		// Re-points one buffer binding (e.g. after a different instance buffer is selected).
		virtual void SetVertexInputBuffer(VertexInputHandle input, uint32_t bufferIndex, const VertexBufferBinding& binding) = 0;
		virtual void SetVertexInputIndexBuffer(VertexInputHandle input, BufferHandle indexBuffer) = 0;
		virtual void BindVertexInput(VertexInputHandle input) = 0;   // invalid handle = attribute-less draws

		// --- Framebuffers -----------------------------------------------------------------------------
		virtual FramebufferHandle CreateFramebuffer(const FramebufferDesc& desc) = 0;
		virtual void DestroyFramebuffer(FramebufferHandle& framebuffer) = 0;
		virtual void SetAttachment(FramebufferHandle framebuffer, AttachmentPoint point, const Attachment& attachment) = 0;
		virtual bool IsFramebufferComplete(FramebufferHandle framebuffer, std::string* reason = nullptr) = 0;
		// Which colour attachments receive fragment outputs 0..n (-1 = none for that output).
		virtual void SetDrawBuffers(FramebufferHandle framebuffer, const int* attachments, int count) = 0;
		virtual void SetReadBuffer(FramebufferHandle framebuffer, int colorAttachment) = 0;   // -1 = none
		virtual void BindFramebuffer(FramebufferHandle framebuffer) = 0;                      // draw + read
		virtual void BindDrawFramebuffer(FramebufferHandle framebuffer) = 0;
		virtual void BindReadFramebuffer(FramebufferHandle framebuffer) = 0;
		virtual FramebufferHandle CurrentDrawFramebuffer() const = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void SetClearDepth(float depth) = 0;
		virtual void Clear(uint32_t clearFlags) = 0;                                          // current draw framebuffer
		virtual void ClearColorAttachment(int attachment, const glm::vec4& value) = 0;
		virtual void ClearColorAttachment(int attachment, const glm::uvec4& value) = 0;
		virtual void ClearDepthStencil(float depth, int stencil) = 0;
		virtual void BlitFramebuffer(FramebufferHandle src, int srcColorAttachment, const Rect& srcRect,
			FramebufferHandle dst, const Rect& dstRect, uint32_t clearFlagsMask, BlitFilter filter) = 0;
		virtual void ReadPixels(FramebufferHandle framebuffer, int colorAttachment, const Rect& rect, PixelFormat format,
			PixelType type, void* out, int rowAlignment = 4) = 0;

		// --- State ------------------------------------------------------------------------------------
		virtual void SetViewport(const Rect& viewport) = 0;
		virtual Rect GetViewport() const = 0;
		virtual void SetScissor(bool enabled, const Rect& rect = {}) = 0;   // enabled: also sets the rect
		virtual void SetScissorEnabled(bool enabled) = 0;
		virtual void SetScissorRect(const Rect& rect) = 0;
		virtual void SetDepthTest(bool enabled) = 0;
		virtual void SetDepthWrite(bool enabled) = 0;
		virtual void SetDepthFunc(CompareOp op) = 0;
		virtual void SetCullEnabled(bool enabled) = 0;
		virtual void SetCullFace(CullFace face) = 0;
		virtual void SetFrontFace(FrontFace face) = 0;
		virtual void SetPolygonMode(PolygonMode mode) = 0;
		virtual void SetPolygonOffset(bool enabled, float factor = 0.0f, float units = 0.0f) = 0;   // enabled: also sets factors
		virtual void SetPolygonOffsetEnabled(bool enabled) = 0;
		virtual void SetPolygonOffsetFactors(float factor, float units) = 0;
		virtual void SetLineWidth(float width) = 0;
		virtual void SetBlend(const BlendState& state) = 0;                      // all render targets
		virtual void SetBlend(int renderTarget, const BlendState& state) = 0;    // one render target
		virtual void SetBlendEnabled(bool enabled) = 0;                          // all targets, factors unchanged
		virtual void SetBlendEnabled(int renderTarget, bool enabled) = 0;
		// Factor / equation setters that leave the enable state alone (all render targets).
		virtual void SetBlendFunc(BlendFactor src, BlendFactor dst) = 0;
		virtual void SetBlendFuncSeparate(BlendFactor srcColor, BlendFactor dstColor, BlendFactor srcAlpha, BlendFactor dstAlpha) = 0;
		virtual void SetBlendFunc(int renderTarget, BlendFactor src, BlendFactor dst) = 0;
		virtual void SetBlendEquation(BlendOp op) = 0;
		virtual void SetBlendEquationSeparate(BlendOp colorOp, BlendOp alphaOp) = 0;
		virtual void SetColorMask(bool r, bool g, bool b, bool a) = 0;          // all render targets
		virtual void SetColorMask(int renderTarget, bool r, bool g, bool b, bool a) = 0;
		virtual void SetFeature(Feature feature, bool enabled) = 0;
		virtual bool IsDepthTestEnabled() const = 0;
		virtual bool IsCullEnabled() const = 0;
		virtual bool IsBlendEnabled() const = 0;
		virtual ProgramHandle CurrentProgram() const = 0;
		virtual TextureHandle BoundTexture(uint32_t unit, TextureType type) const = 0;   // diagnostics / save-restore
		virtual StateSnapshot CaptureState() const = 0;
		virtual void RestoreState(const StateSnapshot& state) = 0;

		// --- Resource binding -------------------------------------------------------------------------
		virtual void BindTexture(uint32_t unit, TextureHandle texture) = 0;      // invalid handle unbinds
		virtual void BindImage(uint32_t unit, TextureHandle texture, int level, bool layered, int layer,
			ImageAccess access, Format format) = 0;
		virtual void BindStorageBuffer(uint32_t slot, BufferHandle buffer, size_t offset = 0, size_t size = 0) = 0;
		virtual void BindUniformBuffer(uint32_t slot, BufferHandle buffer, size_t offset = 0, size_t size = 0) = 0;
		virtual void BindAtomicCounterBuffer(uint32_t slot, BufferHandle buffer, size_t offset = 0, size_t size = 0) = 0;

		// --- Draw / dispatch --------------------------------------------------------------------------
		virtual void Draw(Topology topology, uint32_t firstVertex, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstInstance = 0) = 0;
		virtual void DrawIndexed(Topology topology, uint32_t indexCount, IndexType indexType, size_t indexByteOffset,
			uint32_t instanceCount = 1, int32_t baseVertex = 0, uint32_t firstInstance = 0) = 0;
		// Indirect commands (DrawElementsIndirectCommand layout) read from `args`.
		virtual void DrawIndexedIndirect(Topology topology, IndexType indexType, BufferHandle args, size_t byteOffset,
			uint32_t drawCount, uint32_t stride) = 0;
		virtual void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;
		virtual void DispatchIndirect(BufferHandle args, size_t byteOffset) = 0;
		virtual void PipelineBarrier(uint32_t barriers) = 0;   // Barrier bits: make prior writes visible

		// --- Queries ----------------------------------------------------------------------------------
		virtual QueryHandle CreateQuery(QueryType type) = 0;
		virtual void DestroyQuery(QueryHandle& query) = 0;
		virtual void WriteTimestamp(QueryHandle query) = 0;
		virtual void BeginQuery(QueryHandle query) = 0;
		virtual void EndQuery(QueryHandle query) = 0;
		virtual bool IsQueryResultAvailable(QueryHandle query) = 0;
		virtual uint64_t GetQueryResult(QueryHandle query) = 0;      // waits if not available

		// --- Debug ------------------------------------------------------------------------------------
		virtual void SetDebugName(TextureHandle texture, const char* name) = 0;
		virtual void SetDebugName(BufferHandle buffer, const char* name) = 0;
		virtual void SetDebugName(ProgramHandle program, const char* name) = 0;
		virtual void SetDebugName(FramebufferHandle framebuffer, const char* name) = 0;
		virtual void PushDebugGroup(const char* name) = 0;
		virtual void PopDebugGroup() = 0;

		// --- Editor UI (Dear ImGui renderer for this backend) -------------------------------------------
		virtual bool ImGuiInit() = 0;
		virtual void ImGuiShutdown() = 0;
		virtual void ImGuiNewFrame() = 0;
		virtual void ImGuiRender(ImDrawData* drawData) = 0;
		// Re-uploads the ImGui font atlas after fonts were added once frames have been rendered.
		virtual void ImGuiRebuildFonts() = 0;
		virtual void* ImGuiTextureId(TextureHandle texture) const = 0;   // value for ImGui::Image (ImTextureID)
	};

	// ------------------------------------------------------------------------------------------------
	// Creation and global access
	// ------------------------------------------------------------------------------------------------
	enum class Backend : uint8_t { OpenGL };

	struct DeviceCreateInfo {
		Backend backend = Backend::OpenGL;
		SDL_Window* window = nullptr;
		bool debug = false;
		bool vsync = false;
	};

	// SDL window flags and pre-window attributes the backend needs (call before SDL_CreateWindow).
	uint32_t PrepareWindow(Backend backend, bool debug);
	std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info);

	// The engine's device (set once at startup). All engine rendering code uses this.
	Device& Gpu();
	bool HasGpu();
	void SetGpu(Device* device);

} // namespace rhi
