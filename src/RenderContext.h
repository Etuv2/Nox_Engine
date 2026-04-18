#pragma once
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <GL/glew.h>


class FrameBuffer; class ScreenQuad; class LightManager;
/*
* @struct RenderContext
* @brief Centralized rendering context holding shared resources and settings.
* This struct encapsulates common framebuffers, screen quad, light manager,
* and various rendering settings used across multiple render passes.
* It provides a convenient way to manage and access these resources
* throughout the rendering pipeline.
*/

struct RenderContext {
	// Dimensions
	int width = 1920;
	int height = 1080;

	// Timing
	float deltaTime = 0.0f;  // Frame delta time in seconds


	// Shared FBOs
	std::unique_ptr<FrameBuffer> gbufferFBO; // Extended G-Buffer
	std::unique_ptr<FrameBuffer> hdrFBO; // HDR target
	FrameBuffer* taaFBO = nullptr; // TAA output (for SSGI history) - raw pointer to avoid ownership


	// Screen quad
	std::unique_ptr<ScreenQuad> screenQuad;


	// Light system
	//std::shared_ptr<LightManager> lightManager;
	std::shared_ptr<LightManager> lightManager;


	// Matrices/state filled each frame
	glm::mat4 view{ 1.0f };
	glm::mat4 proj{ 1.0f };

	// Previous frame matrices for TAA
	glm::mat4 prevView{ 1.0f };
	glm::mat4 prevProj{ 1.0f };

	// Renderer mode toggle
	enum class RendererMode {
		DEFERRED_REALTIME = 0,     // Standard deferred rendering with real-time effects
		PATH_TRACED = 1          // Path-traced mode using BVH for high-quality offline rendering
	};
	RendererMode rendererMode = RendererMode::DEFERRED_REALTIME;

	// Path tracing settings
	bool enablePathTracing = false;
	int rtSamplesPerPixel = 4;      // Samples per pixel per frame
	int rtMaxBounces = 4;  // Maximum ray bounces
	float rtResolutionScale = 1.0f;  // Resolution scale for path tracing (0.5 = half res)
	bool rtAccumulate = true;        // Enable temporal accumulation
	bool rtDenoise = true;     // Enable SVGF denoising

	//Advanced path tracing settings
	bool rtEnableNEE = true;     // Next Event Estimation (direct lighting)
	bool rtEnableMIS = true;  // Multiple Importance Sampling

	// SVGF Denoising settings
	float svgfTemporalAlpha = 0.1f;       // Temporal blend factor (lower = more stable)
	float svgfVarianceClipGamma = 1.2f;   // Variance clipping gamma (lower = less ghosting)
	float svgfDepthThreshold = 0.02f;     // Depth similarity threshold (tighter for edges)
	float svgfNormalThreshold = 0.95f;    // Normal similarity threshold (cos angle, higher = sharper)
	int svgfAtrousIterations = 1;         // Number of à-trous filter iterations
	float svgfPhiColor = 10.0f;            // Color weight parameter (lower = sharper color edges)
	float svgfPhiNormal = 64.0f;          // Normal weight parameter (higher = sharper geometric edges)
	float svgfPhiDepth = 0.005f;          // Depth weight parameter (lower = sharper depth edges)

	//BVH Debug Visualization
	bool rtDisplayBVH = false;     // Enable BVH visualization mode
	bool rtDisplayMultipleBVHLayers = false;  // Show multiple BVH layers
	int rtBVHLayerToDisplay = 0;     // Which BVH layer to show
	int rtHeatmapColorLimit = 50;   // Max value for heatmap color scale

	//IBL Environment settings
	bool rtEnableIBL = true;         // Enable IBL environment sampling
	float rtIBLIntensity = 1.0f;  // IBL intensity multiplier


	// SSAO settings
	bool enableSSAO = true;
	bool ssaoHalfRes = true;
	float ssaoResolutionScale = 0.5f;
	float ssaoRadius = 0.75f;   //Keeps AO localized to corners and crevices (was 0.5)
	float ssaoBias = 0.02f;          //Tighter bias for better contact (was 0.025)
	float ssaoIntensity = 0.5f;  //Subtle darkening, not overpowering (was 1.0)
	float ssaoBlurDepthThreshold = 0.015f; // INCREASED: Better edge preservation (was 0.01)
	float ssaoTemporalAlpha = 0.12f;


	// SSGI settings
	bool enableSSGI = true;
	float ssgiStrength = 1.35f;         // Overall GI contribution multiplier
	float ssgiRadius = 4.5f;            // Ray length in view-space units
	int ssgiSampleCount = 256;
	bool ssgiHalfRes = true;  // Run SSGI at half-res for performance (recommended)
	float ssgiWorkingResolutionScale = 0.5f;
	float ssgiTraceResolutionScale = 0.25f;
	float ssgiTemporalAlpha = 0.055f;    // Balanced temporal responsiveness for visible GI
	float ssgiNormalReject = 0.10f;      // Tighter normal rejection for edge stability
	float ssgiDepthReject = 0.1f;         // Bilateral depth sigma in view-space units
	float ssgiThickness = 0.01f;   //Ray-surface intersection thickness 
	bool ssgiEnableSpatialDenoise = true;
	float ssgiTemporalResponse = 0.2f;    // Extra response control for history confidence scaling
	float ssgiUpscaleSharpness = 1.8f;    // Depth-aware upscale edge sharpness
	int ssgiSectorCount = 16;             // Visibility bitmask sectors per slice
	int ssgiDebugMode = 0;                // 0=final, 1..8 stage debug


	// Bloom settings
	bool enableBloom = true;
	float bloomThreshold = 1.0f;
	float bloomKnee = 0.5f;
	float bloomStrength = 0.8f;
	float bloomRadius = 0.5f;


	// TAA settings
	bool enableTAA = true;
	float taaBlendFactor = 0.15f;
	float taaVarianceThreshold = 0.8f;
	float taaLumaWeight = 0.2f;
	bool taaUseYCoCg = false;
	int taaJitterPattern = 0;
	float taaDepthThreshold = 0.002f;
	float taaNormalThreshold = 0.15f;
	float taaEdgeThreshold = 0.08f;
	float taaReactiveMaskStrength = 0.6f;
	float taaBloomPreservationStrength = 0.7f;

	//velocity texture for TAA
	GLuint velocityTex = 0;


	// Shadow settings
	bool enableShadows = true;
	float shadowBias = 0.0008f;
	float shadowNear = 0.001f;
	float shadowFar = 1000.0f;
	bool enablePCSS = true;
	float lightSize = 0.02f;
	
	// Shadow darkness settings - control how dark shadows appear for realism
	float shadowDarkness = 1.0f;           // Multiplier for shadow darkness [0.0=no shadows, 1.0=full darkness]
	float shadowMinBrightness = 0.0f;      // Keep direct-light shadows physically dark by default
	float shadowTransitionHardness = 1.0f; // Softness of shadow boundaries [0.5=very soft, 2.0=sharp]

	// Screen-space shadow settings (contact shadows)
	bool enableScreenSpaceShadows = true;
	float sssResolutionScale = 0.5f;
	float sssMaxRayLength = 10.0f;       // View-space units (changed from pixels)
	int sssSampleCount = 16;             // Reduced from 60 for better performance
	float sssThickness = 0.5f;           // Increased from 0.0015 for more visible shadows
	float sssEdgeThreshold = 0.01f;      // Increased from 0.0025 for better edge detection
	float sssBlendStrength = 0.6f;       // How much contact shadows blend with shadow maps (0.0-1.0)
	float sssLitAreaReduction = 0.7f;    // Reduce contact shadows in bright areas (0.0-1.0)
	float sssTemporalAlpha = 0.1f;


	// Post-processing settings
	float exposure = 0.8f;
	float gamma = 2.2f;
	bool enableHDR = true;

	// Tonemapping settings (GT / Uchimura)
	enum class TonemapType { None = 0, ACES = 1, GT = 2, GT7 = 3 };
	TonemapType tonemapType = TonemapType::ACES;
	float tm_P = 1.0f, tm_a = 1.0f, tm_m = 0.22f, tm_l = 0.4f, tm_c = 1.33f, tm_b = 0.0f;
	// GT7 parameters
	float tm7_peakNits = 1000.0f;   // display peak luminance
	float tm7_blend = 0.6f;
	float tm7_fadeStart = 0.98f;
	float tm7_fadeEnd = 1.16f;
	bool  tm7_useJzazbz = false;    // switch UCS
	bool outputSRGB = true;

	// Environment settings
	glm::vec3 envColor{ 0.05f, 0.05f, 0.05f };

	// IBL (Image-Based Lighting) intensity controls
	float iblIntensity = 0.35f;          // Overall IBL contribution 
	float skyboxExposure = 1.0f;        // Skybox background exposure
	float diffuseIBLScale = 0.3f;       // Diffuse IBL scale
	float specularIBLScale = 0.45f;     // Specular IBL scale

	// LPV Global Illumination settings
	bool enableLPV = false;
	float lpvGIStrength = 1.0f;
	int lpvGridResolution = 128;
	float lpvVoxelSize = 0.5f;
	glm::vec3 lpvGridCenter = glm::vec3(0.0f); // World-space center (updated per frame)
	glm::quat lpvGridOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // World-space orientation
	int lpvRSMResolution = 512;
	int lpvVPLSampleCount = 32000;
	int lpvPropagationIterations = 5;
	float lpvPropagationAttenuation = 0.9f;
	float lpvPropagationBias = 0.1f;
	bool lpvEnableOcclusion = false;
	int lpvUpdateFrequency = 1;
	bool lpvDebugVisualization = false; //Debug visualization toggle
	float lpvDebugBoost = 1.0f;         //Temporary boost for debugging (default 1.0x, set to 5.0x for testing)

	// RTX / Path Tracing settings (consolidated)

	// Debug visualization settings
	enum class DebugMode {
		NONE = 0,
		ALBEDO = 1,
		NORMAL = 2,
		DEPTH = 3,
		SHADOW_MAPS = 4,
		MOTION_VECTORS = 5,
		MATERIAL_ID = 6,  // Visualize material ID buffer
		TRANSFORM_ID = 7  // Visualize stable TransformID buffer
	};
	enum class ShadowDebugVisualization {
		NONE = 0,
		CASCADE_INDEX = 1,
		RAW_CASCADE_DEPTH = 2,
		BIAS_HEATMAP = 3,
		TEXEL_DENSITY = 4,
		SHADOW_MASK = 5
	};
	DebugMode debugMode = DebugMode::NONE;
	ShadowDebugVisualization shadowDebugVisualization = ShadowDebugVisualization::NONE;
	bool wireframeMode = false;
	bool showBoundingBoxes = false;
	bool showBBoxLegend = true;  // Show color legend when bounding boxes are enabled
	bool showLightGizmos = false;

	// Rendering overrides
	bool forceBackfaceCulling = false;  // Force backface culling on all meshes regardless of material settings

};
