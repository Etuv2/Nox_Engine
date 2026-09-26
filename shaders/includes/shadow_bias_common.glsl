#ifndef NOX_SHADOW_BIAS_COMMON_GLSL
#define NOX_SHADOW_BIAS_COMMON_GLSL

// Receiver-side shadow biasing expressed in world space, sized from each shadow layer's own
// texel footprint. Shared by the deferred lighting pass and the surfel GI tracer.
//
// Biasing in NDC depth does not work for this renderer: each directional cascade has its own
// depth range and perspective (spot/point) depth is non-linear, so one constant turns into
// millimetres near the light and metres far away. Instead the receiver is moved:
//   * along the surface normal, by enough to clear the depth slope across the PCF footprint
//     (a filter reaching R texels needs R * texel * sin(theta) of clearance), and
//   * slightly towards the light, to absorb depth-reconstruction noise on lit-facing surfaces.
// The comparison itself is then done without any additional depth bias.

// RenderContext::shadowBias value that corresponds to the default bias strength (1x). The UI
// "Shadow Bias" control scales the texel-based offsets relative to it.
const float NOX_SHADOW_REFERENCE_BIAS = 0.001;
// Normal clearance on top of the filter radius: half a texel for texel-centre quantization plus
// one texel for the bilinear footprint of hardware depth comparison.
const float NOX_SHADOW_NORMAL_BIAS_TEXELS = 1.5;
const float NOX_SHADOW_LIGHT_BIAS_TEXELS = 0.5;

float NoxShadowBiasScale(float configuredBias) {
	return clamp(configuredBias / NOX_SHADOW_REFERENCE_BIAS, 0.05, 20.0);
}

// World-space edge length of one shadow-map texel around worldPos for a light-space (clip)
// matrix. Exact for orthographic cascades, first order around the axis for perspective maps.
float NoxShadowTexelWorldSize(mat4 lightSpace, vec3 worldPos, float resolution) {
	vec4 clip = lightSpace * vec4(worldPos, 1.0);
	vec3 row0 = vec3(lightSpace[0][0], lightSpace[1][0], lightSpace[2][0]);
	vec3 row1 = vec3(lightSpace[0][1], lightSpace[1][1], lightSpace[2][1]);
	float ndcPerWorld = max(length(row0), length(row1)) / max(abs(clip.w), 1e-6);
	return 2.0 / (max(resolution, 1.0) * max(ndcPerWorld, 1e-8));
}

// Position to project into the shadow map instead of the raw surface position.
// N: surface normal, L: unit direction from the surface towards the light.
vec3 NoxShadowReceiverPosition(vec3 worldPos, vec3 N, vec3 L, float texelWorld, float filterRadiusTexels, float biasScale) {
	float NdotL = clamp(dot(N, L), 0.0, 1.0);
	float sinTheta = sqrt(max(1.0 - NdotL * NdotL, 0.0));
	float normalOffset = texelWorld * biasScale * (NOX_SHADOW_NORMAL_BIAS_TEXELS + max(filterRadiusTexels, 0.0)) * sinTheta;
	float lightOffset = texelWorld * biasScale * NOX_SHADOW_LIGHT_BIAS_TEXELS;
	return worldPos + N * normalOffset + L * lightOffset;
}

// Projects a world position into [0,1] shadow-map space. Returns false behind a perspective
// light (w <= 0), where the projection is meaningless.
bool NoxProjectToShadowMap(mat4 lightSpace, vec3 worldPos, out vec3 projCoords) {
	vec4 clip = lightSpace * vec4(worldPos, 1.0);
	if (clip.w <= 1e-6) {
		projCoords = vec3(0.0);
		return false;
	}
	projCoords = clip.xyz / clip.w * 0.5 + 0.5;
	return true;
}

#endif
