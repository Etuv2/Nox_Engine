#version 460 core

// 2x upsample with joint bilateral filter - ALIGNED WITH SSAO APPROACH
// Input: sampler2D inLow (quarter), sampler2D inHigh (half/working resolution - guidance),
//        depthTex (full), normalTex (full)
// Output: image2D outTex (half/working resolution)

layout (local_size_x = 8, local_size_y = 8) in;

// Input bindings
layout (binding = 0) uniform sampler2D inLow;       // Quarter-res blurred SSGI
layout (binding = 1) uniform sampler2D inHigh;      // Working-res guidance (raw SSGI)
layout (binding = 2) uniform sampler2D depthTex;    // Full-res depth
layout (binding = 3) uniform sampler2D normalTex;   // Full-res normals (oct-encoded)
layout (binding = 4, rgba16f) writeonly uniform image2D outTex; // Working-res output

// Parameters
uniform vec2 invDst;    // 1.0 / working-res (destination)
uniform vec2 invFull;     // 1.0 / full-res (for depth/normal sampling)
uniform float depthSigma;   // Depth threshold for edge detection
uniform float normalThresh; // Normal threshold for edge detection

// CRITICAL FIX: Use exact same octahedral decoding as SSAO
vec3 octDecode(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(sign(e.x), sign(e.y));
		n.xy = (1.0 - abs(n.yx)) * s;
	}
	return normalize(n);
}

vec3 DecodeNormalOct8(vec2 e) {
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
    return normalize(n);
}

void main() {
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);
	ivec2 dstSize = imageSize(outTex);

	if (any(greaterThanEqual(id, dstSize))) {
		return;
	}

	// Compute UVs for working and screen resolution
	vec2 uvWork = (vec2(id) + 0.5) * invDst;
	vec2 uvScreen = uvWork; // Normalized UVs map directly

	// CRITICAL FIX: Sample center pixel depth/normal from FULL RESOLUTION
	float centerDepth = textureLod(depthTex, uvScreen, 0).r;
	vec3 centerNormal = octDecode(textureLod(normalTex, uvScreen, 0).rg);

	// Joint bilateral upsample with 3x3 kernel (matches SSAO logic)
	vec3 result = vec3(0.0);
	float weightSum = 0.0;

	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			// Neighbor UVs in working space
			vec2 uvNWork = uvWork + vec2(x, y) * invDst;
			vec2 uvNScreen = uvNWork;

			// Sample low-res (quarter) SSGI
			vec3 neighborColor = textureLod(inLow, uvNWork, 0).rgb;

			// Sample depth/normal from FULL RESOLUTION
			float neighborDepth = textureLod(depthTex, uvNScreen, 0).r;
			vec3 neighborNormal = octDecode(textureLod(normalTex, uvNScreen, 0).rg);

			// Depth weight: exponential falloff
			float depthDiff = abs(centerDepth - neighborDepth);
			float depthWeight = exp(-depthDiff / depthSigma);

			// Normal weight: exponential falloff with hard cutoff for sharp edges
			float normalDot = max(dot(centerNormal, neighborNormal), 0.0);
			float normalDiff = max(0.0, 1.0 - normalDot);
			float normalWeight = exp(-normalDiff / normalThresh);

			// Hard cutoff for sharp edges (prevents bleeding)
			if (normalDot < (1.0 - normalThresh)) {
				normalWeight = 0.0;
			}

			// Combined weight
			float weight = depthWeight * normalWeight;

			result += neighborColor * weight;
			weightSum += weight;
		}
	}

	// Normalize result
	vec3 upsampled = (weightSum > 1e-6)
		? (result / weightSum)
		: textureLod(inLow, uvWork, 0).rgb;

	// Blend with high-res guidance to preserve detail (subtle blend)
	vec3 highRes = textureLod(inHigh, uvWork, 0).rgb;
	vec3 final = mix(upsampled, highRes, 0.15);

	// Ensure non-negative output
	final = max(final, vec3(0.0));

	imageStore(outTex, id, vec4(final, 1.0));
}
