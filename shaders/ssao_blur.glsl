#version 460 core

in vec2 TexCoord;
out float FragColor;

uniform sampler2D ssaoInput;
uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM; 

uniform vec2 texelSize;
uniform float depthThreshold = 0.01;  // Tighter threshold for better edge preservation
uniform float normalThreshold = 0.15;

// FIXED: Decode oct-encoded normal with proper [0,1] to [-1,1] remapping
vec3 DecodeNormalOct8(vec2 e) {
    // Remap from [0,1] (texture storage) to [-1,1]
    e = e * 2.0 - 1.0;
    
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    
    if (n.z < 0.0) {
        // Handle lower hemisphere fold
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }
    
    return normalize(n);
}

void main() {
	float centerAO = texture(ssaoInput, TexCoord).r;
	float centerDepth = texture(gDepth, TexCoord).r;
	
	vec2 encNormal = texture(gPackedNormalRM, TexCoord).rg; 
	vec3 centerNormal = DecodeNormalOct8(encNormal);

	float result = 0.0;
	float weightSum = 0.0;

	// Bilateral blur with 5x5 kernel
	for (int x = -2; x <= 2; ++x) {
		for (int y = -2; y <= 2; ++y) {
			vec2 offset = vec2(x, y) * texelSize;
			vec2 sampleUV = TexCoord + offset;

			float sampleAO = texture(ssaoInput, sampleUV).r;
			float sampleDepth = texture(gDepth, sampleUV).r;
			vec2 encSampleNormal = texture(gPackedNormalRM, sampleUV).rg;
			vec3 sampleNormal = DecodeNormalOct8(encSampleNormal);

			// Depth-aware weighting
			float depthDiff = abs(centerDepth - sampleDepth);
			
			// Normal-aware weighting
			float normalDot = max(0.0, dot(centerNormal, sampleNormal));
			float normalDiff = 1.0 - normalDot;

			// Gaussian spatial weight based on distance
			float spatialWeight = exp(-float(x*x + y*y) / 4.0);

			// Combined bilateral weight
			float weight = spatialWeight * 
			               exp(-depthDiff / depthThreshold) * 
			               exp(-normalDiff / normalThreshold);
			
			result += sampleAO * weight;
			weightSum += weight;
		}
	}

	FragColor = result / max(weightSum, 1e-5);
}
