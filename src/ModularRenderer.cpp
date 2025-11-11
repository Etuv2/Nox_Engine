#include "ModularRenderer.h"
#include "FrameBuffer.h"
#include "ScreenQuad.h"
#include "SceneGraph.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Skybox.h"
#include "LightManager.h"

#include "passes/GBufferPass.h"
#include "passes/ShadowPass.h"
#include "passes/LPVPass.h"
#include "passes/SSAOPass.h"
#include "passes/ScreenSpaceShadowPass.h"
#include "passes/SSGIPass.h"
#include "passes/LightingPass.h"
#include "passes/BloomPass.h"
#include "passes/TAAPass.h"
#include "passes/TransparentForwardPass.h"
#include "passes/PostProcessPass.h"
#include "passes/GUIPass.h"  // NEW: Internal GUI rendering
#include <iostream>

ModularRenderer::ModularRenderer()
{
}

ModularRenderer::~ModularRenderer()
{

}


bool ModularRenderer::Initialize(int windowWidth, int windowHeight)
{
	m_context.width = windowWidth;
	m_context.height = windowHeight;

	// Initialize shared resources (FBOs, screen quad)
	if (!InitializeSharedResources()) {
		std::cerr << "[ModularRenderer] Failed to initialize shared resources.\n";
		return false;
	}

	// Create and initialize all passes
	m_shadowPass = std::make_unique<ShadowPass>();
	m_lpvPass = std::make_unique<LPVPass>(); //Create LPV pass
	m_gbufferPass = std::make_unique<GBufferPass>();
	m_ssaoPass = std::make_unique<SSAOPass>();
	m_screenSpaceShadowPass = std::make_unique<ScreenSpaceShadowPass>();
	m_ssgiPass = std::make_unique<SSGIPass>(); //Create SSGI pass
	m_lightingPass = std::make_unique<LightingPass>();
	m_bloomPass = std::make_unique<BloomPass>();
	m_taaPass = std::make_unique<TAAPass>();
	m_transparentPass = std::make_unique<TransparentForwardPass>();
	m_postProcessPass = std::make_unique<PostProcessPass>();
	m_guiPass = std::make_unique<GUIPass>();

	bool success = true;
	success &= m_shadowPass->Initialize(m_context);
	success &= m_lpvPass->Initialize(m_context);
	success &= m_gbufferPass->Initialize(m_context);
	success &= m_ssaoPass->Initialize(m_context);
	success &= m_screenSpaceShadowPass->Initialize(m_context);
	success &= m_ssgiPass->Initialize(m_context);
	success &= m_lightingPass->Initialize(m_context);
	success &= m_bloomPass->Initialize(m_context);
	success &= m_taaPass->Initialize(m_context);
	success &= m_transparentPass->Initialize(m_context);
	success &= m_postProcessPass->Initialize(m_context);
	success &= m_guiPass->Initialize(m_context);

	if (!success) {
		std::cerr << "[ModularRenderer] Failed to initialize one or more passes.\n";
		return false;
	}

	std::cout << "[ModularRenderer] Initialized successfully with " << windowWidth
		<< "x" << windowHeight << " resolution.\n";
	return true;
}

bool ModularRenderer::InitializeSharedResources()
{
	// Create G-buffer FBO with optimized 3-RT layout for better bandwidth efficiency
	// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Emissive (RGB) + Specular F0 luminance (A)
	m_context.gbufferFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<GLenum>{
			GL_RGBA8,    // RT0: Oct normal + roughness/metallic
			GL_RGBA16F,  // RT1: Albedo + occlusion
			GL_RGBA16F   // RT2: Emissive + specular
		},
		true,  // useDepthAsTexture
		false  // useDepthAsTextureArray
	);

	if (!m_context.gbufferFBO->IsComplete()) {
		std::cerr << "[ModularRenderer] G-buffer FBO not complete!\n";
		return false;
	}

	// Create HDR FBO
	m_context.hdrFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<GLenum>{ GL_RGB16F },
		false, false, 1, false, GL_DEPTH24_STENCIL8
	);

	if (!m_context.hdrFBO->IsComplete()) {
		std::cerr << "[ModularRenderer] HDR FBO not complete!\n";
		return false;
	}

	// Create screen quad
	m_context.screenQuad = std::make_unique<ScreenQuad>();

	std::cout << "[ModularRenderer] Shared resources initialized.\n";
	return true;
}

void ModularRenderer::Resize(int newWidth, int newHeight)
{
	if (newWidth == m_context.width && newHeight == m_context.height) {
		return;
	}

	m_context.width = newWidth;
	m_context.height = newHeight;

	// Resize shared FBOs
	if (m_context.gbufferFBO) {
		m_context.gbufferFBO->Resize(newWidth, newHeight);
	}
	if (m_context.hdrFBO) {
		m_context.hdrFBO->Resize(newWidth, newHeight);
	}

	// Notify all passes about resize
	if (m_shadowPass) m_shadowPass->Resize(m_context, newWidth, newHeight);
	if (m_lpvPass) m_lpvPass->Resize(m_context, newWidth, newHeight);
	if (m_gbufferPass) m_gbufferPass->Resize(m_context, newWidth, newHeight);
	if (m_ssaoPass) m_ssaoPass->Resize(m_context, newWidth, newHeight);
	if (m_screenSpaceShadowPass) m_screenSpaceShadowPass->Resize(m_context, newWidth, newHeight);
	if (m_ssgiPass) m_ssgiPass->Resize(m_context, newWidth, newHeight);
	if (m_lightingPass) m_lightingPass->Resize(m_context, newWidth, newHeight);
	if (m_bloomPass) m_bloomPass->Resize(m_context, newWidth, newHeight);
	if (m_taaPass) m_taaPass->Resize(m_context, newWidth, newHeight);
	if (m_transparentPass) m_transparentPass->Resize(m_context, newWidth, newHeight);
	if (m_postProcessPass) m_postProcessPass->Resize(m_context, newWidth, newHeight);
	if (m_guiPass) m_guiPass->Resize(m_context, newWidth, newHeight);

	std::cout << "[ModularRenderer] Resized to " << newWidth << "x" << newHeight << "\n";
}

void ModularRenderer::UpdateContext(const std::shared_ptr<Camera>& camera,
	float exposure, float gamma,
	bool enableShadows, float shadowBias,
	glm::vec3 envColor)
{
	// Update per-frame parameters (from MainWindow function call)
	m_context.exposure = exposure;
	m_context.gamma = gamma;
	m_context.enableShadows = enableShadows;
	m_context.shadowBias = shadowBias;
	m_context.envColor = envColor;


	// Compute view/projection matrices
	float aspect = static_cast<float>(m_context.width) / static_cast<float>(m_context.height);

	// Store previous matrices for TAA
	m_context.prevView = m_context.view;
	m_context.prevProj = m_context.proj;

	// Get current matrices
	auto [view, proj] = camera->GetUpdatedViewProjectionMatrix(
		aspect,
		camera->GetCameraNearPlane(),
		camera->GetCameraFarPlane()
	);

	m_context.view = view;
	m_context.proj = proj;

	// Apply TAA jitter if enabled
	if (m_context.enableTAA && m_taaPass) {
		glm::vec2 jitter = m_taaPass->GetCurrentJitter();
		// Scale jitter to projection space
		jitter *= (1.0f / glm::vec2(m_context.width, m_context.height));
		m_context.proj[2][0] += jitter.x;
		m_context.proj[2][1] += jitter.y;
	}
}


void ModularRenderer::CheckGLError(const std::string& passName)
{
	if (!ErrorPrintingEnabled) return;
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		std::cerr << "[ModularRenderer] ERROR after " << passName << ": 0x"
			<< std::hex << error << std::dec;

		// Decode common error codes
		switch (error) {
		case GL_INVALID_ENUM:
			std::cerr << " (GL_INVALID_ENUM)";
			break;
		case GL_INVALID_VALUE:
			std::cerr << " (GL_INVALID_VALUE)";
			break;
		case GL_INVALID_OPERATION:
			std::cerr << " (GL_INVALID_OPERATION)";
			break;
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			std::cerr << " (GL_INVALID_FRAMEBUFFER_OPERATION)";
			break;
		case GL_OUT_OF_MEMORY:
			std::cerr << " (GL_OUT_OF_MEMORY)";
			break;
		default:
			std::cerr << " (Unknown error)";
			break;
		}
		std::cerr << std::endl;
	}
}

void ModularRenderer::Render(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& lighting,
	float exposure,
	float gamma,
	bool enableShadows,
	float shadowBias,
	float shadow_near,
	float shadow_far,
	glm::vec3 envColor,
	int windowWidth,
	int windowHeight)
{
	if (!sceneGraph || !camera) {
		std::cerr << "[ModularRenderer] ERROR: Missing scene graph or camera!\n";
		return;
	}

	// Initialize light manager if needed
	if (!sceneGraph->GetLightManager()) {
		std::cout << "[ModularRenderer] Initializing LightManager..." << std::endl;
		auto lightManager = std::make_shared<LightManager>();
		lightManager->InitializeShadowSystem(12, 512);
		lightManager->CollectLightsFromScene(sceneGraph);
		sceneGraph->SetLightManager(lightManager);
	}

	// Update context with light manager
	m_context.lightManager = sceneGraph->GetLightManager();

	// Update lights
	if (m_context.lightManager) {
		m_context.lightManager->UpdateLights(0.016f);
	}

	// Update context with current frame parameters
	UpdateContext(camera, exposure, gamma, enableShadows, shadowBias, envColor);

	// Get skybox from scene
	auto skybox = sceneGraph->GetSkybox();

	// Sync IBL intensity settings from context to skybox
	if (skybox && skybox->IsReady()) {
		skybox->SetIBLIntensity(m_context.iblIntensity);
		skybox->SetSkyboxExposure(m_context.skyboxExposure);
		skybox->SetDiffuseIBLScale(m_context.diffuseIBLScale);
		skybox->SetSpecularIBLScale(m_context.specularIBLScale);
	}

	// Execute rendering pipeline in correct order
	m_shadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	CheckGLError("ShadowPass");

	m_gbufferPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	CheckGLError("GBufferPass");

	//LPV Global Illumination Pass (AFTER G-buffer, so geometry is available for RSM)
	//This generates dynamic indirect lighting from the first light bounce
	if (m_context.enableLPV) {

		// Update LPV config from context (these will be overridden if an LPV node exists)
		if (m_lpvPass) {
			m_lpvPass->config.enableLPV = m_context.enableLPV;
			m_lpvPass->config.gridResolution = m_context.lpvGridResolution;
			m_lpvPass->config.voxelSize = m_context.lpvVoxelSize;
			m_lpvPass->config.rsmResolution = m_context.lpvRSMResolution;
			m_lpvPass->config.vplSampleCount = m_context.lpvVPLSampleCount;
			m_lpvPass->config.propagationIterations = m_context.lpvPropagationIterations;
			m_lpvPass->config.propagationAttenuation = m_context.lpvPropagationAttenuation;
			m_lpvPass->config.propagationBias = m_context.lpvPropagationBias;
			m_lpvPass->config.enableOcclusion = m_context.lpvEnableOcclusion;
			m_lpvPass->config.giStrength = m_context.lpvGIStrength;
			m_lpvPass->config.updateFrequency = m_context.lpvUpdateFrequency;

			//gridCenter is intentionally NOT set here - it will be set by the LPV pass
			// based on either the LPV volume node position or camera position

			m_lpvPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			CheckGLError("LPVPass");
		}
	}
	else {
		std::cout << "[ModularRenderer] LPV GI Pass SKIPPED (disabled)" << std::endl;
	}

	// Only execute SSAO if enabled
	GLuint ssaoTex = 0;
	if (m_context.enableSSAO) {
		m_ssaoPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		CheckGLError("SSAOPass");
		ssaoTex = m_ssaoPass->GetSSAOTexture();
	}
	else {
		std::cout << "[ModularRenderer] SSAO Pass SKIPPED (disabled)" << std::endl;
	}

	// Screen-space shadows (contact shadows) if enabled
	GLuint sssTex = 0;
	if (m_context.enableScreenSpaceShadows) {

		m_screenSpaceShadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		CheckGLError("ScreenSpaceShadowPass");
		sssTex = m_screenSpaceShadowPass->GetShadowTexture();
	}
	else {
		std::cout << "[ModularRenderer] Screen-Space Shadow Pass SKIPPED (disabled)" << std::endl;
	}

	// TAA Pass (velocity + resolve) - executes before lighting
	if (m_context.enableTAA) {
		m_taaPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		CheckGLError("TAAPass");
	}
	else {
		std::cout << "[ModularRenderer] TAA Pass SKIPPED (disabled)" << std::endl;
	}

	//SSGI Pass - Screen Space Global Illumination (before lighting)
	if (m_context.enableSSGI && m_ssgiPass) {
		m_ssgiPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		CheckGLError("SSGIPass");
	}
	else {
		std::cout << "[ModularRenderer] SSGI Pass SKIPPED (disabled)" << std::endl;
	}

	//Provide SSAO texture to lighting pass (or 0 if disabled)
	m_lightingPass->SetSSAOTexture(ssaoTex);
	//Provide screen-space shadow texture to lighting pass (or 0 if disabled)
	m_lightingPass->SetScreenSpaceShadowTexture(sssTex);

	//Provide SSGI texture to lighting pass (or 0 if disabled)
	if (m_context.enableSSGI && m_ssgiPass) {
		m_lightingPass->SetSSGITexture(m_ssgiPass->GetSSGITexture());
	}
	else {
		m_lightingPass->SetSSGITexture(0);
	}

	//Provide LPV textures to lighting pass (or 0 if disabled)
	if (m_context.enableLPV && m_lpvPass) {
		GLuint lpvR = m_lpvPass->GetLPVTextureR();
		GLuint lpvG = m_lpvPass->GetLPVTextureG();
		GLuint lpvB = m_lpvPass->GetLPVTextureB();

		m_lightingPass->SetLPVTextures(lpvR, lpvG, lpvB);
	}
	else {
		m_lightingPass->SetLPVTextures(0, 0, 0);
	}

	m_lightingPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	CheckGLError("LightingPass");

	// Skybox rendering (background into HDR)
	if (skybox && skybox->IsReady()) {
		m_context.hdrFBO->Bind();
		//Skybox uses GL_LEQUAL depth test and writes depth at far plane (z=w)
		skybox->Draw(m_context.view, m_context.proj);
		CheckGLError("Skybox");
	}

	//// Transparent forward rendering(broken out for debugging)
	//m_transparentPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	//CheckGLError("TransparentForwardPass");

	//Unbinding HDR FBO after both skybox and transparent rendering
	FrameBuffer::Unbind();


	// Only execute Bloom if enabled
	GLuint bloomTex = 0;
	if (m_context.enableBloom)
	{
		m_bloomPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		CheckGLError("BloomPass");
		bloomTex = m_bloomPass->GetBloomResult();
	}
	else {
		std::cout << "[ModularRenderer] Bloom Pass SKIPPED (disabled)" << std::endl;
	}

	//Provide bloom texture to post-process (or 0 if disabled)
	m_postProcessPass->SetBloomTexture(bloomTex);
	m_postProcessPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	CheckGLError("PostProcessPass");


	//Render internal GUI elements to backbuffer (after post-processing, before ImGui editor UI)
	//This executes AFTER PostProcessPass which already unbinds to default framebuffer,the backbuffer
	m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
	CheckGLError("GUIPass");


	//Capture color history for SSGI
	if (m_ssgiPass) {
		m_ssgiPass->CaptureHistory(m_context);
	}
}


void ModularRenderer::ResetTAA()
{
	if (m_taaPass) {
		m_taaPass->ResetHistory();
	}
}

int ModularRenderer::GetTAAFrameIndex() const
{
	return m_taaPass ? m_taaPass->GetFrameIndex() : 0;
}
