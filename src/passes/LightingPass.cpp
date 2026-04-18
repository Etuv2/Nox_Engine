#include "LightingPass.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include "../ShadowMapper.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>

LightingPass::LightingPass() {}

LightingPass::~LightingPass() {
	if (m_shader) glDeleteProgram(m_shader);
	// TexturePtr handles its own cleanup via shared_ptr
}

bool LightingPass::Initialize(RenderContext& context) {
	m_shader = CreateShaderProgram("shaders/deferred_lighting_vert.glsl",
		"shaders/deferred_lighting_frag.glsl");
	if (!m_shader) {
		std::cout << "[LightingPass] Trying fallback unified multi-light shader...\n";
		m_shader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
			"shaders/unified_multi_light_deferred.glsl");
		if (!m_shader) {
			std::cerr << "[LightingPass] Failed to create lighting shader.\n";
			return false;
		}
	}

	// Cache all uniform locations once at initialization
	CacheUniformLocations();

	SetupFallbackIBL();

	std::cout << "[LightingPass] Initialized successfully.\n";
	return true;
}

void LightingPass::CacheUniformLocations() {
	if (!m_shader) return;

	// Sampler uniforms
	m_uniforms.gPackedNormalRM = glGetUniformLocation(m_shader, "gPackedNormalRM");
	m_uniforms.gAlbedoAO = glGetUniformLocation(m_shader, "gAlbedoAO");
	m_uniforms.gSpecularF0 = glGetUniformLocation(m_shader, "gSpecularF0");
	m_uniforms.gMaterialID = glGetUniformLocation(m_shader, "gMaterialID");
	m_uniforms.gEmissive = glGetUniformLocation(m_shader, "gEmissive");
	m_uniforms.gClearCoat = glGetUniformLocation(m_shader, "gClearCoat");
	m_uniforms.gPrincipledParams = glGetUniformLocation(m_shader, "gPrincipledParams");
	m_uniforms.gDepth = glGetUniformLocation(m_shader, "gDepth");
	m_uniforms.ssaoMap = glGetUniformLocation(m_shader, "ssaoMap");
	m_uniforms.screenSpaceShadowMap = glGetUniformLocation(m_shader, "screenSpaceShadowMap");
	m_uniforms.ssgiMap = glGetUniformLocation(m_shader, "ssgiMap");
	m_uniforms.lpvTextureR = glGetUniformLocation(m_shader, "lpvTextureR");
	m_uniforms.lpvTextureG = glGetUniformLocation(m_shader, "lpvTextureG");
	m_uniforms.lpvTextureB = glGetUniformLocation(m_shader, "lpvTextureB");
	m_uniforms.irradianceMap = glGetUniformLocation(m_shader, "irradianceMap");
	m_uniforms.prefilteredMap = glGetUniformLocation(m_shader, "prefilteredMap");
	m_uniforms.brdfLUT = glGetUniformLocation(m_shader, "brdfLUT");
	m_uniforms.multiLightShadowArray = glGetUniformLocation(m_shader, "multiLightShadowArray");

	// Matrix uniforms
	m_uniforms.invProjection = glGetUniformLocation(m_shader, "invProjection");
	m_uniforms.invView = glGetUniformLocation(m_shader, "invView");
	m_uniforms.view = glGetUniformLocation(m_shader, "view");
	m_uniforms.viewPos = glGetUniformLocation(m_shader, "viewPos");

	// IBL uniforms
	m_uniforms.prefilteredMaxLOD = glGetUniformLocation(m_shader, "prefilteredMaxLOD");
	m_uniforms.iblIntensity = glGetUniformLocation(m_shader, "iblIntensity");
	m_uniforms.diffuseIBLScale = glGetUniformLocation(m_shader, "diffuseIBLScale");
	m_uniforms.specularIBLScale = glGetUniformLocation(m_shader, "specularIBLScale");

	// Effect strength uniforms
	m_uniforms.aoStrength = glGetUniformLocation(m_shader, "aoStrength");
	m_uniforms.sssStrength = glGetUniformLocation(m_shader, "sssStrength");
	m_uniforms.ssgiStrength = glGetUniformLocation(m_shader, "ssgiStrength");
	m_uniforms.ssgiDebugMode = glGetUniformLocation(m_shader, "ssgiDebugMode");

	// LPV uniforms
	m_uniforms.enableLPV = glGetUniformLocation(m_shader, "enableLPV");
	m_uniforms.lpvGridCenter = glGetUniformLocation(m_shader, "lpvGridCenter");
	m_uniforms.lpvGridResolution = glGetUniformLocation(m_shader, "lpvGridResolution");
	m_uniforms.lpvVoxelSize = glGetUniformLocation(m_shader, "lpvVoxelSize");
	m_uniforms.lpvGIStrength = glGetUniformLocation(m_shader, "lpvGIStrength");
	m_uniforms.lpvGridOrientation = glGetUniformLocation(m_shader, "lpvGridOrientation");
	m_uniforms.lpvDebugVisualization = glGetUniformLocation(m_shader, "lpvDebugVisualization");
	m_uniforms.lpvDebugBoost = glGetUniformLocation(m_shader, "lpvDebugBoost");

	// Light uniforms
	m_uniforms.numLights = glGetUniformLocation(m_shader, "numLights");
	m_uniforms.numDirectionalLights = glGetUniformLocation(m_shader, "numDirectionalLights");
	m_uniforms.numPointLights = glGetUniformLocation(m_shader, "numPointLights");
	m_uniforms.numSpotLights = glGetUniformLocation(m_shader, "numSpotLights");
	m_uniforms.enableShadows = glGetUniformLocation(m_shader, "enableShadows");
	m_uniforms.shadowBias = glGetUniformLocation(m_shader, "shadowBias");
	m_uniforms.maxShadowBias = glGetUniformLocation(m_shader, "maxShadowBias");
	m_uniforms.normalOffsetScale = glGetUniformLocation(m_shader, "normalOffsetScale");
	m_uniforms.cascadeBiasScale = glGetUniformLocation(m_shader, "cascadeBiasScale");
	m_uniforms.cascadeCount = glGetUniformLocation(m_shader, "cascadeCount");
	
	// Cascade blend settings
	m_uniforms.cascadeBlendDistance = glGetUniformLocation(m_shader, "cascadeBlendDistance");
	m_uniforms.cascadeBlendFactor = glGetUniformLocation(m_shader, "cascadeBlendFactor");
	m_uniforms.cascadeSplits = glGetUniformLocation(m_shader, "cascadeSplits");
	m_uniforms.shadowDebugVisualization = glGetUniformLocation(m_shader, "shadowDebugVisualization");
	
	// Point light shadow settings
	m_uniforms.pointLightBias = glGetUniformLocation(m_shader, "pointLightBias");
	m_uniforms.pointLightSlopeBias = glGetUniformLocation(m_shader, "pointLightSlopeBias");
	m_uniforms.pointLightNormalOffset = glGetUniformLocation(m_shader, "pointLightNormalOffset");
	
	// Shadow darkness settings
	m_uniforms.shadowDarkness = glGetUniformLocation(m_shader, "shadowDarkness");
	m_uniforms.shadowMinBrightness = glGetUniformLocation(m_shader, "shadowMinBrightness");
	m_uniforms.shadowTransitionHardness = glGetUniformLocation(m_shader, "shadowTransitionHardness");

	m_uniformsCached = true;
}

void LightingPass::SetupFallbackIBL() {
	// Create white cubemap fallback using Texture builder
	// 1x1 white cube for basic IBL fallback
	m_fallbackCubemap = Texture::Builder::TextureCube(1, GL_RGB16F)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.Build();

	if (!m_fallbackCubemap || !m_fallbackCubemap->IsValid()) {
		std::cerr << "[LightingPass] Failed to create fallback cubemap" << std::endl;
		return;
	}

	// Fill each face with black pixel so "no valid IBL" does not inject ambient light.
	float blackPixel[3] = { 0.0f, 0.0f, 0.0f };
	m_fallbackCubemap->Bind(GL_TEXTURE0);
	for (int face = 0; face < 6; ++face) {
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F,
			1, 1, 0, GL_RGB, GL_FLOAT, blackPixel);
	}
	m_fallbackCubemap->Unbind();

	std::cout << "[LightingPass] Created fallback cubemap (ID: " << m_fallbackCubemap->ID() << ")" << std::endl;

	// Create neutral BRDF LUT fallback
	m_fallbackBRDF = Texture::Builder::Texture2D(1, 1, GL_RG16F)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.Build();

	if (!m_fallbackBRDF || !m_fallbackBRDF->IsValid()) {
		std::cerr << "[LightingPass] Failed to create fallback BRDF LUT" << std::endl;
		return;
	}

	// Fill with neutral gray values
	float brdfPixel[2] = { 0.5f, 0.5f };
	m_fallbackBRDF->Upload2D(0, 0, 0, 1, 1, GL_RG, GL_FLOAT, brdfPixel);

	std::cout << "[LightingPass] Created fallback BRDF LUT (ID: " << m_fallbackBRDF->ID() << ")" << std::endl;
}

void LightingPass::Resize(RenderContext& context, int newWidth, int newHeight) {
	// HDR FBO resized by coordinator
}

void LightingPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox) {

	if constexpr (VerboseLogging) {
		std::cout << "[LightingPass] Starting execution..." << std::endl;
	}

	if (!camera) {
		std::cerr << "[LightingPass] ERROR: No camera!" << std::endl;
		return;
	}

	if (!ctx.hdrFBO) {
		std::cerr << "[LightingPass] ERROR: HDR FBO is null!" << std::endl;
		return;
	}

	if (!ctx.gbufferFBO) {
		std::cerr << "[LightingPass] ERROR: G-buffer FBO is null!" << std::endl;
		return;
	}

	// Bind HDR FBO
	ctx.hdrFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Copy depth from G-buffer to HDR FBO
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.gbufferFBO->GetFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.hdrFBO->GetFBO());
	glBlitFramebuffer(0, 0, ctx.width, ctx.height,
		0, 0, ctx.width, ctx.height,
		GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.hdrFBO->GetFBO());

	// After depth copy, disable depth test for fullscreen quad
	glDisable(GL_DEPTH_TEST);

	glUseProgram(m_shader);

	// Bind G-buffer textures
	// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
	// RT3: R32UI - Material ID
	// RT4: RGBA16F - Emissive color (RGB)
	// RT5: R32UI - Transform ID
	// RT6: RG16F - Clearcoat factor + roughness
	// RT7: RGBA16F - Principled extras (transmission, IOR, reserved)

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_NORMAL);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_ALBEDO);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_SPECULAR);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(2));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_MATERIAL_ID);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(3));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_EMISSIVE_COLOR);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(4));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_CLEARCOAT);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(6));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_PRINCIPLED);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(7));

	glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_DEPTH);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

	// Bind SSAO texture (trust validity - texture ID 0 means disabled)
	glActiveTexture(GL_TEXTURE0 + TextureUnits::SSAO_MAP);
	if (m_ssaoTexture > 0) {
		glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);
	}

	// Bind Screen-Space Shadow texture
	glActiveTexture(GL_TEXTURE0 + TextureUnits::SCREEN_SPACE_SHADOW_MAP);
	if (m_sssTexture > 0) {
		glBindTexture(GL_TEXTURE_2D, m_sssTexture);
	}

	// Bind SSGI texture
	glActiveTexture(GL_TEXTURE0 + TextureUnits::SSGI_MAP);
	if (m_ssgiTexture > 0) {
		glBindTexture(GL_TEXTURE_2D, m_ssgiTexture);
	}

	// Bind LPV 3D textures for global illumination
	bool lpvEnabled = ctx.enableLPV && m_lpvTextureR > 0 && m_lpvTextureG > 0 && m_lpvTextureB > 0;

	if (lpvEnabled) {
		glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_R);
		glBindTexture(GL_TEXTURE_3D, m_lpvTextureR);

		glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_G);
		glBindTexture(GL_TEXTURE_3D, m_lpvTextureG);

		glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_B);
		glBindTexture(GL_TEXTURE_3D, m_lpvTextureB);
	}

	// Set sampler uniforms using cached locations
	glUniform1i(m_uniforms.gPackedNormalRM, TextureUnits::GBUFFER_NORMAL);
	glUniform1i(m_uniforms.gAlbedoAO, TextureUnits::GBUFFER_ALBEDO);
	glUniform1i(m_uniforms.gSpecularF0, TextureUnits::GBUFFER_SPECULAR);
	glUniform1i(m_uniforms.gMaterialID, TextureUnits::GBUFFER_MATERIAL_ID);
	glUniform1i(m_uniforms.gEmissive, TextureUnits::GBUFFER_EMISSIVE_COLOR);
	glUniform1i(m_uniforms.gClearCoat, TextureUnits::GBUFFER_CLEARCOAT);
	glUniform1i(m_uniforms.gPrincipledParams, TextureUnits::GBUFFER_PRINCIPLED);
	glUniform1i(m_uniforms.gDepth, TextureUnits::GBUFFER_DEPTH);
	glUniform1i(m_uniforms.ssaoMap, TextureUnits::SSAO_MAP);
	glUniform1i(m_uniforms.screenSpaceShadowMap, TextureUnits::SCREEN_SPACE_SHADOW_MAP);

	// Set LPV sampler uniforms
	if (lpvEnabled) {
		glUniform1i(m_uniforms.lpvTextureR, TextureUnits::LPV_TEXTURE_R);
		glUniform1i(m_uniforms.lpvTextureG, TextureUnits::LPV_TEXTURE_G);
		glUniform1i(m_uniforms.lpvTextureB, TextureUnits::LPV_TEXTURE_B);
	}

	// Bind IBL textures (skybox or fallback)
	bool useValidIBL = (skybox && skybox->ValidateIBLTextures());

	if (useValidIBL) {
		glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());

		glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());

		glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
		glBindTexture(GL_TEXTURE_2D, skybox->GetBRDFLUT());

		glUniform1f(m_uniforms.prefilteredMaxLOD, skybox->GetPrefilteredMaxLOD());
		glUniform1f(m_uniforms.iblIntensity, ctx.iblIntensity);
		glUniform1f(m_uniforms.diffuseIBLScale, ctx.diffuseIBLScale);
		glUniform1f(m_uniforms.specularIBLScale, ctx.specularIBLScale);
	}
	else {
		// Use fallback IBL
		if (m_fallbackCubemap && m_fallbackCubemap->IsValid()) {
			m_fallbackCubemap->Bind(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
			m_fallbackCubemap->Bind(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
		}

		if (m_fallbackBRDF && m_fallbackBRDF->IsValid()) {
			m_fallbackBRDF->Bind(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
		}

		glUniform1f(m_uniforms.prefilteredMaxLOD, 0.0f);
		glUniform1f(m_uniforms.iblIntensity, 0.0f);
		glUniform1f(m_uniforms.diffuseIBLScale, 0.0f);
		glUniform1f(m_uniforms.specularIBLScale, 0.0f);
	}

	glUniform1i(m_uniforms.irradianceMap, TextureUnits::IRRADIANCE_MAP);
	glUniform1i(m_uniforms.prefilteredMap, TextureUnits::PREFILTERED_ENV_MAP);
	glUniform1i(m_uniforms.brdfLUT, TextureUnits::BRDF_LUT);

	// Upload matrices
	glm::mat4 invProj = glm::inverse(ctx.proj);
	glm::mat4 invView = glm::inverse(ctx.view);
	glUniformMatrix4fv(m_uniforms.invProjection, 1, GL_FALSE, glm::value_ptr(invProj));
	glUniformMatrix4fv(m_uniforms.invView, 1, GL_FALSE, glm::value_ptr(invView));
	glUniformMatrix4fv(m_uniforms.view, 1, GL_FALSE, glm::value_ptr(ctx.view));

	glm::vec3 camPos = camera->GetCameraPosition();
	glUniform3fv(m_uniforms.viewPos, 1, glm::value_ptr(camPos));

	// SSAO strength
	float aoStrength = ctx.enableSSAO ? ctx.ssaoIntensity : 0.0f;
	glUniform1f(m_uniforms.aoStrength, aoStrength);

	// Screen-space shadow strength
	float sssStrength = ctx.enableScreenSpaceShadows ? std::clamp(ctx.sssBlendStrength, 0.0f, 1.0f) : 0.0f;
	glUniform1f(m_uniforms.sssStrength, sssStrength);

	// SSGI uniforms
	glUniform1i(m_uniforms.ssgiMap, TextureUnits::SSGI_MAP);
	glUniform1f(m_uniforms.ssgiStrength, ctx.enableSSGI ? ctx.ssgiStrength : 0.0f);
	glUniform1i(m_uniforms.ssgiDebugMode, ctx.enableSSGI ? ctx.ssgiDebugMode : 0);

	// Upload LPV parameters
	glUniform1i(m_uniforms.enableLPV, lpvEnabled ? 1 : 0);
	if (lpvEnabled) {
		glUniform3fv(m_uniforms.lpvGridCenter, 1, glm::value_ptr(ctx.lpvGridCenter));
		glUniform1i(m_uniforms.lpvGridResolution, ctx.lpvGridResolution);
		glUniform1f(m_uniforms.lpvVoxelSize, ctx.lpvVoxelSize);
		glUniform1f(m_uniforms.lpvGIStrength, ctx.lpvGIStrength);

		glm::vec4 orientQuat = glm::vec4(ctx.lpvGridOrientation.x, ctx.lpvGridOrientation.y,
			ctx.lpvGridOrientation.z, ctx.lpvGridOrientation.w);
		glUniform4fv(m_uniforms.lpvGridOrientation, 1, glm::value_ptr(orientQuat));

		glUniform1i(m_uniforms.lpvDebugVisualization, ctx.lpvDebugVisualization ? 1 : 0);
		glUniform1f(m_uniforms.lpvDebugBoost, ctx.lpvDebugBoost);
	}

	// Bind LightManager data
	if (ctx.lightManager) {

		int activeLightCount = ctx.lightManager->GetActiveLightCount();

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ctx.lightManager->GetTileDataSSBO());

		glUniform1i(m_uniforms.numLights, activeLightCount);
		glUniform1i(m_uniforms.numDirectionalLights, static_cast<int>(ctx.lightManager->GetDirectionalLightCount()));
		glUniform1i(m_uniforms.numPointLights, static_cast<int>(ctx.lightManager->GetPointLightCount()));
		glUniform1i(m_uniforms.numSpotLights, static_cast<int>(ctx.lightManager->GetSpotLightCount()));

		glUniform1i(m_uniforms.enableShadows, ctx.enableShadows ? 1 : 0);

		// Bind shadow array
		GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
		if (shadowArray > 0) {
			glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
			glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
			glUniform1i(m_uniforms.multiLightShadowArray, TextureUnits::SHADOW_MAP_ARRAY);
		}

		// Shadow bias configuration
		glUniform1f(m_uniforms.shadowBias, ctx.shadowBias);
		glUniform1f(m_uniforms.maxShadowBias, ctx.shadowBias * 10.0f);
		glUniform1f(m_uniforms.normalOffsetScale, 0.01f);  // Reduced from 0.1f to minimize floating shadows
		glUniform1f(m_uniforms.cascadeBiasScale, 1.0f);

		// Cascade blend settings from LightManager config
		const auto& shadowConfig = ctx.lightManager->shadowConfig;
		glUniform1f(m_uniforms.cascadeBlendDistance, shadowConfig.cascadeBlendDistance);
		glUniform1f(m_uniforms.cascadeBlendFactor, shadowConfig.cascadeBlendFactor);
		
		// Calculate cascade splits based on camera near/far planes
		// Use the SAME split calculation as LightManager::RenderShadowMaps
		// to ensure cascade selection in shader matches rendered cascade bounds
		float nearPlane = ctx.shadowNear;
		float farPlane = ctx.shadowFar;
		
		// Use ShadowMapper's cascade split function for consistency
		const int cascadeCount = std::max(1, shadowConfig.directionalCascadeCount);
		std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(
			nearPlane,
			farPlane,
			cascadeCount,
			shadowConfig.directionalSplitLambda
		);
		glm::vec4 cascadeSplits(0.0f);
		for (int i = 0; i < std::min(4, static_cast<int>(splits.size())); ++i) {
			cascadeSplits[i] = splits[i];
		}
		glUniform4fv(m_uniforms.cascadeSplits, 1, glm::value_ptr(cascadeSplits));
		const int shadowDebugVisualization =
			ctx.debugMode == RenderContext::DebugMode::SHADOW_MAPS
			? static_cast<int>(ctx.shadowDebugVisualization)
			: 0;
		glUniform1i(m_uniforms.shadowDebugVisualization, shadowDebugVisualization);
		
		// Point light shadow settings
		glUniform1f(m_uniforms.pointLightBias, shadowConfig.pointLightBias);
		glUniform1f(m_uniforms.pointLightSlopeBias, shadowConfig.pointLightSlopeBias);
		glUniform1f(m_uniforms.pointLightNormalOffset, shadowConfig.pointLightNormalOffset);
		
		// Shadow darkness settings - realistic shadow rendering
		glUniform1f(m_uniforms.shadowDarkness, ctx.shadowDarkness);
		glUniform1f(m_uniforms.shadowMinBrightness, ctx.shadowMinBrightness);
		glUniform1f(m_uniforms.shadowTransitionHardness, ctx.shadowTransitionHardness);

		glUniform1i(m_uniforms.cascadeCount, cascadeCount);
	}
	else {
		// No lights
		glUniform1i(m_uniforms.numLights, 0);
		glUniform1i(m_uniforms.numDirectionalLights, 0);
		glUniform1i(m_uniforms.numPointLights, 0);
		glUniform1i(m_uniforms.numSpotLights, 0);
		glUniform1i(m_uniforms.enableShadows, 0);
		glUniform1i(m_uniforms.cascadeCount, 0);
		glUniform1i(m_uniforms.shadowDebugVisualization, 0);
	}

	// Render fullscreen quad
	if (ctx.screenQuad) {
		ctx.screenQuad->Render();
	}
	else {
		std::cerr << "[LightingPass] ERROR: ScreenQuad is null!" << std::endl;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
