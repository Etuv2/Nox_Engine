#pragma once
#include "../RenderPass.h"
#include "../Texture.h"
#include <GL/glew.h>
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
	void SetSSAOTexture(GLuint ssaoTex) { m_ssaoTexture = ssaoTex; }

	//Allow ScreenSpaceShadowPass to provide its output texture
	void SetScreenSpaceShadowTexture(GLuint sssTex) { m_sssTexture = sssTex; }

	void ClearIndirectDiffuseSources();
	void SetIndirectDiffuseSource(int index, GLuint indirectDiffuseTex, float strength = 1.0f);
	void SetOutputMode(OutputMode mode) { m_outputMode = mode; }

	// Enable/disable verbose logging (disabled by default for performance)
	static constexpr bool VerboseLogging = false;
	static constexpr int MaxIndirectDiffuseSources = 4;

private:
	void SetupFallbackIBL();
	void CacheUniformLocations();
	void UploadStaticSamplerUniforms();

	GLuint m_shader = 0;
	TexturePtr m_fallbackCubemap;
	TexturePtr m_fallbackBRDF;
	GLuint m_ssaoTexture = 0;
	GLuint m_sssTexture = 0;
	std::array<GLuint, MaxIndirectDiffuseSources> m_indirectDiffuseTextures{};
	std::array<float, MaxIndirectDiffuseSources> m_indirectDiffuseStrengths{};
	int m_indirectDiffuseSourceCount = 0;
	OutputMode m_outputMode = OutputMode::FullLighting;

	// Cached uniform locations to avoid per-frame glGetUniformLocation calls
	struct UniformLocations {
		// Sampler uniforms
		GLint gPackedNormalRM = -1;
		GLint gAlbedoAO = -1;
		GLint gSpecularF0 = -1;  // Changed from gEmissiveSpec
		GLint gMaterialID = -1;
		GLint gEmissive = -1;    // Separate emissive texture
		GLint gClearCoat = -1;
		GLint gPrincipledParams = -1;
		GLint gDepth = -1;
		GLint ssaoMap = -1;
		GLint screenSpaceShadowMap = -1;
		GLint indirectDiffuseMaps = -1;
		GLint irradianceMap = -1;
		GLint prefilteredMap = -1;
		GLint brdfLUT = -1;
		GLint multiLightShadowArray = -1;

		// Matrix uniforms
		GLint invProjection = -1;
		GLint invView = -1;
		GLint view = -1;
		GLint viewPos = -1;

		// IBL uniforms
		GLint prefilteredMaxLOD = -1;
		GLint iblIntensity = -1;
		GLint diffuseIBLScale = -1;
		GLint specularIBLScale = -1;

		// Effect strength uniforms
		GLint aoStrength = -1;
		GLint sssStrength = -1;
		GLint indirectDiffuseStrengths = -1;
		GLint indirectDiffuseSourceCount = -1;
		GLint indirectDiffuseCompositeMode = -1;
		GLint lightingOutputMode = -1;
		GLint lightingCompositeDebugMode = -1;

		// Light uniforms
		GLint numLights = -1;
		GLint numDirectionalLights = -1;
		GLint numPointLights = -1;
		GLint numSpotLights = -1;
		GLint enableShadows = -1;
		GLint shadowBias = -1;
		GLint maxShadowBias = -1;
		GLint normalOffsetScale = -1;
		GLint cascadeBiasScale = -1;
		GLint cascadeCount = -1;
		
		// Cascade blend settings
		GLint cascadeBlendDistance = -1;
		GLint cascadeBlendFactor = -1;
		GLint cascadeSplits = -1;
		GLint shadowDebugVisualization = -1;
		
		// Point light shadow settings
		GLint pointLightBias = -1;
		GLint pointLightSlopeBias = -1;
		GLint pointLightNormalOffset = -1;
		
		// Shadow darkness settings
		GLint shadowDarkness = -1;
		GLint shadowMinBrightness = -1;
		GLint shadowTransitionHardness = -1;
	} m_uniforms;

	bool m_uniformsCached = false;
};
