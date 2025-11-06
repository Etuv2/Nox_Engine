#version 460 core

in vec2 TexCoord;
out float FragColor;

uniform sampler2D ssaoInput;
uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM; 

uniform vec2 texelSize;
uniform float depthThreshold = 0.02;
uniform float normalThreshold = 0.2;

// Decode oct-encoded normal from gNormal
vec3 DecodeNormalOct8(vec2 e) {
	vec3 n;
	n.z = 1.0 - abs(e.x) - abs(e.y);
	n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
	return normalize(n);
}

void main() {
	float centerAO = texture(ssaoInput, TexCoord).r;
	float centerDepth = texture(gDepth, TexCoord).r;
	
	vec2 encNormal = texture(gPackedNormalRM, TexCoord).rg; 
	vec3 centerNormal = DecodeNormalOct8(encNormal);

	float result = 0.0;
	float weightSum = 0.0;

	for (int x = -2; x <= 2; ++x) {
		for (int y = -2; y <= 2; ++y) {
			vec2 offset = vec2(x, y) * texelSize;
			vec2 sampleUV = TexCoord + offset;

			float sampleAO = texture(ssaoInput, sampleUV).r;
			float sampleDepth = texture(gDepth, sampleUV).r;
			vec2 encSampleNormal = texture(gPackedNormalRM, sampleUV).rg;
			vec3 sampleNormal = DecodeNormalOct8(encSampleNormal);

			float depthDiff = abs(centerDepth - sampleDepth);
			float normalDiff = max(0.0, 1.0 - dot(centerNormal, sampleNormal));

			float weight = exp(-depthDiff / depthThreshold) * exp(-normalDiff / normalThreshold);
			result += sampleAO * weight;
			weightSum += weight;
		}
	}

	FragColor = result / max(weightSum, 1e-5);
}
