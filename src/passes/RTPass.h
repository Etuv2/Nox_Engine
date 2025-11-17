#pragma once
#include "../RenderPass.h"
#include "../Texture.h"
#include <memory>
#include <GL/glew.h>

// Forward declarations
class FrameBuffer;
struct RenderContext;
class ComputeShader;

/**
 * @brief RTX Pass using compute shaders
 * IMPLEMENTATION: Bidirectional Path Tracing with importance sampling
 * Makes use of Bounding Volume Hierarchy (BVH) for ray-scene intersection acceleration
 * REFERENCES:
 * - www.pbr-book.org. (n.d.). Bounding Volume Hierarchies: https://www.pbr-book.org/3ed-2018/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies
 * - ‌Pbr-book.org. (2025). Bidirectional Path Tracing. : https://www.pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Bidirectional_Path_Tracing
 */
class RTPass : public RenderPass {
public:
	RTPass() = default;
	~RTPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;


private:

	// Compute shader program for ray tracing
	std::unique_ptr<ComputeShader> m_rtShader;

	// Framebuffer for ray traced output
	GLuint m_rtFBO_ID = 0;  // Manual FBO for blitting

	// Ray traced output texture
	TexturePtr m_rtTexture;
	
	// Accumulation textures for temporal convergence
	TexturePtr m_accumulationTexture;  // Accumulated radiance
	TexturePtr m_varianceTexture;       // Variance for adaptive sampling
	int m_frameIndex = 0;   // Current accumulation frame

	// Resolution
	int m_w = 0, m_h = 0;        // Full resolution (G-buffer resolution)
	int m_hw = 0, m_hh = 0;    // Working resolution (half or full based on ssgiHalfRes)
	int m_qw = 0, m_qh = 0;      // Quarter resolution (for efficient denoising)
	float m_renderResolutionScale = 1.0f; // Scale factor for render resolution

	//flag to indicate if BVH needs to be rebuilt
	bool m_bvhDirty = true;
	
	// BVH acceleration structure
	struct BVHBuffers {
		GLuint triangleSSBO = 0;  // Triangle data SSBO
		GLuint bvhSSBO = 0;       // BVH node data SSBO
		size_t triangleCount = 0;
		size_t nodeCount = 0;
	} m_bvhBuffers;

	//Light data for ray tracing
	struct LightBuffers {
		GLuint lightSSBO = 0;       // Light data SSBO (from LightManager)
		size_t lightCount = 0;
	} m_lightBuffers;
	
	// SVGF Denoising buffers
	struct SVGFBuffers {
		GLuint momentsSSBO = 0;           // Temporal variance accumulation (mean + variance)
		GLuint historyLengthSSBO = 0;     // Per-pixel history length for variance calculation
		size_t pixelCount = 0;
	} m_svgfBuffers;

	// SVGF intermediate textures
	TexturePtr m_prevRadianceTexture;    // Previous frame radiance for temporal reprojection
	TexturePtr m_momentsTexture;     // Mean and variance (RG32F)
	TexturePtr m_historyLengthTexture; // History length per pixel (R16F)
	TexturePtr m_denoisedTexture;          // Final denoised output
	
	// SVGF compute shaders
	std::unique_ptr<ComputeShader> m_svgfTemporalShader;      // Temporal reprojection
	std::unique_ptr<ComputeShader> m_svgfVarianceShader;      // Variance estimation
	std::unique_ptr<ComputeShader> m_svgfAtrousShader;        // À-trous wavelet filter

	/*
	* Warmup stage where BVH is built and shaders are prepped
	*/
	void runWarmup(const std::shared_ptr<SceneGraph>& sceneGraph);
	
	/*
	* Build and upload BVH to GPU
	*/
	void buildAndUploadBVH(const std::shared_ptr<SceneGraph>& sceneGraph);

	/*
	* Ray tracing execution stage
	*/
	void runRayTracing(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<Skybox>& skybox);

	/*
	* Accumulate frame results for progressive refinement
	*/
	void accumulateFrame(RenderContext& ctx);
	
	/*
	* Reset accumulation (when camera moves)
	*/
	void resetAccumulation();
	
	/*
	* SVGF Denoising Pipeline
	*/
	void runSVGFTemporal(RenderContext& ctx, const std::shared_ptr<Camera>& camera);
	void runSVGFVariance(RenderContext& ctx);
	void runSVGFAtrous(RenderContext& ctx, int iteration);
	
	/*
	* Copy ray traced output to HDR buffer for post-processing
	*/
	void copyToHDRBuffer(RenderContext& ctx);
	
	/*
	* Get the output texture for display
	*/
	GLuint GetOutputTexture() const;


};
