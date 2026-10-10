#pragma once
#include "../RenderPass.h"
#include "../Texture.h"
#include <array>
#include <memory>

/**
 * LightingPass performs deferred lighting using G-buffer data
 * It combines lighting information from various sources (e.g., directional lights, point lights)
 * and applies it to the scene geometry.
 * REFERENCES:
 * - LearnOpenGL.com. (n.d.). Deferred Shading: https://learnopengl.com/Advanced-Lighting/Deferred-Shading
 */
class LightingPass : public RenderPass {
public:
	enum class OutputMode {
		FullLighting = 0,
		BounceableRadiance = 1
	};

	LightingPass();
	~LightingPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	//Allow SSAOPass to provide its output texture
	void SetSSAOTexture(rhi::TextureHandle ssaoTex) { m_ssaoTexture = ssaoTex; }

	//Allow ScreenSpaceShadowPass to provide its output texture
	void SetScreenSpaceShadowTexture(rhi::TextureHandle sssTex) { m_sssTexture = sssTex; }

	void ClearIndirectDiffuseSources();
	void SetIndirectDiffuseSource(int index, rhi::TextureHandle indirectDiffuseTex, float strength = 1.0f);
	void SetOutputMode(OutputMode mode) { m_outputMode = mode; }

	// Enable/disable verbose logging (disabled by default for performance)
	static constexpr bool VerboseLogging = false;
	static constexpr int MaxIndirectDiffuseSources = 4;

private:
	void SetupFallbackIBL();
	void CacheUniformLocations();
	void UploadStaticSamplerUniforms();

	rhi::ProgramHandle m_shader;
	TexturePtr m_fallbackCubemap;
	TexturePtr m_fallbackBRDF;
	rhi::TextureHandle m_ssaoTexture;
	rhi::TextureHandle m_sssTexture;
	std::array<rhi::TextureHandle, MaxIndirectDiffuseSources> m_indirectDiffuseTextures{};
	std::array<float, MaxIndirectDiffuseSources> m_indirectDiffuseStrengths{};
	int m_indirectDiffuseSourceCount = 0;
	OutputMode m_outputMode = OutputMode::FullLighting;

	// Cached uniform locations to avoid per-frame GetUniformLocation calls
	struct UniformLocations {
		// Sampler uniforms
		rhi::UniformLocation gPackedNormalRM = -1;
		rhi::UniformLocation gAlbedoAO = -1;
		rhi::UniformLocation gSpecularF0 = -1;  // Changed from gEmissiveSpec
		rhi::UniformLocation gMaterialID = -1;
		rhi::UniformLocation gEmissive = -1;    // Separate emissive texture
		rhi::UniformLocation gClearCoat = -1;
		rhi::UniformLocation gPrincipledParams = -1;
		rhi::UniformLocation gDepth = -1;
		rhi::UniformLocation ssaoMap = -1;
		rhi::UniformLocation screenSpaceShadowMap = -1;
		rhi::UniformLocation indirectDiffuseMaps = -1;
		rhi::UniformLocation irradianceMap = -1;
		rhi::UniformLocation prefilteredMap = -1;
		rhi::UniformLocation brdfLUT = -1;
		rhi::UniformLocation multiLightShadowArray = -1;

		// Matrix uniforms
		rhi::UniformLocation invProjection = -1;
		rhi::UniformLocation invView = -1;
		rhi::UniformLocation view = -1;
		rhi::UniformLocation viewPos = -1;

		// IBL uniforms
		rhi::UniformLocation prefilteredMaxLOD = -1;
		rhi::UniformLocation iblIntensity = -1;
		rhi::UniformLocation diffuseIBLScale = -1;
		rhi::UniformLocation specularIBLScale = -1;

		// Effect strength uniforms
		rhi::UniformLocation aoStrength = -1;
		rhi::UniformLocation sssStrength = -1;
		rhi::UniformLocation indirectDiffuseStrengths = -1;
		rhi::UniformLocation indirectDiffuseSourceCount = -1;
		rhi::UniformLocation indirectDiffuseCompositeMode = -1;
		rhi::UniformLocation lightingOutputMode = -1;
		rhi::UniformLocation lightingCompositeDebugMode = -1;

		// Light uniforms
		rhi::UniformLocation numLights = -1;
		rhi::UniformLocation numDirectionalLights = -1;
		rhi::UniformLocation numPointLights = -1;
		rhi::UniformLocation numSpotLights = -1;
		rhi::UniformLocation enableShadows = -1;
		rhi::UniformLocation shadowBias = -1;
		rhi::UniformLocation cascadeCount = -1;
		
		// Cascade blend settings
		rhi::UniformLocation cascadeBlendDistance = -1;
		rhi::UniformLocation cascadeBlendFactor = -1;
		rhi::UniformLocation cascadeSplits = -1;
		rhi::UniformLocation shadowDebugVisualization = -1;
		
		// Point light shadow settings
		
		// Shadow darkness settings
		rhi::UniformLocation shadowDarkness = -1;
		rhi::UniformLocation shadowMinBrightness = -1;
		rhi::UniformLocation shadowTransitionHardness = -1;
	} m_uniforms;

	bool m_uniformsCached = false;
};
