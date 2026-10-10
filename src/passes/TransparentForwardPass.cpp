#include "TransparentForwardPass.h"
#include "LightClusterPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include "../RenderContext.h"
#include "../Scene.h"
#include "../MeshComponent.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>

TransparentForwardPass::TransparentForwardPass() {}

TransparentForwardPass::~TransparentForwardPass()
{
	if (m_shader) {
		if (rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_shader);
		m_shader = {};
	}
}

bool TransparentForwardPass::Initialize(RenderContext& context)
{
	m_shader = CreateShaderProgram("shaders/forward_transparent_vert.glsl",
		"shaders/forward_transparent_frag.glsl");
	if (!m_shader) {
		std::cerr << "[TransparentForwardPass] Failed to create shader.\n";
		return false;
	}

	rhi::Device& gpu = rhi::Gpu();
	m_uniforms.view = gpu.GetUniformLocation(m_shader, "view");
	m_uniforms.projection = gpu.GetUniformLocation(m_shader, "projection");
	m_uniforms.model = gpu.GetUniformLocation(m_shader, "model");
	m_uniforms.normalMatrix = gpu.GetUniformLocation(m_shader, "normalMatrix");
	m_uniforms.viewPos = gpu.GetUniformLocation(m_shader, "viewPos");
	m_uniforms.screenSize = gpu.GetUniformLocation(m_shader, "screenSize");
	m_uniforms.gDepth = gpu.GetUniformLocation(m_shader, "gDepth");
	m_uniforms.irradianceMap = gpu.GetUniformLocation(m_shader, "irradianceMap");
	m_uniforms.prefilteredMap = gpu.GetUniformLocation(m_shader, "prefilteredMap");
	m_uniforms.brdfLUT = gpu.GetUniformLocation(m_shader, "brdfLUT");
	m_uniforms.prefilteredMaxLOD = gpu.GetUniformLocation(m_shader, "prefilteredMaxLOD");
	m_uniforms.iblIntensity = gpu.GetUniformLocation(m_shader, "iblIntensity");
	m_uniforms.diffuseIBLScale = gpu.GetUniformLocation(m_shader, "diffuseIBLScale");
	m_uniforms.specularIBLScale = gpu.GetUniformLocation(m_shader, "specularIBLScale");
	m_uniforms.numLights = gpu.GetUniformLocation(m_shader, "numLights");
	m_uniforms.multiLightShadowArray = gpu.GetUniformLocation(m_shader, "multiLightShadowArray");
	m_uniforms.enableShadows = gpu.GetUniformLocation(m_shader, "enableShadows");
	m_uniforms.shadowBias = gpu.GetUniformLocation(m_shader, "shadowBias");
	m_uniforms.cascadeSplits = gpu.GetUniformLocation(m_shader, "cascadeSplits");
	m_uniforms.cascadeBlendDistance = gpu.GetUniformLocation(m_shader, "cascadeBlendDistance");
	m_uniforms.cascadeBlendFactor = gpu.GetUniformLocation(m_shader, "cascadeBlendFactor");
	m_uniforms.shadowDarkness = gpu.GetUniformLocation(m_shader, "shadowDarkness");
	m_uniforms.shadowMinBrightness = gpu.GetUniformLocation(m_shader, "shadowMinBrightness");
	m_uniforms.shadowTransitionHardness = gpu.GetUniformLocation(m_shader, "shadowTransitionHardness");

	if constexpr (VerboseLogging) {
		if (m_runtimeVerboseLogging) {
			std::cout << "[TransparentForwardPass] Initialized successfully.\n";
		}
	}
	return true;
}

void TransparentForwardPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	// No internal buffers - renders directly to HDR FBO
}

void TransparentForwardPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox)
{
	if (!sceneGraph || !camera) {
		std::cerr << "[TransparentForwardPass] Missing sceneGraph or camera!" << std::endl;
		return;
	}

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Starting execution..." << std::endl; }

	auto* componentManager = sceneGraph->GetComponentManager();
	auto* transformSystem = sceneGraph->GetTransformSystem();
	auto* renderSystem = sceneGraph->GetRenderSystem();
	if (!componentManager || !transformSystem || !renderSystem) {
		std::cerr << "[TransparentForwardPass] Missing ECS systems required for transparent pass" << std::endl;
		return;
	}

	bool hasTransparentMeshes = false;
	auto& renderablePool = componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		auto& renderable = entry.component;
		if (renderable.hasAlpha) {
			hasTransparentMeshes = true;
			break;
		}
	}

	if (!hasTransparentMeshes) {
		return;
	}

	if (!ctx.hdrFBO) {
		std::cerr << "[TransparentForwardPass] ERROR: HDR FBO is null!" << std::endl;
		return;
	}

	rhi::Device& gpu = rhi::Gpu();

	// Always bind the HDR target explicitly. The pass must not depend on
	// skybox availability or previous pass side effects to pick the right FBO.
	ctx.hdrFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Rendering to HDR FBO (ID: " << ctx.hdrFBO->GetFBO() << ")" << std::endl; }

	// The HDR depth buffer is populated during lighting via a depth blit from the G-buffer.
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);        // Test against existing depth
	gpu.SetDepthWrite(false);       // Don't write to depth buffer (transparency layering)
	gpu.SetBlendEnabled(true);
	// Dual-source blending: the shader outputs premultiplied color and a per-channel
	// transmittance, so glass keeps full-strength reflections and tints what is behind it.
	gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::Src1Color);
	gpu.SetCullEnabled(true);      // Enable culling for proper transparent rendering
	gpu.SetCullFace(rhi::CullFace::Back);         // Cull back faces

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] State: Depth test=ENABLED(LESS), Depth writes=DISABLED, "
		<< "Blending=ENABLED(ONE, SRC1_COLOR)" << std::endl; }

	gpu.UseProgram(m_shader);

	// Weather state (rain beads on glass, shaders/includes/weather_common.glsl)
	{
		auto setWeather = [&](const char* name, float v) { const rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, name); if (loc >= 0) gpu.SetUniform(loc, v); };
		setWeather("uWeatherWetness", ctx.weatherWetness);
		setWeather("uWeatherPuddles", ctx.weatherPuddles);
		setWeather("uWeatherRain", ctx.weatherRain);
		setWeather("uWeatherPuddleScale", ctx.weatherPuddleScale);
		setWeather("uWeatherTime", ctx.timeSeconds);
	}

	// Upload camera and matrices
	if (m_uniforms.view >= 0) gpu.SetUniform(m_uniforms.view, ctx.view);
	if (m_uniforms.projection >= 0) gpu.SetUniform(m_uniforms.projection, ctx.proj);

	glm::vec3 cameraPos = camera->GetCameraPosition();
	if (m_uniforms.viewPos >= 0) gpu.SetUniform(m_uniforms.viewPos, cameraPos);

	// Upload screen size for depth comparison in fragment shader
	if (m_uniforms.screenSize >= 0) gpu.SetUniform(m_uniforms.screenSize,
		glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Camera position: ("
		<< cameraPos.x << ", " << cameraPos.y << ", " << cameraPos.z << ")" << std::endl; }

	// Bind depth buffer from G-buffer for depth comparisons
	gpu.BindTexture(TextureUnits::GBUFFER_DEPTH, ctx.gbufferFBO->GetDepthTexture());
	if (m_uniforms.gDepth >= 0) gpu.SetUniform(m_uniforms.gDepth, TextureUnits::GBUFFER_DEPTH);

	// Bind IBL textures for physically correct reflections and lighting
	if (skybox && skybox->ValidateIBLTextures()) {
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Binding IBL textures..." << std::endl; }

		gpu.BindTexture(TextureUnits::IRRADIANCE_MAP, skybox->GetIrradianceMap());
		gpu.BindTexture(TextureUnits::PREFILTERED_ENV_MAP, skybox->GetPrefilteredMap());
		gpu.BindTexture(TextureUnits::BRDF_LUT, skybox->GetBRDFLUT());

		if (m_uniforms.irradianceMap >= 0) gpu.SetUniform(m_uniforms.irradianceMap, TextureUnits::IRRADIANCE_MAP);
		if (m_uniforms.prefilteredMap >= 0) gpu.SetUniform(m_uniforms.prefilteredMap, TextureUnits::PREFILTERED_ENV_MAP);
		if (m_uniforms.brdfLUT >= 0) gpu.SetUniform(m_uniforms.brdfLUT, TextureUnits::BRDF_LUT);
		if (m_uniforms.prefilteredMaxLOD >= 0) gpu.SetUniform(m_uniforms.prefilteredMaxLOD, skybox->GetPrefilteredMaxLOD());
		if (m_uniforms.iblIntensity >= 0) gpu.SetUniform(m_uniforms.iblIntensity, ctx.iblIntensity);
		if (m_uniforms.diffuseIBLScale >= 0) gpu.SetUniform(m_uniforms.diffuseIBLScale, ctx.diffuseIBLScale);
		if (m_uniforms.specularIBLScale >= 0) gpu.SetUniform(m_uniforms.specularIBLScale, ctx.specularIBLScale);
	}
	else {
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] WARNING: No valid IBL textures available" << std::endl; }
	}

	// Transparent surfaces use the same light loop and shadowing as the deferred pass
	// (shaders/includes/lighting_common.glsl), so they need the same light/shadow inputs.
	// The shadow sampler must always point at its own unit: left at 0 it would alias the
	// material's 2D samplers, which is an invalid draw.
	if (m_uniforms.multiLightShadowArray >= 0) gpu.SetUniform(m_uniforms.multiLightShadowArray,
		TextureUnits::SHADOW_MAP_ARRAY);

	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);

	bool shadowsAvailable = false;
	if (ctx.lightManager && ctx.lightManager->GetActiveLightCount() > 0) {
		gpu.BindStorageBuffer(0, ctx.lightManager->GetLightDataSSBO());
		gpu.BindStorageBuffer(1, ctx.lightManager->GetShadowMatricesSSBO());
		if (m_uniforms.numLights >= 0) gpu.SetUniform(m_uniforms.numLights,
			ctx.lightManager->GetActiveLightCount());

		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Bound " << ctx.lightManager->GetActiveLightCount()
			<< " active lights" << std::endl; }

		rhi::TextureHandle shadowArray = ctx.lightManager->GetShadowArrayTexture();
		if (shadowArray.IsValid() && gpu.IsTexture(shadowArray)) {
			gpu.BindTexture(TextureUnits::SHADOW_MAP_ARRAY, shadowArray);
			shadowsAvailable = true;
		}

		const auto& shadowConfig = ctx.lightManager->shadowConfig;
		const float nearPlane = std::max(ctx.shadowNear, camera->GetCameraNearPlane());
		const float farPlane = std::max(nearPlane + 1.0f, std::min(ctx.shadowFar, camera->GetCameraFarPlane()));
		const glm::vec4 cascadeSplits = ctx.lightManager->ComputeCascadeSplitVector(nearPlane, farPlane);
		if (m_uniforms.cascadeSplits >= 0) gpu.SetUniform(m_uniforms.cascadeSplits, cascadeSplits);
		if (m_uniforms.cascadeBlendDistance >= 0) gpu.SetUniform(m_uniforms.cascadeBlendDistance, shadowConfig.cascadeBlendDistance);
		if (m_uniforms.cascadeBlendFactor >= 0) gpu.SetUniform(m_uniforms.cascadeBlendFactor, shadowConfig.cascadeBlendFactor);
		if (m_uniforms.shadowBias >= 0) gpu.SetUniform(m_uniforms.shadowBias, ctx.shadowBias);
		if (m_uniforms.shadowDarkness >= 0) gpu.SetUniform(m_uniforms.shadowDarkness, ctx.shadowDarkness);
		if (m_uniforms.shadowMinBrightness >= 0) gpu.SetUniform(m_uniforms.shadowMinBrightness, ctx.shadowMinBrightness);
		if (m_uniforms.shadowTransitionHardness >= 0) gpu.SetUniform(m_uniforms.shadowTransitionHardness, ctx.shadowTransitionHardness);
	}
	else {
		if (m_uniforms.numLights >= 0) gpu.SetUniform(m_uniforms.numLights, 0);
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] No active lights available" << std::endl; }
	}
	if (m_uniforms.enableShadows >= 0) gpu.SetUniform(m_uniforms.enableShadows, (ctx.enableShadows && shadowsAvailable) ? 1 : 0);

	// NOTE: Material-specific transmission and IOR uniforms are now set per-mesh
	// in Scene::Draw() rather than as pass-wide defaults here.
	// This allows each transparent material to use its own KHR_materials_transmission
	// and KHR_materials_ior extension values.

	renderSystem->RenderTransparent(ctx.view, ctx.proj, m_shader);

	//Restore render state for subsequent passes
	gpu.SetDepthWrite(true);      // Re-enable depth writes
	gpu.SetBlendEnabled(false);       // Disable blending
	gpu.SetDepthFunc(rhi::CompareOp::Less);      // Reset depth function to default

	// DO NOT unbind the HDR FBO - let the pipeline coordinator handle that

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Execution complete (HDR FBO remains bound)" << std::endl; }
}
