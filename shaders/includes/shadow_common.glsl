#ifndef NOX_SHADOW_COMMON_GLSL
#define NOX_SHADOW_COMMON_GLSL

// Expects the including shader to declare:
//   uniform sampler2DArrayShadow multiLightShadowArray;
//   buffer ... { mat4 shadowMatrices[]; };
//   uniform float shadowBias;   // RenderContext::shadowBias, used as a bias strength scale

#include "shadow_bias_common.glsl"

const vec2 NOX_SHADOW_POISSON_DISK_16[16] = vec2[](
	vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
	vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
	vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
	vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
	vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
	vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
	vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
	vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
);

vec3 GetCascadeDebugColor(int cascadeIndex) {
	if (cascadeIndex == 0) return vec3(1.0, 0.25, 0.2);
	if (cascadeIndex == 1) return vec3(0.25, 1.0, 0.35);
	if (cascadeIndex == 2) return vec3(0.3, 0.45, 1.0);
	if (cascadeIndex == 3) return vec3(1.0, 0.85, 0.25);
	return vec3(1.0, 0.2, 1.0);
}

// No upper depth bound: a point beyond the far plane is farther from the light than everything
// in the map, so comparing it at depth 1.0 (SampleShadowArrayFiltered clamps) is exact - it is
// shadowed iff any caster was rendered at that texel. Directional cascades only span their
// bounding sphere in depth, so this matters for lookups from outside the view frustum.
bool InShadowBounds(vec3 p) {
	return p.x >= 0.0 && p.x <= 1.0 &&
	       p.y >= 0.0 && p.y <= 1.0 &&
	       p.z >= -0.01;
}

float ShadowHash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float ComputeAdaptiveFilterRadiusTexels(vec3 projCoords, ivec3 dims, float edgeMargin, float radiusScale) {
	vec2 texel = 1.0 / vec2(dims.xy);
	vec2 edgeDist = min(projCoords.xy, 1.0 - projCoords.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	float minEdgeTexels = min(edgeDist.x / texel.x, edgeDist.y / texel.y);
	float safeEdgeRadius = max(minEdgeTexels - 0.75, 0.0);

	float effectiveMargin = max(edgeMargin, max(texel.x, texel.y) * 1.5);
	float edgeFade = smoothstep(0.0, effectiveMargin, minEdgeDist);
	float depthSoftness = smoothstep(0.35, 1.0, clamp(projCoords.z, 0.0, 1.0));
	float baseRadius = mix(0.55, 1.15, depthSoftness);

	float radiusTexels = baseRadius * max(radiusScale, 0.2);
	radiusTexels = min(radiusTexels, safeEdgeRadius);
	return clamp(radiusTexels * edgeFade, 0.0, 2.25);
}

float SampleShadowArrayFiltered(int layer, vec3 projCoords, float bias, float edgeMargin, float radiusScale) {
	if (!InShadowBounds(projCoords)) {
		return 1.0;
	}

	ivec3 dims = textureSize(multiLightShadowArray, 0);
	vec2 texel = 1.0 / vec2(dims.xy);
	vec2 centerUV = clamp(projCoords.xy, texel * 0.5, 1.0 - texel * 0.5);
	float sampleDepth = clamp(projCoords.z, 0.0, 1.0);

	float radiusTexels = ComputeAdaptiveFilterRadiusTexels(projCoords, dims, edgeMargin, radiusScale);
	if (radiusTexels < 0.2) {
		return texture(multiLightShadowArray, vec4(centerUV, float(layer), sampleDepth - bias));
	}

	vec2 texelCoord = floor(projCoords.xy * vec2(dims.xy));
	float angle = ShadowHash12(texelCoord) * 6.28318530718;
	float s = sin(angle);
	float c = cos(angle);
	mat2 rot = mat2(c, -s, s, c);

	float sum = 0.0;
	float weightSum = 0.0;
	for (int i = 0; i < 16; ++i) {
		vec2 disk = rot * NOX_SHADOW_POISSON_DISK_16[i];
		vec2 uv = projCoords.xy + disk * texel * radiusTexels;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
			continue;
		}

		float radial = dot(disk, disk);
		float weight = 1.0 / (1.0 + radial * 1.5);
		sum += texture(multiLightShadowArray, vec4(uv, float(layer), sampleDepth - bias)) * weight;
		weightSum += weight;
	}

	if (weightSum <= 1e-5) {
		return texture(multiLightShadowArray, vec4(centerUV, float(layer), sampleDepth - bias));
	}

	return sum / weightSum;
}

float SampleShadowArray(int layer, vec3 projCoords, float bias) {
	return SampleShadowArrayFiltered(layer, projCoords, bias, 0.0, 1.0);
}

float SampleShadowArrayEdgeSafe(int layer, vec3 projCoords, float bias, float edgeMargin, float radiusScale) {
	return SampleShadowArrayFiltered(layer, projCoords, bias, edgeMargin, radiusScale);
}

float SampleShadowArrayEdgeSafe(int layer, vec3 projCoords, float bias, float edgeMargin) {
	return SampleShadowArrayFiltered(layer, projCoords, bias, edgeMargin, 1.0);
}

// World-space receiver offset (in world units) that SampleShadowLayerBiased applies along the
// normal for this layer; exposed for the shadow debug view.
float ComputeShadowNormalOffsetWorld(int layer, vec3 worldPos, vec3 N, vec3 L, float filterRadiusTexels) {
	float texelWorld = NoxShadowTexelWorldSize(shadowMatrices[layer], worldPos, float(textureSize(multiLightShadowArray, 0).x));
	vec3 receiver = NoxShadowReceiverPosition(worldPos, N, L, texelWorld, filterRadiusTexels, NoxShadowBiasScale(shadowBias));
	return dot(receiver - worldPos, N);
}

// Filtered visibility of one shadow layer (cascade, spot map or cube face) for a surface point.
// L is the unit direction from the surface towards the light. All biasing happens in world space
// (see shadow_bias_common.glsl), so the depth comparison itself is unbiased.
// valid is false when the point does not project into this layer.
float SampleShadowLayerBiased(int layer, vec3 worldPos, vec3 N, vec3 L, float edgeMargin, float radiusScale, out bool valid) {
	mat4 lightSpace = shadowMatrices[layer];
	vec3 surfaceCoords;
	valid = NoxProjectToShadowMap(lightSpace, worldPos, surfaceCoords) && InShadowBounds(surfaceCoords);
	if (!valid) {
		return 1.0;
	}

	ivec3 dims = textureSize(multiLightShadowArray, 0);
	float filterRadiusTexels = ComputeAdaptiveFilterRadiusTexels(surfaceCoords, dims, edgeMargin, radiusScale);
	float texelWorld = NoxShadowTexelWorldSize(lightSpace, worldPos, float(dims.x));
	vec3 receiver = NoxShadowReceiverPosition(worldPos, N, L, texelWorld, filterRadiusTexels, NoxShadowBiasScale(shadowBias));

	vec3 receiverCoords;
	if (!NoxProjectToShadowMap(lightSpace, receiver, receiverCoords)) {
		return 1.0;
	}
	// The offset spans a few texels at most; keep the lookup inside the map when the surface point
	// sits right on its border.
	receiverCoords.xy = clamp(receiverCoords.xy, vec2(0.0), vec2(1.0));
	return SampleShadowArrayFiltered(layer, receiverCoords, 0.0, edgeMargin, radiusScale);
}

float EstimateCascadeCoverage(vec3 projCoords) {
	vec2 edgeDist = min(projCoords.xy, 1.0 - projCoords.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	return clamp(minEdgeDist / 0.5, 0.0, 1.0);
}

float EstimateCascadeTexelDensity(int layer, vec3 worldPos) {
	ivec3 dims = textureSize(multiLightShadowArray, 0);
	float shadowTexels = float(max(dims.x, dims.y));
	mat4 M = shadowMatrices[layer];
	vec4 lsp = M * vec4(worldPos, 1.0);
	vec3 pc = lsp.xyz / lsp.w;
	vec2 dpdx = abs(dFdx(pc.xy));
	vec2 dpdy = abs(dFdy(pc.xy));
	float pixelFootprint = max(max(dpdx.x, dpdx.y), max(dpdy.x, dpdy.y));
	float texelsPerPixel = pixelFootprint * shadowTexels;
	return clamp(texelsPerPixel / 8.0, 0.0, 1.0);
}

#endif
