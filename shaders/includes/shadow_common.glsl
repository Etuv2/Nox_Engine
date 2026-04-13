#ifndef NOX_SHADOW_COMMON_GLSL
#define NOX_SHADOW_COMMON_GLSL

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

bool InShadowBounds(vec3 p) {
	return p.x >= 0.0 && p.x <= 1.0 &&
	       p.y >= 0.0 && p.y <= 1.0 &&
	       p.z >= -0.01 && p.z <= 1.01;
}

float CalculateSlopeBias(vec3 N, vec3 L, float baseBias) {
	float NdotL = max(dot(N, L), 0.001);
	float cosAngle = NdotL;
	float sinAngle = sqrt(max(1.0 - cosAngle * cosAngle, 0.0));
	float tanAngle = sinAngle / cosAngle;
	float slopeFactor = clamp(tanAngle, 0.0, 10.0);
	return baseBias * (1.0 + slopeFactor * 0.5);
}

float CalculateAdaptiveShadowBias(vec3 N, vec3 Ld, int cascadeIndex, float depthComp, float distance) {
	float slopeBias = CalculateSlopeBias(N, -Ld, shadowBias);
	float cascadeScale = 1.0 + float(cascadeIndex) * 0.1;
	float depthBias = shadowBias * 0.0001 * distance;
	float finalBias = slopeBias * cascadeScale + depthBias;
	return clamp(finalBias, shadowBias * 0.25, maxShadowBias);
}

float SampleShadowArray(int layer, vec3 projCoords, float bias) {
	if (!InShadowBounds(projCoords)) return 1.0;

	float sampleDepth = clamp(projCoords.z, 0.0, 1.0);
	ivec3 dims = textureSize(multiLightShadowArray, 0);
	vec2 texel = 1.0 / vec2(dims.xy);

	float sum = 0.0;
	for (int i = 0; i < 16; ++i) {
		vec2 offset = NOX_SHADOW_POISSON_DISK_16[i] * texel * 1.5;
		vec2 uv = projCoords.xy + offset;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
			sum += 1.0;
		} else {
			sum += texture(multiLightShadowArray, vec4(uv, float(layer), sampleDepth - bias));
		}
	}

	return sum / 16.0;
}

float SampleShadowArrayEdgeSafe(int layer, vec3 projCoords, float bias, float edgeMargin) {
	if (!InShadowBounds(projCoords)) return 1.0;

	ivec3 dims = textureSize(multiLightShadowArray, 0);
	vec2 texel = 1.0 / vec2(dims.xy);
	vec2 edgeDist = min(projCoords.xy, 1.0 - projCoords.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	float maxKernelRadius = minEdgeDist / texel.x;
	float kernelScale = clamp(maxKernelRadius / 1.5, 0.0, 1.0);
	float sampleDepth = clamp(projCoords.z, 0.0, 1.0);

	if (kernelScale < 0.1) {
		vec2 clampedUV = clamp(projCoords.xy, texel * 0.5, 1.0 - texel * 0.5);
		return texture(multiLightShadowArray, vec4(clampedUV, float(layer), sampleDepth - bias));
	}

	float sum = 0.0;
	float filterRadius = 1.5 * kernelScale;
	for (int i = 0; i < 16; ++i) {
		vec2 offset = NOX_SHADOW_POISSON_DISK_16[i] * texel * filterRadius;
		vec2 uv = clamp(projCoords.xy + offset, texel * 0.5, 1.0 - texel * 0.5);
		sum += texture(multiLightShadowArray, vec4(uv, float(layer), sampleDepth - bias));
	}

	return sum / 16.0;
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
