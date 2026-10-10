#ifndef NOX_LENS_FLARE_COMMON_GLSL
#define NOX_LENS_FLARE_COMMON_GLSL

// Lens flare data (LensFlarePass, src/SceneRenderExtras.h).
struct NoxFlareSource {
	vec4 posStyle;         // xyz position (world), w style index
	vec4 facingLuminance;  // xyz emission direction (0 = omni), w luminance (cd/m^2)
	vec4 tintSize;         // rgb tint, w apparent glow diameter (m)
};

// One sprite layer of a style; NOX_FLARE_MAX_LAYERS per style.
struct NoxFlareLayer {
	vec4 layerGainWidthGlareMin;  // texture layer, gain, width, glare threshold
	vec4 tintGlareWeight;         // rgb tint, glare weight (0 = constant, 1 = scales with glare)
	vec4 flags;                   // x use source tint, y enabled
};

const int NOX_FLARE_MAX_LAYERS = 4;

layout(std430, binding = 45) readonly buffer NoxFlareSources {
	NoxFlareSource flareSources[];
};

layout(std430, binding = 47) readonly buffer NoxFlareStyles {
	NoxFlareLayer flareLayers[];   // style * NOX_FLARE_MAX_LAYERS + layer
};

#endif // NOX_LENS_FLARE_COMMON_GLSL
