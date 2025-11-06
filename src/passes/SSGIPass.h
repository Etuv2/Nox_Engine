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
 *
 * ALGORITHM OVERVIEW:
 * 1. Stochastic Direction Generation (ssgi_directions_comp.glsl)
 *    - Uses R2 low-discrepancy sequence with Cranley-Patterson rotation
 *    - Combines spatial IGN (Interleaved Gradient Noise) for blue-noise-like distribution
 *    - Provides excellent temporal convergence while avoiding aliasing
 *
 * 2. Screen-Space Ray Marching (ssgi_raymarch_comp.glsl)
 *    - Malley's method for proper cosine-weighted hemisphere sampling
 *    - Adaptive step sizing based on distance and ray direction
 *    - Binary search refinement (6 iterations) for sub-pixel accuracy
 *    - Near-miss fallback to improve hit rate and reduce noise
 *    - Energy-conserving irradiance calculation
 *
 * 3. Spatial Denoising (bilateral blur + upsample)
 *    - Quarter-resolution bilateral blur with adaptive kernel sizing
 *    - Edge-aware filtering using depth and normal discontinuities
 *    - Bilateral upsample to working resolution
 *    - Outlier rejection for robust noise removal
 *
 * 4. Temporal Accumulation (ssgi_temporal_resolve_comp.glsl)
 *    - Exponential moving average with motion-vector reprojection
 *- YCoCg color space for superior variance estimation
 *    - Multi-factor adaptive blending (confidence, variance, luminance change)
 *    - Neighborhood clamping to prevent ghosting
 *    - Enhanced rejection heuristics (depth, normal, motion, edge distance)
 *
 * 5. Final Smoothing (Kawase blur - optional)
 *    - Multi-pass Kawase blur for very smooth results
 *    - Applied after temporal accumulation for stability
 *
 * KEY FEATURES:
 * - Noise-free results with as little as 1 sample per pixel (SPP) per frame
 * - Excellent temporal stability through low-discrepancy sampling
 * - Proper energy conservation following rendering equation
 * - Resolution-independent quality (half-res by default for performance)
 * - Compatible with TAA (Temporal Anti-Aliasing) for further convergence
 *
 * PERFORMANCE:
 * - Half-resolution: ~2-4ms on mid-range GPUs (1080p)
 * - Full-resolution: ~5-8ms on mid-range GPUs (1080p)
 * - Scales well with resolution due to compute shader optimization
 *
 * INTEGRATION:
 * - Outputs indirect diffuse irradiance (NOT radiance)
 * - Lighting pass multiplies by albedo for correct BRDF evaluation
 * - Works alongside other GI techniques (LPV, IBL) additively
 *
 * REFERENCES:
 * - Shubham Sachdeva, "Dynamic, Noise-Free SSGI", SEED/EA (2018)
 * - Jorge Jimenez, "Practical Real-Time Strategies for Accurate Indirect Occlusion", SIGGRAPH 2016
 * - Jimenez et al., "Filtering Approaches for Real-Time Anti-Aliasing", SIGGRAPH 2011 (temporal filtering)
 *
 * Refactored to use the new Texture class for improved resource management.
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
	// === COMPUTE SHADER PIPELINE STAGES ===

	// Stage 1: Generate stochastic hemisphere directions (R2 + IGN + Cranley-Patterson)
	std::unique_ptr<ComputeShader> m_csDirections;

	// Stage 2: Screen-space ray marching with adaptive stepping and binary search refinement
	std::unique_ptr<ComputeShader> m_csRaymarch;

	// Stage 2.5: Downsample to quarter-res for efficient denoising
	std::unique_ptr<ComputeShader> m_csDownsample;

	// Stage 3: Bilateral blur at quarter-res (edge-aware spatial denoising)
	std::unique_ptr<ComputeShader> m_csBilateral;

	// Stage 3.5: Bilateral upsample back to working resolution
	std::unique_ptr<ComputeShader> m_csUpsample;

	// Stage 4: Temporal accumulation with variance clamping (YCoCg color space)
	std::unique_ptr<ComputeShader> m_csTemporal;

	// Stage 5: CRITICAL FIX - Final upsample to FULL resolution for lighting pass
	std::unique_ptr<ComputeShader> m_csFinalUpsample;

	// === TEXTURE RESOURCES (using new Texture system) ===

	// Working resolution (m_hw x m_hh)
	TexturePtr m_dirTex;    // Stochastic random values for direction generation (RG16F)
	TexturePtr m_ssgiRaw;   // Raw ray march output before denoising (RGBA16F)
	TexturePtr m_ssgiBlur;          // Denoised/upsampled result (RGBA16F)
	TexturePtr m_ssgiWork;  // Working-res temporal result (RGBA16F)
	
	// CRITICAL FIX: Full resolution final output (m_w x m_h)
	TexturePtr m_ssgiTex;   // FULL-RES final result for lighting pass (RGBA16F)

	// Quarter resolution (m_qw x m_qh) - for efficient spatial denoising
	TexturePtr m_ssgiQuarter;  // Downsampled raw output (RGBA16F)
	TexturePtr m_ssgiQuarterBlur;   // Quarter-res blurred (RGBA16F)

	// Temporal history textures (working resolution)
	TexturePtr m_historyColor;      // Previous frame HDR color (for sampling ray hits)
	TexturePtr m_historySSGI;// Previous frame SSGI (for temporal accumulation)

	// === KAWASE BLUR RESOURCES (optional final smoothing) ===
	GLuint m_kawaseShader = 0;
	GLuint m_kawaseFBO = 0;
	TexturePtr m_kawasePing;    // Kawase ping buffer
	TexturePtr m_kawasePong;   // Kawase pong buffer
	int m_kawasePasses = 3;         // Number of Kawase blur iterations

	// === RESOLUTION TRACKING ===
	int m_w = 0, m_h = 0;        // Full resolution (G-buffer resolution)
	int m_hw = 0, m_hh = 0;    // Working resolution (half or full based on ssgiHalfRes)
	int m_qw = 0, m_qh = 0;      // Quarter resolution (for efficient denoising)
	bool m_halfRes = true; // Use half resolution for performance (default: true)

	int m_frameIndex = 0;           // Frame counter for stochastic sampling (wraps at 8192)

	// === PIPELINE STAGE IMPLEMENTATIONS ===

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
*
	 * Simple 4-tap box filter to reduce resolution by 2x.
	 * Quarter-res denoising provides good quality with much better performance.
	 */
	void runDownsample(RenderContext& ctx);

	/**
	 * @brief Edge-aware bilateral blur at quarter resolution
	 *
	 * Implements spatial denoising with:
	 * - Adaptive kernel size based on local variance
	 * - Depth-based edge detection
	 * - Normal-based edge detection
	 * - Color-based outlier rejection
	 *
	 * Preserves geometric edges while aggressively removing noise.
*/
	void runBilateral(RenderContext& ctx);

	/**
* @brief Bilateral upsample from quarter to working resolution
	 *
	 * Upsamples blurred quarter-res result while:
	 * - Using depth and normals for edge-aware filtering
	 * - Blending with high-res guidance to preserve detail
	 *
	 * Avoids edge bleeding during upsampling.
	 */
	void runUpsample(RenderContext& ctx);

	/**
  * @brief Temporal accumulation with motion-vector reprojection
	 *
	 * Implements exponential moving average with:
	 * - YCoCg color space for better variance estimation
	 * - Multi-factor adaptive blending (confidence, variance, luma change)
	 * - Variance-based neighborhood clamping (prevents ghosting)
	 * - Enhanced rejection heuristics (depth, normal, motion, edge distance)
 *
	 * CRITICAL: History buffer updated AFTER temporal resolve completes.
 * This ensures next frame reads the converged result, not the pre-blend state.
	 */
	void runTemporal(RenderContext& ctx);

	/**
	 * @brief Optional Kawase blur for final smoothing
	 *
	 * Multi-pass separable blur applied after temporal accumulation.
	 * Provides very smooth results but adds overhead.
   * Consider disabling if temporal accumulation quality is sufficient.
	 */
	void runKawase(RenderContext& ctx);

	/**
	 * @brief CRITICAL FIX: Final upsample from working resolution to full resolution
	 *
	 * This is the final stage that produces the full-resolution SSGI texture
	 * that the lighting pass expects. Uses bilateral upsampling with depth/normal guidance.
	 *
	 * Input: m_ssgiWork (working-res, e.g. 960x540)
	 * Output: m_ssgiTex (full-res, e.g. 1920x1080)
	 */
	void runFinalUpsample(RenderContext& ctx);
};
