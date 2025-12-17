#version 460 core
#include "includes/pbr_common.glsl" // For decoding normal
in vec2 TexCoord;
out float FragColor;

uniform sampler2D ssaoInput;
uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM; 

uniform vec2 texelSize;
uniform float depthThreshold = 0.005;  // Tighter threshold for better edge preservation
uniform float normalThreshold = 0.1;   // Tighter normal threshold



// Linearize depth for better comparison
float LinearizeDepth(float depth) {
    float near = 0.1;
    float far = 1000.0;
    float z = depth * 2.0 - 1.0; // Back to NDC
    return (2.0 * near * far) / (far + near - z * (far - near));
}

void main() {
	float centerAO = texture(ssaoInput, TexCoord).r;
	float centerDepth = texture(gDepth, TexCoord).r;
	
	vec2 encNormal = texture(gPackedNormalRM, TexCoord).rg; 
	vec3 centerNormal = DecodeNormalOct(encNormal);
	
	// Use linear depth for better bilateral comparison
	float centerLinearDepth = LinearizeDepth(centerDepth);

	float result = 0.0;
	float weightSum = 0.0;

	// Bilateral blur with 5x5 kernel
	const int KERNEL_RADIUS = 2;
	for (int x = -KERNEL_RADIUS; x <= KERNEL_RADIUS; ++x) {
		for (int y = -KERNEL_RADIUS; y <= KERNEL_RADIUS; ++y) {
			vec2 offset = vec2(x, y) * texelSize;
			vec2 sampleUV = TexCoord + offset;
			
			// Clamp to valid texture coordinates
			if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
				continue;
			}

			float sampleAO = texture(ssaoInput, sampleUV).r;
			float sampleDepth = texture(gDepth, sampleUV).r;
			vec2 encSampleNormal = texture(gPackedNormalRM, sampleUV).rg;
			vec3 sampleNormal = DecodeNormalOct(encSampleNormal);

			// Depth-aware weighting using linear depth
			float sampleLinearDepth = LinearizeDepth(sampleDepth);
			float depthDiff = abs(centerLinearDepth - sampleLinearDepth);
			
			// Improved normal similarity check
			float normalDot = max(0.0, dot(centerNormal, sampleNormal));
			
			//  Gaussian spatial weight based on distance
			float distSq = float(x*x + y*y);
			float spatialWeight = exp(-distSq / 8.0);

			// Bilateral weight combines spatial, depth, and normal similarity
			// Sharp falloff for edge preservation
			float depthWeight = exp(-depthDiff / depthThreshold);
			float normalWeight = pow(normalDot, 4.0); // Sharp normal falloff
			
			float weight = spatialWeight * depthWeight * normalWeight;
			
			result += sampleAO * weight;
			weightSum += weight;
		}
	}

	// Avoid division by zero and ensure minimum smoothing
	FragColor = result / max(weightSum, 1e-5);
}
