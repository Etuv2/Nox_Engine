#version 460 core

// Final upsample from working resolution (half-res) to full resolution
// Outputs full-res SSGI for the lighting pass

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D inSSGI;    // Working-res SSGI (half-res)
layout (binding = 1) uniform sampler2D depthTex;  // Full-res depth for guidance
layout (binding = 2) uniform sampler2D normalTex; // Full-res normals (oct-encoded)

// Output (full resolution)
layout (binding = 3, rgba16f) writeonly uniform image2D outFullRes;

// Parameters
uniform vec2 invDst;        // 1.0 / full-res
uniform float depthSigma;   // Depth threshold
uniform float normalThresh; // Normal threshold

vec3 octDecode(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(sign(e.x), sign(e.y));
		n.xy = (1.0 - abs(n.yx)) * s;
	}
	return normalize(n);
}

void main() {
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);
	ivec2 dstSize = imageSize(outFullRes);

	if (any(greaterThanEqual(id, dstSize))) return;

	vec2 uvFull = (vec2(id) + 0.5) * invDst;

	float centerDepth = textureLod(depthTex, uvFull, 0).r;
	vec3 centerNormal = octDecode(textureLod(normalTex, uvFull, 0).rg);

	vec3 result = vec3(0.0);
	float weightSum = 0.0;

	vec2 workingResSize = textureSize(inSSGI, 0);
	vec2 texelSize = 1.0 / workingResSize;

	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec2 uvNeighbor = uvFull + vec2(x, y) * texelSize;

			vec3 neighborColor = textureLod(inSSGI, uvNeighbor, 0).rgb;
			float neighborDepth = textureLod(depthTex, uvNeighbor, 0).r;
			vec3 neighborNormal = octDecode(textureLod(normalTex, uvNeighbor, 0).rg);

			float depthDiff = abs(centerDepth - neighborDepth);
			float depthWeight = exp(-depthDiff / depthSigma);

			float normalDot = max(dot(centerNormal, neighborNormal), 0.0);
			float normalDiff = max(0.0, 1.0 - normalDot);
			float normalWeight = exp(-normalDiff / normalThresh);

			if (normalDot < (1.0 - normalThresh)) {
				normalWeight = 0.0;
			}

			float weight = depthWeight * normalWeight;

			result += neighborColor * weight;
			weightSum += weight;
		}
	}

	vec3 upsampled = (weightSum > 1e-6)
		? (result / weightSum)
		: textureLod(inSSGI, uvFull, 0).rgb;

	upsampled = max(upsampled, vec3(0.0));

	imageStore(outFullRes, id, vec4(upsampled, 1.0));
}
