#version 450 core
out vec4 FragColor;
in vec3 WorldPos;

uniform samplerCube environmentMap;
uniform float roughness;
uniform float resolution; // CRITICAL FIX: Add resolution parameter

const float PI = 3.14159265359;

// ----------------------------------------------------------------------------
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}
// ----------------------------------------------------------------------------
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
// efficient VanDerCorpus calculation.
float RadicalInverse_VdC(uint bits) 
{
     bits = (bits << 16u) | (bits >> 16u);
     bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
     bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
     bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
     bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
     return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}
// ----------------------------------------------------------------------------
vec2 Hammersley(uint i, uint N)
{
	return vec2(float(i)/float(N), RadicalInverse_VdC(i));
}
// ----------------------------------------------------------------------------
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
	float a = roughness*roughness;
	
	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	
	// from spherical coordinates to cartesian coordinates - halfway vector
	vec3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;
	
	// from tangent-space H vector to world-space sample vector
	vec3 up          = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent   = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);
	
	vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
}
// ----------------------------------------------------------------------------
void main()
{		
    vec3 N = normalize(WorldPos);
  
    // make the simplifying assumption that V equals R equals the normal 
    vec3 R = N;
    vec3 V = R;

    // CRITICAL FIX: Adaptive sample count based on roughness
    // Use more samples for rough surfaces to ensure proper blur
    uint SAMPLE_COUNT = uint(1024.0 + roughness * 2048.0); // 1024 to 3072 samples
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;
    
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        // generates a sample vector that's biased towards the preferred alignment direction (importance sampling).
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if(NdotL > 0.0)
        {
            // CRITICAL FIX: Use actual resolution parameter instead of hardcoded 512
            float D   = DistributionGGX(N, H, roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001; 

            float saTexel  = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            // CRITICAL FIX: Better mip level calculation with minimum roughness handling
            float mipLevel = 0.0;
            if(roughness > 0.0) {
                mipLevel = 0.5 * log2(saSample / saTexel);
                // Clamp to prevent negative mip levels
                mipLevel = max(mipLevel, 0.0);
            }
    
            // Sample environment and ensure positive
            vec3 envSample = textureLod(environmentMap, L, mipLevel).rgb;
            envSample = max(envSample, vec3(0.0)); // Prevent negative samples
            
            prefilteredColor += envSample * NdotL;
            totalWeight      += NdotL;
        }
    }

    if(totalWeight > 0.0) {
        prefilteredColor = prefilteredColor / totalWeight;
    } else {
        // CRITICAL FIX: Fallback for edge cases
        prefilteredColor = texture(environmentMap, R).rgb;
    }
 
    // CRITICAL FIX: Ensure strictly positive output
    prefilteredColor = max(prefilteredColor, vec3(0.0));

    // CRITICAL FIX: Ensure rough surfaces are properly blurred
    // For very rough surfaces (roughness > 0.8), add extra blur
    if(roughness > 0.8) {
        vec3 extraBlur = vec3(0.0);
        int blurSamples = 16;
        float blurRadius = (roughness - 0.8) * 0.5; // 0 to 0.1 radius
    
        for(int i = 0; i < blurSamples; ++i) {
            float angle = float(i) * 2.0 * PI / float(blurSamples);
            vec3 offset = vec3(cos(angle) * blurRadius, sin(angle) * blurRadius, 0.0);
            vec3 sampleDir = normalize(R + offset);
            vec3 blurSample = texture(environmentMap, sampleDir).rgb;
            blurSample = max(blurSample, vec3(0.0)); // Prevent negative samples
            extraBlur += blurSample;
     }
      extraBlur /= float(blurSamples);
        
     // Blend with original result based on roughness
        float blendFactor = (roughness - 0.8) * 5.0; // 0 to 1
        prefilteredColor = mix(prefilteredColor, extraBlur, blendFactor * 0.3);
 }
    
    // Final safety check for NaN/Inf
    if (any(isnan(prefilteredColor)) || any(isinf(prefilteredColor))) {
     prefilteredColor = vec3(0.0);
    }

    FragColor = vec4(prefilteredColor, 1.0);
}