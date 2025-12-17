#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"

out vec4 FragColor;
in vec3 WorldPos;

uniform samplerCube environmentMap;
uniform float roughness;
uniform float resolution;

// Radical inverse for low-discrepancy sequences (Halton)
float RadicalInverse_VdC(uint bits) 
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// Hammersley sequence for better sample distribution
vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i)/float(N), RadicalInverse_VdC(i));
}

// ----------------------------------------------------------------------------
// Robust tangent frame construction (Duff et al. 2017)
// This avoids discontinuities at poles that cause seams
void buildOrthonormalBasis(vec3 n, out vec3 tangent, out vec3 bitangent)
{
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    
    tangent = vec3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = vec3(b, sign + n.y * n.y * a, -n.y);
}

// ----------------------------------------------------------------------------
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness*roughness;
    
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
    
    // From spherical coordinates to Cartesian coordinates - halfway vector
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
    
    // Use robust tangent frame construction to avoid seams
    vec3 tangent, bitangent;
    buildOrthonormalBasis(N, tangent, bitangent);
    
    // Transform from tangent space to world space
    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

// ----------------------------------------------------------------------------
void main()
{		
    vec3 N = normalize(WorldPos);
    
    // Make the simplifying assumption that V equals R equals the normal 
    vec3 R = N;
    vec3 V = R;

    // Roughness-adaptive sample count
    // Smooth surfaces (low roughness) need fewer samples
    // Rough surfaces (high roughness) need MORE samples for proper blur
    uint baseSamples = 512u;
    uint roughSamples = uint(roughness * 3072.0); // 0 to 3072 based on roughness
    uint SAMPLE_COUNT = baseSamples + roughSamples;
    
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;
    
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        // Generates a sample vector that's biased towards the preferred alignment direction (importance sampling)
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if(NdotL > 0.0)
        {
            // Proper mip level calculation
            float D = DistributionGGX(N, H, roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001; 

            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            // Better mip level selection
            float mipLevel = 0.0;
            if(roughness > 0.0) {
                mipLevel = 0.5 * log2(saSample / saTexel);
                mipLevel = clamp(mipLevel, 0.0, 8.0); // Prevent excessive mip levels
            }
            
            // Sample environment and ensure positive
            vec3 envSample = textureLod(environmentMap, L, mipLevel).rgb;
            envSample = max(envSample, vec3(0.0));
            
            prefilteredColor += envSample * NdotL;
            totalWeight += NdotL;
        }
    }

    // Proper normalization
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