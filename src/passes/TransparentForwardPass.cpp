#include "TransparentForwardPass.h"
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
		glDeleteProgram(m_shader);
		m_shader = 0;
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

	m_uniforms.view = glGetUniformLocation(m_shader, "view");
	m_uniforms.projection = glGetUniformLocation(m_shader, "projection");
	m_uniforms.model = glGetUniformLocation(m_shader, "model");
	m_uniforms.normalMatrix = glGetUniformLocation(m_shader, "normalMatrix");
	m_uniforms.viewPos = glGetUniformLocation(m_shader, "viewPos");
	m_uniforms.screenSize = glGetUniformLocation(m_shader, "screenSize");
	m_uniforms.gDepth = glGetUniformLocation(m_shader, "gDepth");
	m_uniforms.irradianceMap = glGetUniformLocation(m_shader, "irradianceMap");
	m_uniforms.prefilteredMap = glGetUniformLocation(m_shader, "prefilteredMap");
	m_uniforms.brdfLUT = glGetUniformLocation(m_shader, "brdfLUT");
	m_uniforms.prefilteredMaxLOD = glGetUniformLocation(m_shader, "prefilteredMaxLOD");
	m_uniforms.iblIntensity = glGetUniformLocation(m_shader, "iblIntensity");
	m_uniforms.diffuseIBLScale = glGetUniformLocation(m_shader, "diffuseIBLScale");
	m_uniforms.specularIBLScale = glGetUniformLocation(m_shader, "specularIBLScale");
	m_uniforms.numLights = glGetUniformLocation(m_shader, "numLights");
	m_uniforms.multiLightShadowArray = glGetUniformLocation(m_shader, "multiLightShadowArray");
	m_uniforms.enableShadows = glGetUniformLocation(m_shader, "enableShadows");
	m_uniforms.shadowBias = glGetUniformLocation(m_shader, "shadowBias");
	m_uniforms.cascadeSplits = glGetUniformLocation(m_shader, "cascadeSplits");
	m_uniforms.cascadeBlendDistance = glGetUniformLocation(m_shader, "cascadeBlendDistance");
	m_uniforms.cascadeBlendFactor = glGetUniformLocation(m_shader, "cascadeBlendFactor");
	m_uniforms.shadowDarkness = glGetUniformLocation(m_shader, "shadowDarkness");
	m_uniforms.shadowMinBrightness = glGetUniformLocation(m_shader, "shadowMinBrightness");
	m_uniforms.shadowTransitionHardness = glGetUniformLocation(m_shader, "shadowTransitionHardness");

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

	// Always bind the HDR target explicitly. The pass must not depend on
	// skybox availability or previous pass side effects to pick the right FBO.
	ctx.hdrFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Rendering to HDR FBO (ID: " << ctx.hdrFBO->GetFBO() << ")" << std::endl; }

	// The HDR depth buffer is populated during lighting via a depth blit from the G-buffer.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);        // Test against existing depth
	glDepthMask(GL_FALSE);       // Don't write to depth buffer (transparency layering)
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha blending
	glEnable(GL_CULL_FACE);      // Enable culling for proper transparent rendering
	glCullFace(GL_BACK);         // Cull back faces

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] State: Depth test=ENABLED(LESS), Depth writes=DISABLED, "
		<< "Blending=ENABLED(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)" << std::endl; }

	glUseProgram(m_shader);

	// Upload camera and matrices
	if (m_uniforms.view >= 0) glUniformMatrix4fv(m_uniforms.view,
		1, GL_FALSE, glm::value_ptr(ctx.view));
	if (m_uniforms.projection >= 0) glUniformMatrix4fv(m_uniforms.projection,
		1, GL_FALSE, glm::value_ptr(ctx.proj));

	glm::vec3 cameraPos = camera->GetCameraPosition();
	if (m_uniforms.viewPos >= 0) glUniform3fv(m_uniforms.viewPos,
		1, glm::value_ptr(cameraPos));

	// Upload screen size for depth comparison in fragment shader
	if (m_uniforms.screenSize >= 0) glUniform2f(m_uniforms.screenSize,
		static_cast<float>(ctx.width), static_cast<float>(ctx.height));

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Camera position: ("
		<< cameraPos.x << ", " << cameraPos.y << ", " << cameraPos.z << ")" << std::endl; }

	// Bind depth buffer from G-buffer for depth comparisons
	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_DEPTH);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	if (m_uniforms.gDepth >= 0) glUniform1i(m_uniforms.gDepth, TextureUnits::GBUFFER_DEPTH);

	// Bind IBL textures for physically correct reflections and lighting
	if (skybox && skybox->ValidateIBLTextures()) {
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Binding IBL textures..." << std::endl; }

		glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());

		glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());

		glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
		glBindTexture(GL_TEXTURE_2D, skybox->GetBRDFLUT());

		if (m_uniforms.irradianceMap >= 0) glUniform1i(m_uniforms.irradianceMap, TextureUnits::IRRADIANCE_MAP);
		if (m_uniforms.prefilteredMap >= 0) glUniform1i(m_uniforms.prefilteredMap, TextureUnits::PREFILTERED_ENV_MAP);
		if (m_uniforms.brdfLUT >= 0) glUniform1i(m_uniforms.brdfLUT, TextureUnits::BRDF_LUT);
		if (m_uniforms.prefilteredMaxLOD >= 0) glUniform1f(m_uniforms.prefilteredMaxLOD, skybox->GetPrefilteredMaxLOD());
		if (m_uniforms.iblIntensity >= 0) glUniform1f(m_uniforms.iblIntensity, ctx.iblIntensity);
		if (m_uniforms.diffuseIBLScale >= 0) glUniform1f(m_uniforms.diffuseIBLScale, ctx.diffuseIBLScale);
		if (m_uniforms.specularIBLScale >= 0) glUniform1f(m_uniforms.specularIBLScale, ctx.specularIBLScale);
	}
	else {
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] WARNING: No valid IBL textures available" << std::endl; }
	}

	// Transparent surfaces use the same light loop and shadowing as the deferred pass
	// (shaders/includes/lighting_common.glsl), so they need the same light/shadow inputs.
	// The shadow sampler must always point at its own unit: left at 0 it would alias the
	// material's 2D samplers, which is an invalid draw.
	if (m_uniforms.multiLightShadowArray >= 0) glUniform1i(m_uniforms.multiLightShadowArray,
		TextureUnits::SHADOW_MAP_ARRAY);

	bool shadowsAvailable = false;
	if (ctx.lightManager && ctx.lightManager->GetActiveLightCount() > 0) {
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
		if (m_uniforms.numLights >= 0) glUniform1i(m_uniforms.numLights,
			ctx.lightManager->GetActiveLightCount());

		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Bound " << ctx.lightManager->GetActiveLightCount()
			<< " active lights" << std::endl; }

		GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
		if (shadowArray > 0 && glIsTexture(shadowArray)) {
			glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
			glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
			shadowsAvailable = true;
		}

		const auto& shadowConfig = ctx.lightManager->shadowConfig;
		const float nearPlane = std::max(ctx.shadowNear, camera->GetCameraNearPlane());
		const float farPlane = std::max(nearPlane + 1.0f, std::min(ctx.shadowFar, camera->GetCameraFarPlane()));
		const glm::vec4 cascadeSplits = ctx.lightManager->ComputeCascadeSplitVector(nearPlane, farPlane);
		if (m_uniforms.cascadeSplits >= 0) glUniform4fv(m_uniforms.cascadeSplits, 1, glm::value_ptr(cascadeSplits));
		if (m_uniforms.cascadeBlendDistance >= 0) glUniform1f(m_uniforms.cascadeBlendDistance, shadowConfig.cascadeBlendDistance);
		if (m_uniforms.cascadeBlendFactor >= 0) glUniform1f(m_uniforms.cascadeBlendFactor, shadowConfig.cascadeBlendFactor);
		if (m_uniforms.shadowBias >= 0) glUniform1f(m_uniforms.shadowBias, ctx.shadowBias);
		if (m_uniforms.shadowDarkness >= 0) glUniform1f(m_uniforms.shadowDarkness, ctx.shadowDarkness);
		if (m_uniforms.shadowMinBrightness >= 0) glUniform1f(m_uniforms.shadowMinBrightness, ctx.shadowMinBrightness);
		if (m_uniforms.shadowTransitionHardness >= 0) glUniform1f(m_uniforms.shadowTransitionHardness, ctx.shadowTransitionHardness);
	}
	else {
		if (m_uniforms.numLights >= 0) glUniform1i(m_uniforms.numLights, 0);
		if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] No active lights available" << std::endl; }
	}
	if (m_uniforms.enableShadows >= 0) glUniform1i(m_uniforms.enableShadows, (ctx.enableShadows && shadowsAvailable) ? 1 : 0);

	// NOTE: Material-specific transmission and IOR uniforms are now set per-mesh
	// in Scene::Draw() rather than as pass-wide defaults here.
	// This allows each transparent material to use its own KHR_materials_transmission
	// and KHR_materials_ior extension values.

	renderSystem->RenderTransparent(ctx.view, ctx.proj, m_shader);

	//Restore render state for subsequent passes
	glDepthMask(GL_TRUE);      // Re-enable depth writes
	glDisable(GL_BLEND);       // Disable blending
	glDepthFunc(GL_LESS);      // Reset depth function to default

	// DO NOT unbind the HDR FBO - let the pipeline coordinator handle that

	if constexpr (VerboseLogging) { if (m_runtimeVerboseLogging) std::cout << "[TransparentForwardPass] Execution complete (HDR FBO remains bound)" << std::endl; }
}
