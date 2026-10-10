#include "Skybox.h"
#include "ShaderLoader.h"
#include "stb_image.h"
#include "TextureUnits.h"
#include "FrameBuffer.h"
#include "GpuBuffer.h"
#include <iostream>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
	// Fixed raster state for the cubemap/IBL captures. A skybox can be (re)built in the middle of a
	// running frame (scene switch in the editor), where culling, scissor (ImGui), blending, colour
	// masks and depth state are whatever the last pass left; the capture cube is drawn from inside,
	// so inherited back-face culling or a scissor rectangle would leave faces black. Restores on exit.
	class CaptureStateScope {
	public:
		CaptureStateScope() : m_saved(rhi::Gpu().CaptureState()) {
			rhi::Device& gpu = rhi::Gpu();
			gpu.SetCullEnabled(false);
			gpu.SetScissorEnabled(false);
			gpu.SetBlendEnabled(false);
			gpu.SetDepthTest(false);
			gpu.SetFeature(rhi::Feature::StencilTest, false);
			gpu.SetDepthWrite(true);
			gpu.SetColorMask(true, true, true, true);
			gpu.SetPolygonMode(rhi::PolygonMode::Fill);
		}
		~CaptureStateScope() {
			rhi::Gpu().RestoreState(m_saved);
		}
	private:
		rhi::StateSnapshot m_saved;
	};

	// Full render state save/restore around the IBL generation helpers (replaces the former GL state
	// guards, which were scoped to the whole helper).
	// RHI-TODO: StateSnapshot does not cover texture-unit bindings; the former guards also restored them.
	class RenderStateScope {
	public:
		RenderStateScope() : m_saved(rhi::Gpu().CaptureState()) {}
		~RenderStateScope() {
			rhi::Gpu().RestoreState(m_saved);
		}
	private:
		rhi::StateSnapshot m_saved;
	};
}

void Skybox::CreateCaptureFBO(int width, int height)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!m_captureFBO)
		m_captureFBO = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
	if (!m_captureRBO) {
		rhi::TextureDesc depthDesc;
		depthDesc.format = rhi::Format::D24;
		depthDesc.width = width;
		depthDesc.height = height;
		m_captureRBO = gpu.CreateTexture(depthDesc);
	}
	gpu.BindFramebuffer(m_captureFBO);
	gpu.ResizeTexture(m_captureRBO, width, height, 1, 1);
	gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Depth, rhi::Attachment{ m_captureRBO });
	std::string reason;
	if (!gpu.IsFramebufferComplete(m_captureFBO, &reason))
		std::cerr << "[Skybox] Capture FBO incomplete: " << reason << std::endl;
	gpu.BindFramebuffer(rhi::kBackBuffer);
}

void Skybox::ResizeCaptureRBO(int width, int height)
{
	rhi::Gpu().ResizeTexture(m_captureRBO, width, height, 1, 1);
}

TexturePtr Skybox::loadHDRTexture(const std::string& hdrPath)
{
	int width, height, nComponents;
	float* data = stbi_loadf(hdrPath.c_str(), &width, &height, &nComponents, 0);
	if (!data) {
		std::cerr << "[Skybox] Failed to load HDR file: " << hdrPath << std::endl;
		return nullptr;
	}

	// Use the new Texture builder for HDR loading
	rhi::PixelFormat format = (nComponents >= 4) ? rhi::PixelFormat::RGBA : rhi::PixelFormat::RGB;

	auto hdrTexture = Texture::Builder::Texture2D(width, height, rhi::Format::RGB16F)
		.Format(format)
		.DataType(rhi::PixelType::Float)
		.Data(data)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.WrapMode(rhi::Wrap::ClampToEdge)
		.TextureType(TextureType::HDR)
		.Build();

	stbi_image_free(data);

	std::cout << "[Skybox] Loaded HDR texture: " << width << "x" << height
		<< " (" << nComponents << " channels)" << std::endl;

	return hdrTexture;
}

void Skybox::buildSkyboxCube()
{
	if (m_skyboxVAO)
		return; // Already built

	float skyboxVertices[] = {
		// positions
		-1.f,-1.f, 1.f,  1.f,-1.f, 1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f,-1.f, 1.f,
		-1.f, 1.f,-1.f,  1.f, 1.f,-1.f,  1.f,-1.f,-1.f,  1.f,-1.f,-1.f, -1.f,-1.f,-1.f, -1.f, 1.f,-1.f,
		 1.f,-1.f,-1.f,1.f, 1.f,-1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f,  1.f,-1.f, 1.f,  1.f,-1.f,-1.f,
		-1.f,-1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f,-1.f, -1.f, 1.f,-1.f, -1.f,-1.f,-1.f, -1.f,-1.f, 1.f,
		-1.f, 1.f,-1.f,  1.f, 1.f,-1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f,-1.f,
		-1.f,-1.f, 1.f,  1.f,-1.f, 1.f,  1.f,-1.f,-1.f,  1.f,-1.f,-1.f, -1.f,-1.f,-1.f, -1.f,-1.f, 1.f
	};

	// Create vertex buffer using GpuBuffer wrapper
	m_skyboxVBO = std::make_unique<GpuBuffer>(
		BufferType::Vertex,
		sizeof(skyboxVertices),
		skyboxVertices,
		BufferUsage::StaticDraw
	);
	m_skyboxVBO->SetLabel("Skybox_CubeVBO");

	rhi::VertexBufferBinding binding;
	binding.buffer = m_skyboxVBO->GetID();
	binding.stride = static_cast<uint32_t>(3 * sizeof(float));
	rhi::VertexAttribute position;
	position.location = 0;
	position.components = 3;
	position.type = rhi::ScalarType::Float;
	position.offset = 0;
	rhi::VertexInputDesc vaoDesc;
	vaoDesc.buffers.push_back(binding);
	vaoDesc.attributes.push_back(position);
	m_skyboxVAO = rhi::Gpu().CreateVertexInput(vaoDesc);
}

void Skybox::renderCube()
{
	rhi::Device& gpu = rhi::Gpu();
	gpu.BindVertexInput(m_skyboxVAO);
	gpu.Draw(rhi::Topology::Triangles, 0, 36);
	gpu.BindVertexInput(rhi::VertexInputHandle{});
}

bool Skybox::Init(const std::string& hdrPath,
	const std::string& equirectVertShader,
	const std::string& equirectFragShader,
	const std::string& skyboxVertShader,
	const std::string& skyboxFragShader,
	int windowWidth,
	int windowHeight)
{
	std::cout << "[Skybox] Initializing skybox with IBL support..." << std::endl;
	std::cout << "[Skybox] HDR file: " << hdrPath << std::endl;
	rhi::Gpu().SetFeature(rhi::Feature::SeamlessCubemaps, true);

	// Store the HDR path for serialization
	m_hdrPath = hdrPath;

	m_pipelineReady = false;
	m_regenAttempted = false;

	// Load core shaders
	m_equiRectToCubeShader = CreateShaderProgram(equirectVertShader.c_str(), equirectFragShader.c_str());
	if (!m_equiRectToCubeShader) {
		std::cerr << "[Skybox] ERROR: Failed to compile equirect->cube shader" << std::endl;
		return false;
	}

	m_skyboxShader = CreateShaderProgram(skyboxVertShader.c_str(), skyboxFragShader.c_str());
	if (!m_skyboxShader) {
		std::cerr << "[Skybox] ERROR: Failed to compile skybox shader" << std::endl;
		return false;
	}

	// Load HDR using new Texture class
	auto hdrTexture = loadHDRTexture(hdrPath);
	if (!hdrTexture) {
		std::cerr << "[Skybox] ERROR: HDR load failed" << std::endl;
		return false;
	}

	// Use higher resolution for environment map (512 -> 1024) for better quality
	const int envMapSize = 1024;

	// Create environment cubemap using new Texture builder
	m_envCubemap = Texture::Builder::TextureCube(envMapSize, rhi::Format::RGB16F)
		.FilterMode(rhi::Filter::LinearMipLinear, rhi::Filter::Linear)
		.TextureType(TextureType::Cubemap)
		.GenerateMipmaps(false)  // We'll generate after conversion
		.Build();

	buildSkyboxCube();
	CreateCaptureFBO(envMapSize, envMapSize);

	rhi::Device& gpu = rhi::Gpu();

	// Save state (the RHI binds textures by slot and has no active texture unit to save)
	const CaptureStateScope captureState;
	const rhi::Rect prevViewport = gpu.GetViewport();
	const rhi::FramebufferHandle prevFBO = gpu.CurrentDrawFramebuffer();
	const rhi::ProgramHandle prevProg = gpu.CurrentProgram();

	// Convert HDR equirect to cubemap
	gpu.UseProgram(m_equiRectToCubeShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_equiRectToCubeShader, "equirectangularMap"), 0);

	// Bind HDR texture
	hdrTexture->Bind(0);

	glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	gpu.SetUniform(gpu.GetUniformLocation(m_equiRectToCubeShader, "projection"), captureProjection);

	glm::mat4 captureViews[] = {
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
	 glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
	};

	gpu.SetViewport({ 0, 0, envMapSize, envMapSize });
	gpu.BindFramebuffer(m_captureFBO);

	for (int i = 0; i < 6; ++i) {
		gpu.SetUniform(gpu.GetUniformLocation(m_equiRectToCubeShader, "view"), captureViews[i]);
		gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_envCubemap->ID(), 0, i });

		if (!gpu.IsFramebufferComplete(m_captureFBO)) {
			std::cerr << "[Skybox] ERROR: Env conversion FBO incomplete (face " << i << ")" << std::endl;
			return false;
		}

		gpu.Clear(rhi::ClearColor | rhi::ClearDepth);
		renderCube();
	}

	// Generate mipmaps for environment map
	m_envCubemap->GenerateMipmaps();

	// Immediately generate IBL resources (strict requirement)
	std::cout << "[Skybox] Generating IBL resources with CORRECTED shaders..." << std::endl;
	if (!GenerateIBLResources() || !VerifyIBLPipelineComplete()) {
		std::cerr << "[Skybox] FATAL: Initial IBL generation failed - attempting forced regeneration..." << std::endl;
		ForceRegenerateIBL();
		if (!VerifyIBLPipelineComplete()) {
			std::cerr << "[Skybox] FATAL: Skybox pipeline invalid after forced regeneration." << std::endl;
			return false;
		}
	}
	m_pipelineReady = true;

	// Restore state
	gpu.BindFramebuffer(prevFBO);
	gpu.SetViewport(prevViewport);
	gpu.UseProgram(prevProg);

	// Pre-bind environment map sampler location
	gpu.UseProgram(m_skyboxShader);
	SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);
	gpu.UseProgram(prevProg);

	LogTextureInfo();
	std::cout << "[Skybox] Skybox + IBL fully initialized with CORRECTED energy conservation." << std::endl;
	return true;
}

void Skybox::Draw(const glm::mat4& view, const glm::mat4& projection)
{
	// Fast readiness check
	if (!m_pipelineReady) {
		if (!EnsureReady()) {
			return;
		}
	}

	if (!m_envCubemap || !m_envCubemap->IsValid()) {
		return;
	}

	// Verify skybox VAO exists (may have been cleaned up during scene switch)
	if (!m_skyboxVAO) {
		buildSkyboxCube();
	}

	// Use comprehensive state management for skybox rendering
	RenderStateScope renderState;

	// Remove translation from view matrix
	glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));

	rhi::Device& gpu = rhi::Gpu();

	// Set up optimal state for skybox rendering
	// Must enable depth test and set proper depth function
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::LessEqual);  // Skybox renders at max depth (z=w in clip space)
	gpu.SetDepthWrite(false);   // Don't write to depth buffer
	gpu.SetCullEnabled(false); // Render all faces of the cube
	gpu.SetBlendEnabled(false);     // No blending needed for skybox

	// Render the skybox
	gpu.UseProgram(m_skyboxShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_skyboxShader, "view"), viewNoTrans);
	gpu.SetUniform(gpu.GetUniformLocation(m_skyboxShader, "projection"), projection);
	gpu.SetUniform(gpu.GetUniformLocation(m_skyboxShader, "skyboxExposure"), m_skyboxExposure);

	// Bind environment cubemap using new Texture class
	m_envCubemap->Bind(TextureUnits::SKYBOX_CUBEMAP);
	SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);

	renderCube();

	// Restore depth mask to prevent issues with subsequent rendering
	gpu.SetDepthWrite(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);
	gpu.SetCullEnabled(true);
}

void Skybox::Cleanup()
{
	rhi::Device& gpu = rhi::Gpu();

	// GpuBuffer handles its own cleanup via RAII
	m_skyboxVBO.reset();
	if (m_skyboxVAO) gpu.DestroyVertexInput(m_skyboxVAO);
	m_skyboxVAO = rhi::VertexInputHandle{};

	// New Texture class handles cleanup automatically via smart pointers
	m_envCubemap.reset();
	m_irradianceMap.reset();
	m_prefilteredMap.reset();
	m_brdfLUT.reset();

	if (m_equiRectToCubeShader) gpu.DestroyProgram(m_equiRectToCubeShader);
	if (m_skyboxShader) gpu.DestroyProgram(m_skyboxShader);
	if (m_irradianceShader) gpu.DestroyProgram(m_irradianceShader);
	if (m_prefilterShader) gpu.DestroyProgram(m_prefilterShader);
	if (m_brdfShader) gpu.DestroyProgram(m_brdfShader);
	if (m_captureFBO) gpu.DestroyFramebuffer(m_captureFBO);
	if (m_captureRBO) gpu.DestroyTexture(m_captureRBO);

	m_equiRectToCubeShader = rhi::ProgramHandle{};
	m_skyboxShader = rhi::ProgramHandle{};
	m_irradianceShader = rhi::ProgramHandle{};
	m_prefilterShader = rhi::ProgramHandle{};
	m_brdfShader = rhi::ProgramHandle{};
	m_captureFBO = rhi::FramebufferHandle{};
	m_captureRBO = rhi::TextureHandle{};

	m_pipelineReady = false;
}

bool Skybox::ValidateIBLTextures() const
{
	// Delegate to the faster verification method
	return VerifyIBLPipelineComplete();
}

void Skybox::LogTextureInfo() const
{
	std::cout << "[Skybox] IBL Texture Information:" << std::endl;
	std::cout << "  Environment Map: " << (m_envCubemap ? m_envCubemap->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "  Irradiance Map: " << (m_irradianceMap ? m_irradianceMap->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "  Prefiltered Map: " << (m_prefilteredMap ? m_prefilteredMap->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "  BRDF LUT: " << (m_brdfLUT ? m_brdfLUT->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "  Max LOD: " << m_prefilteredMaxLOD << std::endl;
}

bool Skybox::GenerateIBLResources()
{
	std::cout << "[Skybox] Generating IBL resources..." << std::endl;

	// Verify shader files exist and load properly
	std::cout << "[Skybox] Loading irradiance convolution shader..." << std::endl;
	m_irradianceShader = CreateShaderProgram("shaders/irradiance_convolution_vert.glsl", "shaders/irradiance_convolution_frag.glsl");
	if (!m_irradianceShader) {
		std::cerr << "[Skybox] Failed to load irradiance convolution shader!" << std::endl;
		return false;
	}
	std::cout << "[Skybox]  Irradiance shader loaded: " << m_irradianceShader << std::endl;

	std::cout << "[Skybox] Loading prefilter shader..." << std::endl;
	m_prefilterShader = CreateShaderProgram("shaders/prefilter_vert.glsl", "shaders/prefilter_frag.glsl");
	if (!m_prefilterShader) {
		std::cerr << "[Skybox] Failed to load prefilter shader!" << std::endl;
		return false;
	}
	std::cout << "[Skybox]  Prefilter shader loaded: " << m_prefilterShader << std::endl;

	std::cout << "[Skybox] Loading BRDF integration shader..." << std::endl;
	m_brdfShader = CreateShaderProgram("shaders/brdf_vert.glsl", "shaders/brdf_frag.glsl");
	if (!m_brdfShader) {
		std::cerr << "[Skybox] Failed to load BRDF shader!" << std::endl;
		return false;
	}
	std::cout << "[Skybox]  BRDF shader loaded: " << m_brdfShader << std::endl;

	std::cout << "[Skybox] Starting IBL resource generation..." << std::endl;

	if (!GenerateIrradianceMap()) {
		std::cerr << "[Skybox] Irradiance map generation failed!" << std::endl;
		return false;
	}

	if (!GeneratePrefilteredMap()) {
		std::cerr << "[Skybox] Prefiltered map generation failed!" << std::endl;
		return false;
	}

	if (!GenerateBRDFLUT()) {
		std::cerr << "[Skybox] BRDF LUT generation failed!" << std::endl;
		return false;
	}

	std::cout << "[Skybox]  All IBL resources generated successfully!" << std::endl;
	std::cout << "[Skybox] - Irradiance Map: " << (m_irradianceMap ? m_irradianceMap->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "[Skybox] - Prefiltered Map: " << (m_prefilteredMap ? m_prefilteredMap->ID() : rhi::TextureHandle{}) << std::endl;
	std::cout << "[Skybox] - BRDF LUT: " << (m_brdfLUT ? m_brdfLUT->ID() : rhi::TextureHandle{}) << std::endl;
	return true;
}

bool Skybox::GenerateIrradianceMap()
{
	std::cout << "[Skybox] Generating irradiance map..." << std::endl;

	// Use comprehensive state management
	RenderStateScope renderState;

	// Create irradiance cubemap using new Texture builder
	m_irradianceMap = Texture::Builder::TextureCube(32, rhi::Format::RGB16F)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.TextureType(TextureType::Cubemap)
		.Build();

	ResizeCaptureRBO(32, 32);

	glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	glm::mat4 views[] = {  // Capture views
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
	};

	rhi::Device& gpu = rhi::Gpu();

	gpu.UseProgram(m_irradianceShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_irradianceShader, "environmentMap"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(m_irradianceShader, "projection"), proj);

	// Bind source environment map to unit 0
	m_envCubemap->Bind(0);

	gpu.SetDepthTest(false);
	gpu.SetCullEnabled(false);
	gpu.SetViewport({ 0, 0, 32, 32 });
	gpu.BindFramebuffer(m_captureFBO);

	std::cout << "[Skybox] Starting irradiance convolution..." << std::endl;
	for (int face = 0; face < 6; ++face) {
		gpu.SetUniform(gpu.GetUniformLocation(m_irradianceShader, "view"), views[face]);
		gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_irradianceMap->ID(), 0, face });

		std::string reason;
		if (!gpu.IsFramebufferComplete(m_captureFBO, &reason)) {
			std::cerr << "[Skybox] Irradiance FBO incomplete (face " << face << ") " << reason << std::endl;
			return false;
		}

		gpu.Clear(rhi::ClearColor);
		renderCube();
	}

	// Enhanced debug readback with validation
	float pixel[3] = { 0,0,0 };
	rhi::FramebufferHandle debugFBO = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
	gpu.BindFramebuffer(debugFBO);
	gpu.SetAttachment(debugFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_irradianceMap->ID(), 0, 0 });

	if (gpu.IsFramebufferComplete(debugFBO)) {
		gpu.ReadPixels(debugFBO, 0, { 16, 16, 1, 1 }, rhi::PixelFormat::RGB, rhi::PixelType::Float, pixel);
		std::cout << "[Skybox] Irradiance debug sample = (" << pixel[0] << ", " << pixel[1] << ", " << pixel[2] << ")" << std::endl;

		if (pixel[0] < 0.0f || pixel[1] < 0.0f || pixel[2] < 0.0f) {
			std::cerr << "[Skybox] WARNING: Irradiance contains negative values! This indicates a shader error." << std::endl;
		}

		if (std::isnan(pixel[0]) || std::isnan(pixel[1]) || std::isnan(pixel[2])) {
			std::cerr << "[Skybox] ERROR: Irradiance contains NaN values! IBL will be corrupted." << std::endl;
			gpu.BindFramebuffer(m_captureFBO);
			gpu.DestroyFramebuffer(debugFBO);
			return false;
		}
	}

	gpu.BindFramebuffer(m_captureFBO);
	gpu.DestroyFramebuffer(debugFBO);

	std::cout << "[Skybox] Irradiance map generated (32x32)" << std::endl;
	return true;
}

bool Skybox::GeneratePrefilteredMap()
{
	std::cout << "[Skybox] Generating prefiltered map..." << std::endl;

	RenderStateScope renderState;

	const unsigned int baseSize = 128;
	const unsigned int maxMipLevels = 5;

	// Create prefiltered cubemap with mipmaps using builder
	m_prefilteredMap = Texture::Builder::TextureCube(baseSize, rhi::Format::RGB16F)
		.FilterMode(rhi::Filter::LinearMipLinear, rhi::Filter::Linear)
		.TextureType(TextureType::Cubemap)
		.GenerateMipmaps(false)  // We'll manually create mip levels
		.Build();

	rhi::Device& gpu = rhi::Gpu();

	// Manually create all mip levels (storage for every face and level, contents undefined)
	gpu.ResizeTexture(m_prefilteredMap->ID(), static_cast<int>(baseSize), static_cast<int>(baseSize), 1, static_cast<int>(maxMipLevels));

	// Set max mip level
	m_prefilteredMap->SetMipmapRange(0, maxMipLevels - 1);

	// Create lightweight FBO
	rhi::FramebufferHandle fbo = gpu.CreateFramebuffer(rhi::FramebufferDesc{});

	glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	glm::mat4 views[] = {
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
	 glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
	};

	gpu.UseProgram(m_prefilterShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "environmentMap"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "projection"), proj);
	// Face size of the source cubemap (mip 0); drives the per-sample mip selection in the shader.
	gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "resolution"), static_cast<float>(m_envCubemap->Width()));

	// Bind environment map to unit 0
	m_envCubemap->Bind(0);

	gpu.SetDepthTest(false);
	gpu.SetCullEnabled(false);

	for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
		unsigned int w = baseSize >> mip;
		unsigned int h = baseSize >> mip;
		gpu.SetViewport({ 0, 0, static_cast<int>(w), static_cast<int>(h) });

		// Perceptual roughness: the lighting shaders fetch mip = perceptualRoughness * maxLOD and
		// ImportanceSampleGGX squares its input into alpha, so the mip ladder must be linear here.
		float roughness = float(mip) / float(maxMipLevels - 1);
		gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "roughness"), roughness);

		std::cout << "[Skybox] Prefilter mip " << mip << " size=" << w << "x" << h
			<< " roughness=" << roughness << std::endl;

		for (int face = 0; face < 6; ++face) {
			gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "view"), views[face]);
			gpu.BindFramebuffer(fbo);
			gpu.SetAttachment(fbo, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_prefilteredMap->ID(), static_cast<int>(mip), face });

			const int drawBuf = 0;   // colour attachment 0
			gpu.SetDrawBuffers(fbo, &drawBuf, 1);

			std::string reason;
			if (!gpu.IsFramebufferComplete(fbo, &reason)) {
				std::cerr << "[Skybox] Prefilter FBO incomplete mip=" << mip << " face=" << face
					<< " " << reason << std::endl;
				gpu.DestroyFramebuffer(fbo);
				return false;
			}

			gpu.Clear(rhi::ClearColor);
			renderCube();
		}
	}

	// Debug sample
	float pix[3] = { 0,0,0 };
	rhi::FramebufferHandle dbgFBO = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
	gpu.BindFramebuffer(dbgFBO);
	gpu.SetAttachment(dbgFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_prefilteredMap->ID(), 0, 0 });

	if (gpu.IsFramebufferComplete(dbgFBO)) {
		gpu.ReadPixels(dbgFBO, 0, { 64, 64, 1, 1 }, rhi::PixelFormat::RGB, rhi::PixelType::Float, pix);
		std::cout << "[Skybox] Prefilter debug sample = (" << pix[0] << ", " << pix[1] << ", " << pix[2] << ")" << std::endl;

		if (pix[0] < 0.0f || pix[1] < 0.0f || pix[2] < 0.0f) {
			std::cerr << "[Skybox] WARNING: Prefiltered map contains negative values!" << std::endl;
		}

		if (std::isnan(pix[0]) || std::isnan(pix[1]) || std::isnan(pix[2])) {
			std::cerr << "[Skybox] ERROR: Prefiltered map contains NaN values!" << std::endl;
			gpu.BindFramebuffer(fbo);
			gpu.DestroyFramebuffer(dbgFBO);
			gpu.DestroyFramebuffer(fbo);
			return false;
		}
	}

	gpu.BindFramebuffer(fbo);
	gpu.DestroyFramebuffer(dbgFBO);
	gpu.DestroyFramebuffer(fbo);

	m_prefilteredMaxLOD = maxMipLevels - 1;
	std::cout << "[Skybox] Prefiltered map generation completed (" << baseSize << " base, "
		<< maxMipLevels << " mips)" << std::endl;
	return true;
}

bool Skybox::GenerateBRDFLUT()
{
	std::cout << "[Skybox] Generating BRDF LUT..." << std::endl;

	RenderStateScope renderState;

	// Create BRDF integration lookup table using new Texture builder
	m_brdfLUT = Texture::Builder::Texture2D(512, 512, rhi::Format::RG16F)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.WrapMode(rhi::Wrap::ClampToEdge)
		.TextureType(TextureType::Custom)
		.Build();

	rhi::Device& gpu = rhi::Gpu();

	// Full screen quad for BRDF integration
	rhi::VertexInputHandle quadVAO;
	float quadVertices[] = {
		-1.f,  1.f, 0.f, 0.f, 1.f,
		-1.f, -1.f, 0.f, 0.f, 0.f,
	   1.f,  1.f, 0.f, 1.f, 1.f,
 1.f, -1.f, 0.f, 1.f, 0.f
	};

	// Use GpuBuffer wrapper for temporary quad VBO
	GpuBuffer quadVBO(BufferType::Vertex, sizeof(quadVertices), quadVertices, BufferUsage::StaticDraw);

	rhi::VertexBufferBinding quadBinding;
	quadBinding.buffer = quadVBO.GetID();
	quadBinding.stride = static_cast<uint32_t>(5 * sizeof(float));
	rhi::VertexAttribute quadPosition;
	quadPosition.location = 0;
	quadPosition.components = 3;
	quadPosition.type = rhi::ScalarType::Float;
	quadPosition.offset = 0;
	rhi::VertexAttribute quadTexcoord;
	quadTexcoord.location = 1;
	quadTexcoord.components = 2;
	quadTexcoord.type = rhi::ScalarType::Float;
	quadTexcoord.offset = static_cast<uint32_t>(3 * sizeof(float));
	rhi::VertexInputDesc quadDesc;
	quadDesc.buffers.push_back(quadBinding);
	quadDesc.attributes.push_back(quadPosition);
	quadDesc.attributes.push_back(quadTexcoord);
	quadVAO = gpu.CreateVertexInput(quadDesc);
	gpu.BindVertexInput(quadVAO);

	ResizeCaptureRBO(512, 512);

	gpu.SetDepthTest(false);
	gpu.SetCullEnabled(false);

	gpu.SetViewport({ 0, 0, 512, 512 });
	gpu.BindFramebuffer(m_captureFBO);
	gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_brdfLUT->ID(), 0, rhi::kAllLayers });

	std::string reason;
	if (!gpu.IsFramebufferComplete(m_captureFBO, &reason)) {
		std::cerr << "[Skybox] BRDF LUT FBO incomplete: " << reason << std::endl;
		gpu.DestroyVertexInput(quadVAO);
		// GpuBuffer handles its own cleanup via RAII
		return false;
	}

	gpu.UseProgram(m_brdfShader);
	gpu.Clear(rhi::ClearColor | rhi::ClearDepth);
	gpu.Draw(rhi::Topology::TriangleStrip, 0, 4);

	gpu.DestroyVertexInput(quadVAO);
	// GpuBuffer handles its own cleanup via RAII

	// Debug sample
	float debugPixel[2] = { 0, 0 };
	rhi::FramebufferHandle debugFBO = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
	gpu.BindFramebuffer(debugFBO);
	gpu.SetAttachment(debugFBO, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_brdfLUT->ID(), 0, rhi::kAllLayers });

	if (gpu.IsFramebufferComplete(debugFBO)) {
		gpu.ReadPixels(debugFBO, 0, { 256, 256, 1, 1 }, rhi::PixelFormat::RG, rhi::PixelType::Float, debugPixel);
		std::cout << "[Skybox] BRDF LUT debug sample = (" << debugPixel[0] << ", " << debugPixel[1] << ")" << std::endl;
	}

	gpu.BindFramebuffer(m_captureFBO);
	gpu.DestroyFramebuffer(debugFBO);

	std::cout << "[Skybox] BRDF LUT generated successfully (512x512)" << std::endl;
	return true;
}

bool Skybox::VerifyIBLPipelineComplete() const
{
	// Check environment cubemap first
	if (!m_envCubemap || !m_envCubemap->IsValid()) {
		return false;
	}

	// Check all IBL textures using new Texture class
	if (!m_irradianceMap || !m_irradianceMap->IsValid()) {
		return false;
	}

	if (!m_prefilteredMap || !m_prefilteredMap->IsValid()) {
		return false;
	}

	if (!m_brdfLUT || !m_brdfLUT->IsValid()) {
		return false;
	}

	// Check shaders are loaded
	if (!m_skyboxShader) {
		return false;
	}

	if (!m_irradianceShader || !m_prefilterShader || !m_brdfShader) {
		return false;
	}

	// Check cube geometry
	if (!m_skyboxVAO || m_skyboxVBO == 0) {
		return false;
	}

	// Validate environment cubemap has content (using new Texture class)
	if (m_envCubemap->Width() == 0 || m_envCubemap->Height() == 0) {
		return false;
	}

	return true;
}

void Skybox::ForceRegenerateIBL()
{
	std::cout << "[Skybox] FORCING complete IBL pipeline regeneration..." << std::endl;
	const CaptureStateScope captureState;

	// Clean up existing IBL resources (automatic via smart pointers)
	m_irradianceMap.reset();
	m_prefilteredMap.reset();
	m_brdfLUT.reset();

	// Recreate all IBL resources
	if (!GenerateIBLResources()) {
		std::cerr << "[Skybox] Failed to regenerate IBL resources!" << std::endl;
		return;
	}

	// Validate everything was created
	if (!ValidateIBLTextures()) {
		std::cerr << "[Skybox] IBL validation failed after regeneration!" << std::endl;
		return;
	}

	std::cout << "[Skybox]  IBL pipeline regeneration completed successfully!" << std::endl;
}

bool Skybox::EnsureReady()
{
	if (m_pipelineReady) return true;

	if (!m_regenAttempted) {
		m_regenAttempted = true;
		ForceRegenerateIBL();
		if (VerifyIBLPipelineComplete()) {
			m_pipelineReady = true;
		}
	}

	return m_pipelineReady;
}
