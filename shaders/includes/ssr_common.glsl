#ifndef NOX_SSR_COMMON_GLSL
#define NOX_SSR_COMMON_GLSL

// Screen-space reflections shared helpers (docs/nfs2015/README.md, M5).
// Conventions: right-handed view space, camera looks down -Z, depth textures store window-space
// depth in [0,1]; projection is the frame's (jittered) projection matrix.

uniform mat4 uProj;
uniform mat4 uInvProj;
uniform mat4 uView;
uniform mat4 uInvView;
uniform vec2 uFullSize;      // full-resolution size in pixels
uniform float uMaxRoughness; // perceptual roughness above which SSR is skipped

const float SSR_PI = 3.14159265359;

float SsrLinearViewZ(float depth) {
	// glm::perspective: clip.z = A z + B, clip.w = -z  ->  z = -B / (ndcZ + A)
	float ndcZ = depth * 2.0 - 1.0;
	return -uProj[3][2] / (ndcZ + uProj[2][2]);
}

vec3 SsrViewPosFromDepth(vec2 uv, float depth) {
	vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 v = uInvProj * clip;
	return v.xyz / v.w;
}

// Identical to DecodeNormalOct in pbr_common.glsl (G-buffer RT0 encoding).
vec3 SsrDecodeOct(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n;
	n.z = 1.0 - abs(e.x) - abs(e.y);
	if (n.z < 0.0) {
		vec2 signE = sign(e);
		signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
		n.xy = (1.0 - abs(e.yx)) * signE;
	} else {
		n.xy = e.xy;
	}
	return normalize(n);
}

float SsrHash(vec2 p, float seed) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031 + seed * 0.618034);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

// GGX distribution (alpha = perceptual roughness^2).
float SsrD_GGX(float NdotH, float alpha) {
	float a2 = alpha * alpha;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / (SSR_PI * d * d);
}

float SsrG1_Smith(float NdotX, float alpha) {
	float a2 = alpha * alpha;
	return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// Heitz 2018, "Sampling the GGX Distribution of Visible Normals". Ve in tangent space (z = N).
vec3 SsrSampleGGXVNDF(vec3 Ve, float alpha, vec2 u) {
	vec3 Vh = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
	float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 T2 = cross(Vh, T1);
	float r = sqrt(u.x);
	float phi = 2.0 * SSR_PI * u.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
	vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
	return normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

// pdf of the reflected direction for VNDF sampling: G1(V) D(H) / (4 NdotV).
float SsrPdfVNDF(float NdotV, float NdotH, float alpha) {
	return SsrG1_Smith(NdotV, alpha) * SsrD_GGX(NdotH, alpha) / max(4.0 * NdotV, 1e-6);
}

// Specular BRDF value (without Fresnel, which the split-sum applies) times NdotL.
float SsrBRDFCos(vec3 N, vec3 V, vec3 L, float alpha) {
	vec3 H = normalize(V + L);
	float NdotL = max(dot(N, L), 0.0);
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = max(dot(N, H), 0.0);
	float G = SsrG1_Smith(NdotL, alpha) * SsrG1_Smith(NdotV, alpha);
	return SsrD_GGX(NdotH, alpha) * G / (4.0 * NdotV) ;
}

#endif // NOX_SSR_COMMON_GLSL
