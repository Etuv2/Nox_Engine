#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"

out vec2 FragColor;
in vec2 TexCoords;

// Note: PI, GeometrySchlickGGX, GeometrySmith are now in pbr_common.glsl (included above)


// REFERENCES:
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
// https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf
// Radical inverse for low-discrepancy sequences (Halton) : https://en.wikipedia.org/wiki/Halton_sequence

// Radical Inverse based on Van der Corput sequence
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

vec2 IntegrateBRDF(float NdotV, float roughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV*NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0; 

    vec3 N = vec3(0.0, 0.0, 1.0);
    
    const uint SAMPLE_COUNT = 1024u;
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        // generates a sample vector that's biased towards the
        // preferred alignment direction (importance sampling).
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 L = ImportanceSampleGGX(Xi, N, V, roughness);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(dot(normalize(V + L), N), 0.0);
        float VdotH = max(dot(V, normalize(V + L)), 0.0);

        if(NdotL > 0.0)
        {
            float Vis = VisibilitySmithGGXCorrelated(NdotV, NdotL, roughness);
            float G_Vis = (4.0 * Vis * VdotH * NdotL) / max(NdotH, 1e-6);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);
    return vec2(A, B);
}
void main() 
{
    vec2 integratedBRDF = IntegrateBRDF(TexCoords.x, TexCoords.y);
    FragColor = integratedBRDF;
}