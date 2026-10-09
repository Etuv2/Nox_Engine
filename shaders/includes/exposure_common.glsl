#ifndef NOX_EXPOSURE_COMMON_GLSL
#define NOX_EXPOSURE_COMMON_GLSL

// Camera exposure state (ExposurePass, docs/nfs2015/README.md M3).
// preExposure: multiplier every lighting/sky/emissive shader applies to the radiance it writes
//              this frame (1.0 for legacy, non-photometric scenes).
// exposure:    final exposure for this frame (auto exposure updates it after TAA); post applies
//              exposure / preExposure to the pre-exposed image.
// Frostbite PBR 3.0: exposure = 1 / (1.2 * 2^EV100).

#ifndef NOX_EXPOSURE_WRITE
layout(std430, binding = 42) readonly buffer ExposureState {
	float exposurePre;
	float exposureCurrent;
	float exposureEV100;
	float exposureAverageLuminance;
	float exposurePrevPre;   // pre-exposure of the previous frame (history buffers are in this scale)
};

float NoxPreExposure() { return exposurePre; }
float NoxExposureRatio() { return exposureCurrent / max(exposurePre, 1e-12); }
// Rescales a value read from last frame's pre-exposed history into this frame's pre-exposure.
float NoxHistoryExposureScale() { return exposurePre / max(exposurePrevPre, 1e-12); }
#endif

#endif // NOX_EXPOSURE_COMMON_GLSL
