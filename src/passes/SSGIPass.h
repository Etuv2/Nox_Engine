#pragma once

#include "../RenderPass.h"
#include "../Texture.h"  // Include new Texture system
#include <memory>

class ComputeShader;
class RenderContext;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;

/**
 * @brief Screen Space Global Illumination Pass using compute shaders
 *
 * IMPLEMENTATION: Follows Shubham Sachdeva's "Dynamic, Noise-Free Screen Space Diffuse Global Illumination" approach
 * (https://www.ea.com/seed/news/seed-dd18-presentation-slides-raytracing)
 * REFERENCES:
 * - Shubham Sachdeva, "Dynamic, Noise-Free SSGI", SEED/EA (2018)
 * - Jorge Jimenez, "Practical Real-Time Strategies for Accurate Indirect Occlusion", SIGGRAPH 2016
 * - Jimenez et al., "Filtering Approaches for Real-Time Anti-Aliasing", SIGGRAPH 2011 (temporal filtering)
 */
class SSGIPass : public RenderPass {
public:
	SSGIPass() = default;
	~SSGIPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	// Capture current HDR frame for ray marching history
	void CaptureHistory(RenderContext& ctx);

	// Get final SSGI texture (indirect diffuse irradiance)
	GLuint GetSSGITexture() const { return m_ssgiTex ? m_ssgiTex->ID() : 0; }

private:
	// Compute shader programs for each stage
	std::unique_ptr<ComputeShader> m_csDirections;
	std::unique_ptr<ComputeShader> m_csRaymarch;
	std::unique_ptr<ComputeShader> m_csDownsample;
	std::unique_ptr<ComputeShader> m_csBilateral;
	std::unique_ptr<ComputeShader> m_csUpsample;
	std::unique_ptr<ComputeShader> m_csTemporal;
	std::unique_ptr<ComputeShader> m_csFinalUpsample;


	// Working resolution (m_hw x m_hh)
	TexturePtr m_dirTex;    // Stochastic random values for direction generation (RG16F)
	TexturePtr m_ssgiRaw;   // Raw ray march output before denoising (RGBA16F)
	TexturePtr m_ssgiBlur;          // Denoised/upsampled result (RGBA16F)
	TexturePtr m_ssgiWork;  // Working-res temporal result (RGBA16F)

	TexturePtr m_ssgiTex;   // FULL-RES final result for lighting pass (RGBA16F)

	// Quarter resolution (m_qw x m_qh) - for efficient spatial denoising
	TexturePtr m_ssgiQuarter;  // Downsampled raw output (RGBA16F)
	TexturePtr m_ssgiQuarterBlur;   // Quarter-res blurred (RGBA16F)

	// Temporal history textures (working resolution)
	TexturePtr m_historyColor;      // Previous frame HDR color (for sampling ray hits)
	TexturePtr m_historySSGI;// Previous frame SSGI (for temporal accumulation)

	// kawase blur resources
	GLuint m_kawaseShader = 0;
	GLuint m_kawaseFBO = 0;
	TexturePtr m_kawasePing;    // Kawase ping buffer
	TexturePtr m_kawasePong;   // Kawase pong buffer
	int m_kawasePasses = 3;         // Number of Kawase blur iterations

	int m_w = 0, m_h = 0;        // Full resolution (G-buffer resolution)
	int m_hw = 0, m_hh = 0;    // Working resolution (half or full based on ssgiHalfRes)
	int m_qw = 0, m_qh = 0;      // Quarter resolution (for efficient denoising)
	bool m_halfRes = true; // Use half resolution for performance (default: true)

	int m_frameIndex = 0;           // Frame counter for stochastic sampling (wraps at 8192)


	/**
	 * @brief Generate stochastic hemisphere directions using R2 + IGN + Cranley-Patterson
	 *
	 * Outputs 2D random values to m_dirTex that will be used by raymarch stage
	 * to generate cosine-weighted hemisphere samples via Malley's method.
 */
	void runDirections(RenderContext& ctx);

	/**
	 * @brief Perform screen-space ray marching to find indirect lighting
	 *
  * For each pixel:
	 * 1. Reconstruct world position and normal from G-buffer
	 * 2. Generate cosine-weighted hemisphere direction from random values
 * 3. March ray through screen-space depth buffer with adaptive stepping
	 * 4. Refine hit with binary search for sub-pixel accuracy
	 * 5. Sample previous frame color at hit location for indirect irradiance
	 * 6. Apply geometric attenuation (distance + cosine falloff)
	 *
	 * Outputs raw noisy indirect irradiance to m_ssgiRaw.
   */
	void runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox);

	/**
	 * @brief Downsample to quarter resolution for efficient spatial denoising
	 */
	void runDownsample(RenderContext& ctx);

	/**
	 * @brief Edge-aware bilateral blur at quarter resolution
	 * Preserves geometric edges while aggressively removing noise.
*/
	void runBilateral(RenderContext& ctx);

	/**
* @brief Bilateral upsample from quarter to working resolution
	 */
	void runUpsample(RenderContext& ctx);

	/**
  * @brief Temporal accumulation with motion-vector reprojection
	 */
	void runTemporal(RenderContext& ctx);

	/**
	 * @brief Optional Kawase blur for final smoothing
	 */
	void runKawase(RenderContext& ctx);

	/**
	 * @brief Final upsample from working resolution to full resolution
	 *
	 * This is the final stage that produces the full-resolution SSGI texture
	 * that the lighting pass expects. Uses bilateral upsampling with depth/normal guidance.
	 *
	 * Input: m_ssgiWork (working-res, e.g. 960x540)
	 * Output: m_ssgiTex (full-res, e.g. 1920x1080)
	 */
	void runFinalUpsample(RenderContext& ctx);
};
