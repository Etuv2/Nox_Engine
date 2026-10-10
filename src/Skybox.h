#pragma once
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Framebuffer.h"
#include "TextureUnits.h"
#include "Texture.h"  // Use new refactored Texture class
#include "GpuBuffer.h"

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

	// Get the source HDR path for serialization
	const std::string& GetHDRPath() const { return m_hdrPath; }

	// Retrieve the environment cubemap texture ID so other code (e.g., PBR shaders)
	rhi::TextureHandle GetEnvironmentMap() const { return m_envCubemap ? m_envCubemap->ID() : rhi::TextureHandle{}; }
	
	// IBL Resources for physically based lighting
	rhi::TextureHandle GetIrradianceMap() const { return m_irradianceMap ? m_irradianceMap->ID() : rhi::TextureHandle{}; }
	rhi::TextureHandle GetPrefilteredMap() const { return m_prefilteredMap ? m_prefilteredMap->ID() : rhi::TextureHandle{}; }
	rhi::TextureHandle GetBRDFLUT() const { return m_brdfLUT ? m_brdfLUT->ID() : rhi::TextureHandle{}; }
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
	rhi::VertexInputHandle m_skyboxVAO;
	GpuBufferPtr m_skyboxVBO;  // Use GpuBuffer wrapper

	// Shader programs.
	rhi::ProgramHandle m_equiRectToCubeShader;  // equirectangular-to-cubemap converter
	rhi::ProgramHandle m_skyboxShader;      // shader for final skybox rendering
	
	// Dedicated FBO/RBO for IBL capture to avoid abstraction
	rhi::FramebufferHandle m_captureFBO;
	rhi::TextureHandle m_captureRBO;

	// IBL Resources (using new Texture class)
	TexturePtr m_irradianceMap;         // Diffuse irradiance cubemap (32x32)
	TexturePtr m_prefilteredMap;// Prefiltered environment map with mipmaps (128x128)
	TexturePtr m_brdfLUT;               // BRDF integration lookup table (512x512)
	float m_prefilteredMaxLOD = 4.0f;   // Maximum mip level for prefiltered map
	
	// IBL shader programs
	rhi::ProgramHandle m_irradianceShader;      // Convolution shader for irradiance
	rhi::ProgramHandle m_prefilterShader;  // Prefilter shader for specular
	rhi::ProgramHandle m_brdfShader;  // BRDF integration shader

	// IBL intensity controls to prevent over-bright results
	float m_iblIntensity = 0.1f;  // Overall IBL multiplier
	float m_skyboxExposure = 1.0f;      // Exposure for skybox rendering only
	float m_diffuseIBLScale = 0.3f;     // Scale for diffuse irradiance contribution
	float m_specularIBLScale = 0.45f;    // Scale for specular prefiltered contribution

	// Pipeline readiness flag (set true only after successful full verification)
	bool m_pipelineReady = false;
	// Guard to prevent infinite regeneration attempts within a frame
	mutable bool m_regenAttempted = false;
	
	// Source HDR path for serialization
	std::string m_hdrPath;
};
