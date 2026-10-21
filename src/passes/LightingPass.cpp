#include "LightingPass.h"
#include "../PresentTarget.h"
#include "LightClusterPass.h"
#include "ExposurePass.h"
#include "ReflectionProbePass.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>

LightingPass::LightingPass() {}

LightingPass::~LightingPass() {
	if (m_shader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_shader);
	// TexturePtr handles its own cleanup via shared_ptr
}

void LightingPass::ClearIndirectDiffuseSources() {
	m_indirectDiffuseTextures.fill({});
	m_indirectDiffuseStrengths.fill(0.0f);
	m_indirectDiffuseSourceCount = 0;
}

void LightingPass::SetIndirectDiffuseSource(int index, rhi::TextureHandle indirectDiffuseTex, float strength) {
	if (index < 0 || index >= MaxIndirectDiffuseSources) {
		return;
	}

	m_indirectDiffuseTextures[static_cast<size_t>(index)] = indirectDiffuseTex;
	m_indirectDiffuseStrengths[static_cast<size_t>(index)] = indirectDiffuseTex.IsValid() ? std::max(strength, 0.0f) : 0.0f;

	if (indirectDiffuseTex.IsValid() && strength > 0.0f) {
		m_indirectDiffuseSourceCount = std::max(m_indirectDiffuseSourceCount, index + 1);
	}
	else if (index + 1 == m_indirectDiffuseSourceCount) {
		while (m_indirectDiffuseSourceCount > 0) {
			const int last = m_indirectDiffuseSourceCount - 1;
			if (m_indirectDiffuseTextures[static_cast<size_t>(last)].IsValid() &&
				m_indirectDiffuseStrengths[static_cast<size_t>(last)] > 0.0f) {
				break;
			}
			--m_indirectDiffuseSourceCount;
		}
	}
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
	rhi::Gpu().UseProgram(m_shader);
	UploadStaticSamplerUniforms();
	rhi::Gpu().UseProgram({});

	SetupFallbackIBL();

	std::cout << "[LightingPass] Initialized successfully.\n";
	return true;
}

void LightingPass::CacheUniformLocations() {
	if (!m_shader) return;

	rhi::Device& gpu = rhi::Gpu();

	// Sampler uniforms
	m_uniforms.gPackedNormalRM = gpu.GetUniformLocation(m_shader, "gPackedNormalRM");
	m_uniforms.gAlbedoAO = gpu.GetUniformLocation(m_shader, "gAlbedoAO");
	m_uniforms.gSpecularF0 = gpu.GetUniformLocation(m_shader, "gSpecularF0");
	m_uniforms.gMaterialID = gpu.GetUniformLocation(m_shader, "gMaterialID");
	m_uniforms.gEmissive = gpu.GetUniformLocation(m_shader, "gEmissive");
	m_uniforms.gClearCoat = gpu.GetUniformLocation(m_shader, "gClearCoat");
	m_uniforms.gPrincipledParams = gpu.GetUniformLocation(m_shader, "gPrincipledParams");
	m_uniforms.gDepth = gpu.GetUniformLocation(m_shader, "gDepth");
	m_uniforms.ssaoMap = gpu.GetUniformLocation(m_shader, "ssaoMap");
	m_uniforms.screenSpaceShadowMap = gpu.GetUniformLocation(m_shader, "screenSpaceShadowMap");
	m_uniforms.indirectDiffuseMaps = gpu.GetUniformLocation(m_shader, "indirectDiffuseMaps[0]");
	m_uniforms.irradianceMap = gpu.GetUniformLocation(m_shader, "irradianceMap");
	m_uniforms.prefilteredMap = gpu.GetUniformLocation(m_shader, "prefilteredMap");
	m_uniforms.brdfLUT = gpu.GetUniformLocation(m_shader, "brdfLUT");
	m_uniforms.multiLightShadowArray = gpu.GetUniformLocation(m_shader, "multiLightShadowArray");

	// Matrix uniforms
	m_uniforms.invProjection = gpu.GetUniformLocation(m_shader, "invProjection");
	m_uniforms.invView = gpu.GetUniformLocation(m_shader, "invView");
	m_uniforms.view = gpu.GetUniformLocation(m_shader, "view");
	m_uniforms.viewPos = gpu.GetUniformLocation(m_shader, "viewPos");

	// IBL uniforms
	m_uniforms.prefilteredMaxLOD = gpu.GetUniformLocation(m_shader, "prefilteredMaxLOD");
	m_uniforms.iblIntensity = gpu.GetUniformLocation(m_shader, "iblIntensity");
	m_uniforms.diffuseIBLScale = gpu.GetUniformLocation(m_shader, "diffuseIBLScale");
	m_uniforms.specularIBLScale = gpu.GetUniformLocation(m_shader, "specularIBLScale");

	// Effect strength uniforms
	m_uniforms.aoStrength = gpu.GetUniformLocation(m_shader, "aoStrength");
	m_uniforms.sssStrength = gpu.GetUniformLocation(m_shader, "sssStrength");
	m_uniforms.indirectDiffuseStrengths = gpu.GetUniformLocation(m_shader, "indirectDiffuseStrengths[0]");
	m_uniforms.indirectDiffuseSourceCount = gpu.GetUniformLocation(m_shader, "indirectDiffuseSourceCount");
	m_uniforms.indirectDiffuseCompositeMode = gpu.GetUniformLocation(m_shader, "indirectDiffuseCompositeMode");
	m_uniforms.lightingOutputMode = gpu.GetUniformLocation(m_shader, "lightingOutputMode");
	m_uniforms.lightingCompositeDebugMode = gpu.GetUniformLocation(m_shader, "uLightingCompositeDebugMode");
	m_uniforms.lightingGIDebugExposure = gpu.GetUniformLocation(m_shader, "uLightingGIDebugExposure");

	// Light uniforms
	m_uniforms.numLights = gpu.GetUniformLocation(m_shader, "numLights");
	m_uniforms.numDirectionalLights = gpu.GetUniformLocation(m_shader, "numDirectionalLights");
	m_uniforms.numPointLights = gpu.GetUniformLocation(m_shader, "numPointLights");
	m_uniforms.numSpotLights = gpu.GetUniformLocation(m_shader, "numSpotLights");
	m_uniforms.enableShadows = gpu.GetUniformLocation(m_shader, "enableShadows");
	m_uniforms.shadowBias = gpu.GetUniformLocation(m_shader, "shadowBias");
	m_uniforms.cascadeCount = gpu.GetUniformLocation(m_shader, "cascadeCount");

	// Cascade blend settings
	m_uniforms.cascadeBlendDistance = gpu.GetUniformLocation(m_shader, "cascadeBlendDistance");
	m_uniforms.cascadeBlendFactor = gpu.GetUniformLocation(m_shader, "cascadeBlendFactor");
	m_uniforms.cascadeSplits = gpu.GetUniformLocation(m_shader, "cascadeSplits");
	m_uniforms.shadowDebugVisualization = gpu.GetUniformLocation(m_shader, "shadowDebugVisualization");

	// Shadow darkness settings
	m_uniforms.shadowDarkness = gpu.GetUniformLocation(m_shader, "shadowDarkness");
	m_uniforms.shadowMinBrightness = gpu.GetUniformLocation(m_shader, "shadowMinBrightness");
	m_uniforms.shadowTransitionHardness = gpu.GetUniformLocation(m_shader, "shadowTransitionHardness");

	m_uniformsCached = true;
}

void LightingPass::UploadStaticSamplerUniforms() {
	if (!m_shader) return;

	rhi::Device& gpu = rhi::Gpu();

	if (m_uniforms.gPackedNormalRM >= 0) gpu.SetUniform(m_uniforms.gPackedNormalRM, TextureUnits::GBUFFER_NORMAL);
	if (m_uniforms.gAlbedoAO >= 0) gpu.SetUniform(m_uniforms.gAlbedoAO, TextureUnits::GBUFFER_ALBEDO);
	if (m_uniforms.gSpecularF0 >= 0) gpu.SetUniform(m_uniforms.gSpecularF0, TextureUnits::GBUFFER_SPECULAR);
	if (m_uniforms.gMaterialID >= 0) gpu.SetUniform(m_uniforms.gMaterialID, TextureUnits::GBUFFER_MATERIAL_ID);
	if (m_uniforms.gEmissive >= 0) gpu.SetUniform(m_uniforms.gEmissive, TextureUnits::GBUFFER_EMISSIVE_COLOR);
	if (m_uniforms.gClearCoat >= 0) gpu.SetUniform(m_uniforms.gClearCoat, TextureUnits::GBUFFER_CLEARCOAT);
	if (m_uniforms.gPrincipledParams >= 0) gpu.SetUniform(m_uniforms.gPrincipledParams, TextureUnits::GBUFFER_PRINCIPLED);
	if (m_uniforms.gDepth >= 0) gpu.SetUniform(m_uniforms.gDepth, TextureUnits::GBUFFER_DEPTH);
	if (m_uniforms.ssaoMap >= 0) gpu.SetUniform(m_uniforms.ssaoMap, TextureUnits::SSAO_MAP);
	if (m_uniforms.screenSpaceShadowMap >= 0) gpu.SetUniform(m_uniforms.screenSpaceShadowMap, TextureUnits::SCREEN_SPACE_SHADOW_MAP);
	if (m_uniforms.indirectDiffuseMaps >= 0) {
		int units[MaxIndirectDiffuseSources]{};
		for (int i = 0; i < MaxIndirectDiffuseSources; ++i) {
			units[i] = TextureUnits::INDIRECT_DIFFUSE_SOURCE_BASE + i;
		}
		gpu.SetUniformArray(m_uniforms.indirectDiffuseMaps, units, MaxIndirectDiffuseSources);
	}
	if (m_uniforms.irradianceMap >= 0) gpu.SetUniform(m_uniforms.irradianceMap, TextureUnits::IRRADIANCE_MAP);
	if (m_uniforms.prefilteredMap >= 0) gpu.SetUniform(m_uniforms.prefilteredMap, TextureUnits::PREFILTERED_ENV_MAP);
	if (m_uniforms.brdfLUT >= 0) gpu.SetUniform(m_uniforms.brdfLUT, TextureUnits::BRDF_LUT);
	if (m_uniforms.multiLightShadowArray >= 0) gpu.SetUniform(m_uniforms.multiLightShadowArray, TextureUnits::SHADOW_MAP_ARRAY);
}

void LightingPass::SetupFallbackIBL() {
	// Create white cubemap fallback using Texture builder
	// 1x1 white cube for basic IBL fallback
	m_fallbackCubemap = Texture::Builder::TextureCube(1, rhi::Format::RGB16F)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.Build();

	if (!m_fallbackCubemap || !m_fallbackCubemap->IsValid()) {
		std::cerr << "[LightingPass] Failed to create fallback cubemap" << std::endl;
		return;
	}

	// Fill each face with black pixel so "no valid IBL" does not inject ambient light.
	float blackPixel[3] = { 0.0f, 0.0f, 0.0f };
	rhi::Device& gpu = rhi::Gpu();
	m_fallbackCubemap->Bind(0);
	for (int face = 0; face < 6; ++face) {
		// Cube face = array layer z of the cube texture.
		rhi::TextureRegion region;
		region.level = 0;
		region.offset = glm::ivec3(0, 0, face);
		region.extent = glm::ivec3(1, 1, 1);
		gpu.UpdateTexture(m_fallbackCubemap->ID(), region, rhi::PixelFormat::RGB, rhi::PixelType::Float, blackPixel);
	}
	gpu.BindTexture(0, {});   // Unbind

	std::cout << "[LightingPass] Created fallback cubemap (ID: " << m_fallbackCubemap->ID() << ")" << std::endl;

	// Create neutral BRDF LUT fallback
	m_fallbackBRDF = Texture::Builder::Texture2D(1, 1, rhi::Format::RG16F)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.Build();

	if (!m_fallbackBRDF || !m_fallbackBRDF->IsValid()) {
		std::cerr << "[LightingPass] Failed to create fallback BRDF LUT" << std::endl;
		return;
	}

	// Fill with neutral gray values
	float brdfPixel[2] = { 0.5f, 0.5f };
	m_fallbackBRDF->Upload2D(0, 0, 0, 1, 1, rhi::PixelFormat::RG, rhi::PixelType::Float, brdfPixel);

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

	rhi::Device& gpu = rhi::Gpu();

	// Bind HDR FBO
	ctx.hdrFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetBlendEnabled(false);
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);
	gpu.SetDepthWrite(true);

	gpu.Clear(rhi::ClearColor | rhi::ClearDepth);

	// Copy depth from G-buffer to HDR FBO
	gpu.BindReadFramebuffer(ctx.gbufferFBO->GetFBO());
	gpu.BindDrawFramebuffer(ctx.hdrFBO->GetFBO());
	gpu.BlitFramebuffer(ctx.gbufferFBO->GetFBO(), 0, { 0, 0, ctx.width, ctx.height },
		ctx.hdrFBO->GetFBO(), { 0, 0, ctx.width, ctx.height }, rhi::ClearDepth, rhi::BlitFilter::Nearest);
	gpu.BindFramebuffer(ctx.hdrFBO->GetFBO());

	// After depth copy, disable depth test for fullscreen quad
	gpu.SetDepthTest(false);

	gpu.UseProgram(m_shader);

	// Bind G-buffer textures
	// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
	// RT3: R32UI - Material ID
	// RT4: RGBA16F - Emissive color (RGB)
	// RT5: R32UI - Transform ID
	// RT6: RG16F - Clearcoat factor + roughness
	// RT7: RGBA16F - Principled extras (transmission, IOR, reserved)

	gpu.BindTexture(TextureUnits::GBUFFER_NORMAL, ctx.gbufferFBO->GetColorAttachment(0));
	gpu.BindTexture(TextureUnits::GBUFFER_ALBEDO, ctx.gbufferFBO->GetColorAttachment(1));
	gpu.BindTexture(TextureUnits::GBUFFER_SPECULAR, ctx.gbufferFBO->GetColorAttachment(2));
	gpu.BindTexture(TextureUnits::GBUFFER_MATERIAL_ID, ctx.gbufferFBO->GetColorAttachment(3));
	gpu.BindTexture(TextureUnits::GBUFFER_EMISSIVE_COLOR, ctx.gbufferFBO->GetColorAttachment(4));
	gpu.BindTexture(TextureUnits::GBUFFER_CLEARCOAT, ctx.gbufferFBO->GetColorAttachment(6));
	gpu.BindTexture(TextureUnits::GBUFFER_PRINCIPLED, ctx.gbufferFBO->GetColorAttachment(7));
	gpu.BindTexture(TextureUnits::GBUFFER_DEPTH, ctx.gbufferFBO->GetDepthTexture());

	// Bind SSAO texture (trust validity - texture ID 0 means disabled)
	if (m_ssaoTexture.IsValid()) {
		gpu.BindTexture(TextureUnits::SSAO_MAP, m_ssaoTexture);
	}

	// Bind Screen-Space Shadow texture
	if (m_sssTexture.IsValid()) {
		gpu.BindTexture(TextureUnits::SCREEN_SPACE_SHADOW_MAP, m_sssTexture);
	}

	// Local reflection probes (ReflectionProbePass, docs/nfs2015 M5).
	ReflectionProbePass::BindForShading(ctx);
	SET_UNIFORM_TEXTURE_UNIT(m_shader, "reflectionProbes", TextureUnits::REFLECTION_PROBES);
	if (const rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "reflectionProbeCount"); loc >= 0) {
		gpu.SetUniform(loc, m_outputMode == OutputMode::FullLighting ? ctx.reflectionProbeCount : 0);
	}
	if (const rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "reflectionProbeMaxLOD"); loc >= 0) {
		gpu.SetUniform(loc, ctx.reflectionProbeMaxLOD);
	}
	if (const rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "ssrProbeBlend"); loc >= 0) {
		gpu.SetUniform(loc, std::clamp(ctx.ssrProbeBlend, 0.0f, 1.0f));
	}
	if (const rhi::UniformLocation loc = gpu.GetUniformLocation(m_shader, "ssrDebugOnly"); loc >= 0) {
		gpu.SetUniform(loc, (m_outputMode == OutputMode::FullLighting && ctx.ssrDebugView != 0) ? 1 : 0);
	}

	// Screen-space reflections (SSRPass, docs/nfs2015 M5); the bounce-radiance mode does not use them.
	{
		const bool hasSSR = ctx.ssrTexture.IsValid() && m_outputMode == OutputMode::FullLighting;
		gpu.BindTexture(TextureUnits::SSR_REFLECTION, hasSSR ? ctx.ssrTexture : rhi::TextureHandle{});
		SET_UNIFORM_TEXTURE_UNIT(m_shader, "ssrReflection", TextureUnits::SSR_REFLECTION);
		const rhi::UniformLocation hasSSRLoc = gpu.GetUniformLocation(m_shader, "hasSSR");
		if (hasSSRLoc >= 0) gpu.SetUniform(hasSSRLoc, hasSSR ? 1 : 0);
	}

	for (int i = 0; i < MaxIndirectDiffuseSources; ++i) {
		gpu.BindTexture(TextureUnits::INDIRECT_DIFFUSE_SOURCE_BASE + i, m_indirectDiffuseTextures[static_cast<size_t>(i)]);
	}

	bool useValidIBL = skybox && skybox->ValidateIBLTextures();

	if (useValidIBL) {
		gpu.BindTexture(TextureUnits::IRRADIANCE_MAP, skybox->GetIrradianceMap());
		gpu.BindTexture(TextureUnits::PREFILTERED_ENV_MAP, skybox->GetPrefilteredMap());
		gpu.BindTexture(TextureUnits::BRDF_LUT, skybox->GetBRDFLUT());

		gpu.SetUniform(m_uniforms.prefilteredMaxLOD, skybox->GetPrefilteredMaxLOD());
		gpu.SetUniform(m_uniforms.iblIntensity, ctx.iblIntensity);
		gpu.SetUniform(m_uniforms.diffuseIBLScale, ctx.diffuseIBLScale);
		gpu.SetUniform(m_uniforms.specularIBLScale, ctx.specularIBLScale);
	}
	else {
		// Use fallback IBL
		if (m_fallbackCubemap && m_fallbackCubemap->IsValid()) {
			m_fallbackCubemap->Bind(TextureUnits::IRRADIANCE_MAP);
			m_fallbackCubemap->Bind(TextureUnits::PREFILTERED_ENV_MAP);
		}

		if (m_fallbackBRDF && m_fallbackBRDF->IsValid()) {
			m_fallbackBRDF->Bind(TextureUnits::BRDF_LUT);
		}

		gpu.SetUniform(m_uniforms.prefilteredMaxLOD, 0.0f);
		gpu.SetUniform(m_uniforms.iblIntensity, 0.0f);
		gpu.SetUniform(m_uniforms.diffuseIBLScale, 0.0f);
		gpu.SetUniform(m_uniforms.specularIBLScale, 0.0f);
	}
	// Upload matrices
	glm::mat4 invProj = glm::inverse(ctx.proj);
	glm::mat4 invView = glm::inverse(ctx.view);
	gpu.SetUniform(m_uniforms.invProjection, invProj);
	gpu.SetUniform(m_uniforms.invView, invView);
	gpu.SetUniform(m_uniforms.view, ctx.view);

	glm::vec3 camPos = camera->GetCameraPosition();
	gpu.SetUniform(m_uniforms.viewPos, camPos);

	// SSAO strength
	float aoStrength = ctx.enableSSAO ? ctx.ssaoIntensity : 0.0f;
	gpu.SetUniform(m_uniforms.aoStrength, aoStrength);

	// Screen-space shadow strength
	float sssStrength = ctx.enableScreenSpaceShadows ? std::clamp(ctx.sssBlendStrength, 0.0f, 1.0f) : 0.0f;
	gpu.SetUniform(m_uniforms.sssStrength, sssStrength);

	gpu.SetUniformArray(m_uniforms.indirectDiffuseStrengths, m_indirectDiffuseStrengths.data(), MaxIndirectDiffuseSources);
	gpu.SetUniform(m_uniforms.indirectDiffuseSourceCount, m_indirectDiffuseSourceCount);
	gpu.SetUniform(
		m_uniforms.indirectDiffuseCompositeMode,
		ctx.enableIndirectDiffuse ? ctx.indirectDiffuseCompositeMode : 0);
	gpu.SetUniform(m_uniforms.lightingOutputMode, static_cast<int>(m_outputMode));

	if (m_uniforms.lightingCompositeDebugMode >= 0) {
		gpu.SetUniform(m_uniforms.lightingCompositeDebugMode, std::clamp(ctx.lightingCompositeDebugMode, 0, 5));
	}
	if (m_uniforms.lightingGIDebugExposure >= 0) {
		gpu.SetUniform(m_uniforms.lightingGIDebugExposure, std::max(ctx.lightingGIDebugExposure, 0.0f));
	}

	// Clustered light lists and exposure state (docs/nfs2015 M2/M3).
	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);

	// Bind LightManager data
	if (ctx.lightManager) {

		int activeLightCount = ctx.lightManager->GetActiveLightCount();

		gpu.BindStorageBuffer(0, ctx.lightManager->GetLightDataSSBO());
		gpu.BindStorageBuffer(1, ctx.lightManager->GetShadowMatricesSSBO());
		gpu.BindStorageBuffer(2, ctx.lightManager->GetTileDataSSBO());

		gpu.SetUniform(m_uniforms.numLights, activeLightCount);
		gpu.SetUniform(m_uniforms.numDirectionalLights, static_cast<int>(ctx.lightManager->GetDirectionalLightCount()));
		gpu.SetUniform(m_uniforms.numPointLights, static_cast<int>(ctx.lightManager->GetPointLightCount()));
		gpu.SetUniform(m_uniforms.numSpotLights, static_cast<int>(ctx.lightManager->GetSpotLightCount()));

		gpu.SetUniform(m_uniforms.enableShadows, ctx.enableShadows ? 1 : 0);

		// Bind shadow array
		rhi::TextureHandle shadowArray = ctx.lightManager->GetShadowArrayTexture();
		if (shadowArray.IsValid()) {
			gpu.BindTexture(TextureUnits::SHADOW_MAP_ARRAY, shadowArray);
		}

		// Shadow bias configuration
		gpu.SetUniform(m_uniforms.shadowBias, ctx.shadowBias);

		// Cascade blend settings from LightManager config
		const auto& shadowConfig = ctx.lightManager->shadowConfig;
		gpu.SetUniform(m_uniforms.cascadeBlendDistance, shadowConfig.cascadeBlendDistance);
		gpu.SetUniform(m_uniforms.cascadeBlendFactor, shadowConfig.cascadeBlendFactor);

		// Calculate cascade splits based on camera near/far planes
		// Use the SAME split calculation as LightManager::RenderShadowMaps
		// to ensure cascade selection in shader matches rendered cascade bounds
		float nearPlane = std::max(ctx.shadowNear, camera ? camera->GetCameraNearPlane() : ctx.shadowNear);
		float farPlane = std::max(nearPlane + 1.0f,
			std::min(ctx.shadowFar, camera ? camera->GetCameraFarPlane() : ctx.shadowFar));

		const int cascadeCount = std::max(1, shadowConfig.directionalCascadeCount);
		const glm::vec4 cascadeSplits = ctx.lightManager->ComputeCascadeSplitVector(nearPlane, farPlane);
		gpu.SetUniform(m_uniforms.cascadeSplits, cascadeSplits);
		const int shadowDebugVisualization =
			ctx.debugMode == RenderContext::DebugMode::SHADOW_MAPS
			? static_cast<int>(ctx.shadowDebugVisualization)
			: 0;
		gpu.SetUniform(m_uniforms.shadowDebugVisualization, shadowDebugVisualization);

		// Shadow darkness settings - realistic shadow rendering
		gpu.SetUniform(m_uniforms.shadowDarkness, ctx.shadowDarkness);
		gpu.SetUniform(m_uniforms.shadowMinBrightness, ctx.shadowMinBrightness);
		gpu.SetUniform(m_uniforms.shadowTransitionHardness, ctx.shadowTransitionHardness);

		gpu.SetUniform(m_uniforms.cascadeCount, cascadeCount);
	}
	else {
		// No lights
		gpu.SetUniform(m_uniforms.numLights, 0);
		gpu.SetUniform(m_uniforms.numDirectionalLights, 0);
		gpu.SetUniform(m_uniforms.numPointLights, 0);
		gpu.SetUniform(m_uniforms.numSpotLights, 0);
		gpu.SetUniform(m_uniforms.enableShadows, 0);
		gpu.SetUniform(m_uniforms.cascadeCount, 0);
		gpu.SetUniform(m_uniforms.shadowDebugVisualization, 0);
	}

	// Render fullscreen quad
	if (ctx.screenQuad) {
		ctx.screenQuad->Render();
	}
	else {
		std::cerr << "[LightingPass] ERROR: ScreenQuad is null!" << std::endl;
	}

	gpu.BindFramebuffer(PresentTarget::Get());
}
