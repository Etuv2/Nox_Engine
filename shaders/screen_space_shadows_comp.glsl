#version 460 core

// Screen-Space Contact Shadows

// Input / Output
layout (binding = 0) uniform sampler2D uDepth;       // GBuffer depth
layout (r8, binding = 1) writeonly uniform image2D uOutShadow; // output 0..1 shadow

// === Parameters (std140, matches C++ struct) ===
layout (std140, binding = 2) uniform Params {
	vec2 invScreen;
	vec2 screenSize;

	vec3 lightDirVS;      // FROM surface TO light (view space)
	float maxRayLength;   
	int   numSteps;
	float thickness;
	float edgeThreshold;
	float jitterStrength;
	int   enableJitter;
	int   debugMode;
	int   _pad0, _pad1;

	mat4 invProj;
	mat4 proj;

	uint frameIndex;
	uint _pad2, _pad3, _pad4;

	int   enableBilateralBlur; 
	int   blurRadius;
	float depthSensitivity;
	float _pad5;
};


// Helpers


float Hash21(vec2 p) {
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

vec2 SubpixelDither(ivec2 pix) {
	float n = Hash21(vec2(pix) * 37.0) - 0.5;
	return n * 0.35 * invScreen;
}

float LinearizeDepth(float d) {
	float z = d * 2.0 - 1.0;
	float A = proj[2][2];
	float B = proj[3][2];
	float C = proj[2][3]; 
	return B / (z * C - A); // negative in front of camera
}

vec2 VS_to_UV(vec3 posVS) {
	vec4 clip = proj * vec4(posVS, 1.0);
	vec2 ndc = clip.xy / clip.w;
	return ndc * 0.5 + 0.5;
}

vec3 ReconstructVS(vec2 uv, float depth) {
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 vs = invProj * ndc;
	return vs.xyz / vs.w;
}

// Edge-aware bilinear fetch to prevent cross-edge sampling
float SampleDepthEdgeAware(vec2 uv, float centerDepth, float edgeThr) {
	ivec2 size = textureSize(uDepth, 0);
	vec2 texel = 1.0 / vec2(size);

	vec2 st = uv * vec2(size) - 0.5;
	ivec2 ij = ivec2(floor(st));
	vec2 f = fract(st);

	float d00 = texelFetch(uDepth, clamp(ij, ivec2(0), size - 1), 0).r;
	float d10 = texelFetch(uDepth, clamp(ij + ivec2(1,0), ivec2(0), size - 1), 0).r;
	float d01 = texelFetch(uDepth, clamp(ij + ivec2(0,1), ivec2(0), size - 1), 0).r;
	float d11 = texelFetch(uDepth, clamp(ij + ivec2(1,1), ivec2(0), size - 1), 0).r;

	if (abs(d00 - d10) > edgeThr || abs(d01 - d11) > edgeThr)
		return centerDepth;

	float dx0 = mix(d00, d10, f.x);
	float dx1 = mix(d01, d11, f.x);
	return mix(dx0, dx1, f.y);
}

// Simple occlusion check in view space
bool Occluded(float sampleDepth, float zVS, float thicknessVS) {
	float zSmp = LinearizeDepth(sampleDepth);
	return (zSmp > zVS - thicknessVS) && (zSmp < zVS + thicknessVS);
}


// Main kernel

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
	ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
	if (pix.x >= int(screenSize.x) || pix.y >= int(screenSize.y))
		return;

	vec2 uv = (vec2(pix) + 0.5) * invScreen;
	float depthCenter = texture(uDepth, uv).r;
	vec3 posVS = ReconstructVS(uv, depthCenter);

	// Correct per-pixel stable jitter
	vec2 jitter = vec2(0.0);
	if (enableJitter != 0) {
		float h = Hash21(vec2(pix));
		float a = h * 6.2831853;
		jitter = vec2(cos(a), sin(a)) * jitterStrength * 0.5;
	}

	vec2 dither = SubpixelDither(pix);
	vec3 dirVS = normalize(lightDirVS); // already FROM surface TO light
	float stepLenVS = maxRayLength / float(max(1, numSteps));
	vec3 stepVS = dirVS * stepLenVS;

	float vis = 1.0;
	vec3 sampleVS = posVS + dirVS * (stepLenVS * 0.5);

	for (int i = 0; i < numSteps; ++i) {
		vec2 suv = VS_to_UV(sampleVS) + (jitter + dither) * invScreen;
		if (any(bvec2(suv.x < 0.0, suv.x > 1.0)) || any(bvec2(suv.y < 0.0, suv.y > 1.0)))
			break;

		float depthSmp = SampleDepthEdgeAware(suv, depthCenter, edgeThreshold);
		if (Occluded(depthSmp, sampleVS.z, thickness)) {
			vis -= 1.0 / float(numSteps);
		}

		sampleVS += stepVS;
	}

	// Clamp to [0,1]
	vis = clamp(vis, 0.0, 1.0);

	// Optional bilateral blur
	// Bilateral blur optimized to prevent branching
	const int R = 1;
	float zC = depthCenter;
	float wSum = 0.0, vSum = 0.0;
	for (int dy = -R; dy <= R; ++dy) {
		for (int dx = -R; dx <= R; ++dx) {
			ivec2 q = clamp(pix + ivec2(dx, dy), ivec2(0), ivec2(screenSize) - 1);
			vec2 uvQ = (vec2(q) + 0.5) * invScreen;
			float zQ = texture(uDepth, uvQ).r;
			float vQ = vis;
			float wDepth = exp(-abs(LinearizeDepth(zQ) - LinearizeDepth(zC)) / depthSensitivity);
			float wSpatial = exp(-float(dx*dx + dy*dy) * 0.5);
			float w = wDepth * wSpatial;
			wSum += w;
			vSum += vQ * w;
		}
	}
	float blurred = vSum / max(wSum, 1e-6);
	float blurMix = float(enableBilateralBlur != 0) * 0.25;
	vis = mix(vis, blurred, blurMix);
	imageStore(uOutShadow, pix, vec4(vis, vis, vis, 1.0));
}
