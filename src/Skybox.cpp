#include "Skybox.h"
#include "ShaderLoader.h"
#include "stb_image.h"
#include "TextureUnits.h"
#include "FrameBuffer.h"
#include "GLState.h"
#include <iostream>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

void Skybox::CreateCaptureFBO(int width, int height)
{
	if (m_captureFBO == 0)
		glGenFramebuffers(1, &m_captureFBO);
	if (m_captureRBO == 0)
		glGenRenderbuffers(1, &m_captureRBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glBindRenderbuffer(GL_RENDERBUFFER, m_captureRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_captureRBO);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		std::cerr << "[Skybox] Capture FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Skybox::ResizeCaptureRBO(int width, int height)
{
	glBindRenderbuffer(GL_RENDERBUFFER, m_captureRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
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
	GLenum format = (nComponents >= 4) ? GL_RGBA : GL_RGB;

	auto hdrTexture = Texture::Builder::Texture2D(width, height, GL_RGB16F)
		.Format(format)
		.DataType(GL_FLOAT)
		.Data(data)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.TextureType(TextureType::HDR)
		.Build();

	stbi_image_free(data);

	std::cout << "[Skybox] Loaded HDR texture: " << width << "x" << height
		<< " (" << nComponents << " channels)" << std::endl;

	return hdrTexture;
}

void Skybox::buildSkyboxCube()
{
	if (m_skyboxVAO != 0)
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

	glGenVertexArrays(1, &m_skyboxVAO);
	glGenBuffers(1, &m_skyboxVBO);
	glBindVertexArray(m_skyboxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glBindVertexArray(0);
}

void Skybox::renderCube()
{
	glBindVertexArray(m_skyboxVAO);
	glDrawArrays(GL_TRIANGLES, 0, 36);
	glBindVertexArray(0);
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
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

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

	// CRITICAL FIX: Use higher resolution for environment map (512 -> 1024) for better quality
	const int envMapSize = 1024;
	
	// Create environment cubemap using new Texture builder
	m_envCubemap = Texture::Builder::TextureCube(envMapSize, GL_RGB16F)
		.FilterMode(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR)
		.TextureType(TextureType::Cubemap)
		.GenerateMipmaps(false)  // We'll generate after conversion
		.Build();

	buildSkyboxCube();
	CreateCaptureFBO(envMapSize, envMapSize);

	// Save state
	GLint prevViewport[4];
	GLint prevFBO;
	GLint prevProg;
	GLint prevActiveTexture;
	GLint prevCube;
	glGetIntegerv(GL_VIEWPORT, prevViewport);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &prevCube);

	// Convert HDR equirect to cubemap
	glUseProgram(m_equiRectToCubeShader);
	glUniform1i(glGetUniformLocation(m_equiRectToCubeShader, "equirectangularMap"), 0);

	// Bind HDR texture
	hdrTexture->Bind(GL_TEXTURE0);

	glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	glUniformMatrix4fv(glGetUniformLocation(m_equiRectToCubeShader, "projection"), 1, GL_FALSE, glm::value_ptr(captureProjection));

	glm::mat4 captureViews[] = {
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
	 glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
		   glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
	};

	glViewport(0, 0, envMapSize, envMapSize);
	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);

	for (int i = 0; i < 6; ++i) {
		glUniformMatrix4fv(glGetUniformLocation(m_equiRectToCubeShader, "view"), 1, GL_FALSE, glm::value_ptr(captureViews[i]));
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_envCubemap->ID(), 0);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			std::cerr << "[Skybox] ERROR: Env conversion FBO incomplete (face " << i << ")" << std::endl;
			return false;
		}

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		renderCube();
	}

	// Generate mipmaps for environment map
	m_envCubemap->GenerateMipmaps();

	// CRITICAL: Immediately generate IBL resources (strict requirement)
	std::cout << "[Skybox] Generating IBL resources with CORRECTED shaders..." << std::endl;
	if (!GenerateIBLResources() || !VerifyIBLPipelineComplete()) {
		std::cerr << "[Skybox] FATAL: Initial IBL generation failed – attempting forced regeneration..." << std::endl;
		ForceRegenerateIBL();
		if (!VerifyIBLPipelineComplete()) {
			std::cerr << "[Skybox] FATAL: Skybox pipeline invalid after forced regeneration." << std::endl;
			return false;
		}
	}
	m_pipelineReady = true;

	// Restore state
	glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	glUseProgram(prevProg);
	glActiveTexture(prevActiveTexture);
	glBindTexture(GL_TEXTURE_CUBE_MAP, prevCube);

	// Pre-bind environment map sampler location
	glUseProgram(m_skyboxShader);
	SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);
	glUseProgram(prevProg);

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

	// Use comprehensive state management for skybox rendering
	SCOPED_GL_RENDER_STATE();

	// Remove translation from view matrix
	glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));

	// Set up optimal state for skybox rendering
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);

	// Render the skybox
	glUseProgram(m_skyboxShader);
	glUniformMatrix4fv(glGetUniformLocation(m_skyboxShader, "view"), 1, GL_FALSE, glm::value_ptr(viewNoTrans));
	glUniformMatrix4fv(glGetUniformLocation(m_skyboxShader, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
	glUniform1f(glGetUniformLocation(m_skyboxShader, "skyboxExposure"), m_skyboxExposure);

	// Bind environment cubemap using new Texture class
	m_envCubemap->Bind(GL_TEXTURE0 + TextureUnits::SKYBOX_CUBEMAP);
	SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);

	renderCube();
}

void Skybox::Cleanup()
{
	if (m_skyboxVBO) glDeleteBuffers(1, &m_skyboxVBO);
	if (m_skyboxVAO) glDeleteVertexArrays(1, &m_skyboxVAO);

	// New Texture class handles cleanup automatically via smart pointers
	m_envCubemap.reset();
	m_irradianceMap.reset();
	m_prefilteredMap.reset();
	m_brdfLUT.reset();

	if (m_equiRectToCubeShader) glDeleteProgram(m_equiRectToCubeShader);
	if (m_skyboxShader) glDeleteProgram(m_skyboxShader);
	if (m_irradianceShader) glDeleteProgram(m_irradianceShader);
	if (m_prefilterShader) glDeleteProgram(m_prefilterShader);
	if (m_brdfShader) glDeleteProgram(m_brdfShader);
	if (m_captureFBO) glDeleteFramebuffers(1, &m_captureFBO);
	if (m_captureRBO) glDeleteRenderbuffers(1, &m_captureRBO);

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
	std::cout << "  Environment Map: " << (m_envCubemap ? m_envCubemap->ID() : 0) << std::endl;
	std::cout << "  Irradiance Map: " << (m_irradianceMap ? m_irradianceMap->ID() : 0) << std::endl;
	std::cout << "  Prefiltered Map: " << (m_prefilteredMap ? m_prefilteredMap->ID() : 0) << std::endl;
	std::cout << "  BRDF LUT: " << (m_brdfLUT ? m_brdfLUT->ID() : 0) << std::endl;
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
	std::cout << "[Skybox] - Irradiance Map: " << (m_irradianceMap ? m_irradianceMap->ID() : 0) << std::endl;
	std::cout << "[Skybox] - Prefiltered Map: " << (m_prefilteredMap ? m_prefilteredMap->ID() : 0) << std::endl;
	std::cout << "[Skybox] - BRDF LUT: " << (m_brdfLUT ? m_brdfLUT->ID() : 0) << std::endl;
	return true;
}

bool Skybox::GenerateIrradianceMap()
{
	std::cout << "[Skybox] Generating irradiance map..." << std::endl;

	// Use comprehensive state management
	SCOPED_GL_STATE();

	// Create irradiance cubemap using new Texture builder
	m_irradianceMap = Texture::Builder::TextureCube(32, GL_RGB16F)
		.FilterMode(GL_LINEAR, GL_LINEAR)
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

	glUseProgram(m_irradianceShader);
	glUniform1i(glGetUniformLocation(m_irradianceShader, "environmentMap"), 0);
	glUniformMatrix4fv(glGetUniformLocation(m_irradianceShader, "projection"), 1, GL_FALSE, glm::value_ptr(proj));

	// Bind source environment map to unit 0
	m_envCubemap->Bind(GL_TEXTURE0);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glViewport(0, 0, 32, 32);
	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);

	std::cout << "[Skybox] Starting irradiance convolution..." << std::endl;
	for (int face = 0; face < 6; ++face) {
		glUniformMatrix4fv(glGetUniformLocation(m_irradianceShader, "view"), 1, GL_FALSE, glm::value_ptr(views[face]));
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_irradianceMap->ID(), 0);

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			std::cerr << "[Skybox] Irradiance FBO incomplete (face " << face << ") status=0x"
				<< std::hex << status << std::dec << std::endl;
			return false;
		}

		glClear(GL_COLOR_BUFFER_BIT);
		renderCube();
	}

	// Enhanced debug readback with validation
	float pixel[3] = { 0,0,0 };
	GLuint debugFBO;
	glGenFramebuffers(1, &debugFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, debugFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_irradianceMap->ID(), 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		glReadPixels(16, 16, 1, 1, GL_RGB, GL_FLOAT, pixel);
		std::cout << "[Skybox] Irradiance debug sample = (" << pixel[0] << ", " << pixel[1] << ", " << pixel[2] << ")" << std::endl;

		if (pixel[0] < 0.0f || pixel[1] < 0.0f || pixel[2] < 0.0f) {
			std::cerr << "[Skybox] WARNING: Irradiance contains negative values! This indicates a shader error." << std::endl;
		}

		if (std::isnan(pixel[0]) || std::isnan(pixel[1]) || std::isnan(pixel[2])) {
			std::cerr << "[Skybox] ERROR: Irradiance contains NaN values! IBL will be corrupted." << std::endl;
			glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
			glDeleteFramebuffers(1, &debugFBO);
			return false;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glDeleteFramebuffers(1, &debugFBO);

	std::cout << "[Skybox] Irradiance map generated (32x32)" << std::endl;
	return true;
}

bool Skybox::GeneratePrefilteredMap()
{
	std::cout << "[Skybox] Generating prefiltered map..." << std::endl;

	SCOPED_GL_STATE();

	const unsigned int baseSize = 128;
	const unsigned int maxMipLevels = 5;

	// Create prefiltered cubemap with mipmaps using builder
	m_prefilteredMap = Texture::Builder::TextureCube(baseSize, GL_RGB16F)
		.FilterMode(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR)
		.TextureType(TextureType::Cubemap)
		.GenerateMipmaps(false)  // We'll manually create mip levels
		.Build();

	// Manually create all mip levels
	for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
		unsigned int w = baseSize >> mip;
		unsigned int h = baseSize >> mip;

		glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilteredMap->ID());
		for (int face = 0; face < 6; ++face) {
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, mip, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
		}
	}

	// Set max mip level
	m_prefilteredMap->SetMipmapRange(0, maxMipLevels - 1);

	// Create lightweight FBO
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);

	glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	glm::mat4 views[] = {
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
	 glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
		glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
	};

	glUseProgram(m_prefilterShader);
	glUniform1i(glGetUniformLocation(m_prefilterShader, "environmentMap"), 0);
	glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
	glUniform1f(glGetUniformLocation(m_prefilterShader, "resolution"), 512.0f);

	// Bind environment map to unit 0
	m_envCubemap->Bind(GL_TEXTURE0);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
		unsigned int w = baseSize >> mip;
		unsigned int h = baseSize >> mip;
		glViewport(0, 0, w, h);

		float roughness = float(mip) / float(maxMipLevels - 1);
		roughness *= roughness;
		glUniform1f(glGetUniformLocation(m_prefilterShader, "roughness"), roughness);

		std::cout << "[Skybox] Prefilter mip " << mip << " size=" << w << "x" << h
			<< " roughness=" << roughness << std::endl;

		for (int face = 0; face < 6; ++face) {
			glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "view"), 1, GL_FALSE, glm::value_ptr(views[face]));
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_prefilteredMap->ID(), mip);

			GLenum drawBuf = GL_COLOR_ATTACHMENT0;
			glDrawBuffers(1, &drawBuf);

			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				std::cerr << "[Skybox] Prefilter FBO incomplete mip=" << mip << " face=" << face
					<< " status=0x" << std::hex << status << std::dec << std::endl;
				glDeleteFramebuffers(1, &fbo);
				return false;
			}

			glClear(GL_COLOR_BUFFER_BIT);
			renderCube();
		}
	}

	// Debug sample
	float pix[3] = { 0,0,0 };
	GLuint dbgFBO;
	glGenFramebuffers(1, &dbgFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, dbgFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_prefilteredMap->ID(), 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		glReadPixels(64, 64, 1, 1, GL_RGB, GL_FLOAT, pix);
		std::cout << "[Skybox] Prefilter debug sample = (" << pix[0] << ", " << pix[1] << ", " << pix[2] << ")" << std::endl;

		if (pix[0] < 0.0f || pix[1] < 0.0f || pix[2] < 0.0f) {
			std::cerr << "[Skybox] WARNING: Prefiltered map contains negative values!" << std::endl;
		}

		if (std::isnan(pix[0]) || std::isnan(pix[1]) || std::isnan(pix[2])) {
			std::cerr << "[Skybox] ERROR: Prefiltered map contains NaN values!" << std::endl;
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glDeleteFramebuffers(1, &dbgFBO);
			glDeleteFramebuffers(1, &fbo);
			return false;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDeleteFramebuffers(1, &dbgFBO);
	glDeleteFramebuffers(1, &fbo);

	m_prefilteredMaxLOD = maxMipLevels - 1;
	std::cout << "[Skybox] Prefiltered map generation completed (" << baseSize << " base, "
		<< maxMipLevels << " mips)" << std::endl;
	return true;
}

bool Skybox::GenerateBRDFLUT()
{
	std::cout << "[Skybox] Generating BRDF LUT..." << std::endl;

	SCOPED_GL_STATE();

	// Create BRDF integration lookup table using new Texture builder
	m_brdfLUT = Texture::Builder::Texture2D(512, 512, GL_RG16F)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.TextureType(TextureType::Custom)
		.Build();

	// Full screen quad for BRDF integration
	GLuint quadVAO = 0, quadVBO = 0;
	float quadVertices[] = {
		-1.f,  1.f, 0.f, 0.f, 1.f,
		-1.f, -1.f, 0.f, 0.f, 0.f,
	   1.f,  1.f, 0.f, 1.f, 1.f,
 1.f, -1.f, 0.f, 1.f, 0.f
	};

	glGenVertexArrays(1, &quadVAO);
	glGenBuffers(1, &quadVBO);
	glBindVertexArray(quadVAO);
	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

	ResizeCaptureRBO(512, 512);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glViewport(0, 0, 512, 512);
	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLUT->ID(), 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		std::cerr << "[Skybox] BRDF LUT FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
		glDeleteVertexArrays(1, &quadVAO);
		glDeleteBuffers(1, &quadVBO);
		return false;
	}

	glUseProgram(m_brdfShader);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDeleteVertexArrays(1, &quadVAO);
	glDeleteBuffers(1, &quadVBO);

	// Debug sample
	float debugPixel[2] = { 0, 0 };
	GLuint debugFBO;
	glGenFramebuffers(1, &debugFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, debugFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLUT->ID(), 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		glReadPixels(256, 256, 1, 1, GL_RG, GL_FLOAT, debugPixel);
		std::cout << "[Skybox] BRDF LUT debug sample = (" << debugPixel[0] << ", " << debugPixel[1] << ")" << std::endl;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glDeleteFramebuffers(1, &debugFBO);

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
	if (m_skyboxShader == 0) {
		return false;
	}

	if (m_irradianceShader == 0 || m_prefilterShader == 0 || m_brdfShader == 0) {
		return false;
	}

	// Check cube geometry
	if (m_skyboxVAO == 0 || m_skyboxVBO == 0) {
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

