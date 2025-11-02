#pragma once
#include <string>
#include <memory>
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Framebuffer.h"
#include "TextureUnits.h"
#include "Texture.h"  // Use new refactored Texture class

//Skybox class for loading HDR images, converting to cubemap, and rendering as background.

class Skybox
{
public:
	Skybox() = default;
	~Skybox() = default; 

	// Initialize from HDR and build full IBL pipeline (strict). Returns false on failure.
	bool Init(const std::string& hdrPath,
		const std::string& equirectVertShader,
		const std::string& equirectFragShader,
		const std::string& skyboxVertShader,
		const std::string& skyboxFragShader,
		int windowWidth,
		int windowHeight);

	void Draw(const glm::mat4& view, const glm::mat4& projection);

	// Cleanup all OpenGL resources.
	void Cleanup();

	// Retrieve the environment cubemap texture ID so other code (e.g., PBR shaders)
	GLuint GetEnvironmentMap() const { return m_envCubemap ? m_envCubemap->ID() : 0; }
	
	// IBL Resources for physically based lighting
	GLuint GetIrradianceMap() const { return m_irradianceMap ? m_irradianceMap->ID() : 0; }
	GLuint GetPrefilteredMap() const { return m_prefilteredMap ? m_prefilteredMap->ID() : 0; }
	GLuint GetBRDFLUT() const { return m_brdfLUT ? m_brdfLUT->ID() : 0; }
	float GetPrefilteredMaxLOD() const { return m_prefilteredMaxLOD; }

	// IBL intensity controls to prevent over-bright lighting
	void SetIBLIntensity(float intensity) { m_iblIntensity = intensity; }
	float GetIBLIntensity() const { return m_iblIntensity; }
	
	void SetSkyboxExposure(float exposure) { m_skyboxExposure = exposure; }
	float GetSkyboxExposure() const { return m_skyboxExposure; }
	
	// Separate control for diffuse vs specular IBL contribution
	void SetDiffuseIBLScale(float scale) { m_diffuseIBLScale = scale; }
	float GetDiffuseIBLScale() const { return m_diffuseIBLScale; }
	
	void SetSpecularIBLScale(float scale) { m_specularIBLScale = scale; }
	float GetSpecularIBLScale() const { return m_specularIBLScale; }

	// Validation methods for IBL resources (heavy validation)
	bool ValidateIBLTextures() const; // legacy detailed validation
	void LogTextureInfo() const;
	
	// Comprehensive pipeline verification (lightweight)
	bool VerifyIBLPipelineComplete() const;
	void ForceRegenerateIBL();

	// Fast readiness query used by renderer
	bool IsReady() const { return m_pipelineReady; }
	// Ensure ready (attempt regeneration once if not). Returns true if ready after call.
	bool EnsureReady();

protected:
	// Loads the HDR file into a floating-point 2D texture.
	TexturePtr loadHDRTexture(const std::string& hdrPath);

	// Builds the cube geometry (VAO/VBO) for rendering the skybox.
	void buildSkyboxCube();

	// Helper to render the cube (binds VAO and issues draw call).
	void renderCube();
	
	// IBL precomputation methods
	bool GenerateIBLResources();
	bool GenerateIrradianceMap();
	bool GeneratePrefilteredMap();
	bool GenerateBRDFLUT();

	// Internal capture helpers
	void CreateCaptureFBO(int width, int height);
	void ResizeCaptureRBO(int width, int height);

private:
	// Cubemap holding the environment map (using new Texture class)
	TexturePtr m_envCubemap;

	// VAO/VBO for drawing a unit cube.
	GLuint m_skyboxVAO = 0;
	GLuint m_skyboxVBO = 0;

	// Shader programs.
	GLuint m_equiRectToCubeShader = 0;  // equirectangular-to-cubemap converter
	GLuint m_skyboxShader = 0;      // shader for final skybox rendering

	// Framebuffer for rendering the skybox (legacy, still kept but not used for IBL capture now).
	std::unique_ptr<FrameBuffer> m_captureBuffer;
	
	// Dedicated FBO/RBO for IBL capture to avoid abstraction side-effects
	GLuint m_captureFBO = 0;
	GLuint m_captureRBO = 0;

	// IBL Resources (using new Texture class)
	TexturePtr m_irradianceMap;         // Diffuse irradiance cubemap (32x32)
	TexturePtr m_prefilteredMap;// Prefiltered environment map with mipmaps (128x128)
	TexturePtr m_brdfLUT;               // BRDF integration lookup table (512x512)
	float m_prefilteredMaxLOD = 4.0f;   // Maximum mip level for prefiltered map
	
	// IBL shader programs
	GLuint m_irradianceShader = 0;      // Convolution shader for irradiance
	GLuint m_prefilterShader = 0;  // Prefilter shader for specular
	GLuint m_brdfShader = 0;  // BRDF integration shader

	// IBL intensity controls to prevent over-bright results
	float m_iblIntensity = 0.4f;  // Overall IBL multiplier (reduced from 1.0 to 0.4)
	float m_skyboxExposure = 1.0f;      // Exposure for skybox rendering only
	float m_diffuseIBLScale = 0.5f;     // Scale for diffuse irradiance contribution
	float m_specularIBLScale = 0.6f;    // Scale for specular prefiltered contribution

	// Pipeline readiness flag (set true only after successful full verification)
	bool m_pipelineReady = false;
	// Guard to prevent infinite regeneration attempts within a frame
	mutable bool m_regenAttempted = false;
};
