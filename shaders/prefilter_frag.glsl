#version 460 core
out vec4 FragColor;
in vec3 WorldPos;

uniform samplerCube environmentMap;
uniform float roughness;
uniform float resolution;

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

    // CRITICAL FIX: Roughness-adaptive sample count
    // Smooth surfaces (low roughness) need fewer samples
    // Rough surfaces (high roughness) need MORE samples for proper blur
    uint baseSamples = 512u;
    uint roughSamples = uint(roughness * 3072.0); // 0 to 3072 based on roughness
    uint SAMPLE_COUNT = baseSamples + roughSamples;
    
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
   // CRITICAL FIX: Proper mip level calculation
   float D   = DistributionGGX(N, H, roughness);
          float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
     float pdf = D * NdotH / (4.0 * HdotV) + 0.0001; 

   float saTexel  = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

    // CRITICAL FIX: Better mip level selection
         float mipLevel = 0.0;
   if(roughness > 0.0) {
mipLevel = 0.5 * log2(saSample / saTexel);
     mipLevel = clamp(mipLevel, 0.0, 8.0); // Prevent excessive mip levels
   }
    
  // Sample environment and ensure positive
     vec3 envSample = textureLod(environmentMap, L, mipLevel).rgb;
         envSample = max(envSample, vec3(0.0));
 
       prefilteredColor += envSample * NdotL;
 totalWeight      += NdotL;
     }
    }

    // CRITICAL FIX: Proper normalization
    if(totalWeight > 0.0001) {
        prefilteredColor = prefilteredColor / totalWeight;
    } else {
   // Fallback: sample environment directly
        prefilteredColor = texture(environmentMap, R).rgb;
    }
 
    // Ensure strictly positive output
    prefilteredColor = max(prefilteredColor, vec3(0.0));

    // Safety check for NaN/Inf
    if (any(isnan(prefilteredColor)) || any(isinf(prefilteredColor))) {
        prefilteredColor = vec3(0.0);
    }

    FragColor = vec4(prefilteredColor, 1.0);
}