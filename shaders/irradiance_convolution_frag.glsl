#version 460 core
out vec4 FragColor;
in vec3 WorldPos;

uniform samplerCube environmentMap;

const float PI = 3.14159265359;

// Robust tangent frame construction that avoids pole discontinuities
// Uses the method from "Building an Orthonormal Basis, Revisited" (Duff et al. 2017)
void buildOrthonormalBasis(vec3 n, out vec3 tangent, out vec3 bitangent)
{
    // Choose the axis that's least parallel to n for better numerical stability
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    
    tangent = vec3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = vec3(b, sign + n.y * n.y * a, -n.y);
}

void main()
{		
    // The world vector acts as the normal of a tangent surface
    // from the origin, aligned to WorldPos. Given this normal, calculate all
    // incoming radiance of the environment.
    vec3 N = normalize(WorldPos);

    vec3 irradiance = vec3(0.0);   
    
    // Build robust tangent frame to avoid discontinuities at poles
    vec3 right, up;
    buildOrthonormalBasis(N, right, up);
       
    // Sampling parameters - balanced quality vs performance
    // Use finer sampling for better quality
    float sampleDelta = 0.015;
    float nrSamples = 0.0;
    
    // Hemisphere convolution for diffuse irradiance
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // Spherical to Cartesian (in tangent space)
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);
            float cosPhi = cos(phi);
            float sinPhi = sin(phi);
            
            vec3 tangentSample = vec3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
            
            // Tangent space to world space
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N; 
            sampleVec = normalize(sampleVec);

            // Sample the environment map
            vec3 envSample = texture(environmentMap, sampleVec).rgb;
        
            // Ensure samples are positive
            envSample = max(envSample, vec3(0.0));
   
            // Proper cosine-weighted hemisphere integration
            // Lambert BRDF already has 1/PI, so irradiance integral is:
            // integral(L * cos(theta) * sin(theta) * d_theta * d_phi)
            irradiance += envSample * cosTheta * sinTheta;
            nrSamples++;
        }
    }
  
    // Correct normalization
    // The integral over hemisphere with sin(theta) term gives PI
    // So we normalize by: PI / total_samples
    irradiance = PI * irradiance / max(nrSamples, 1.0);
    
    // Ensure strictly positive output
    irradiance = max(irradiance, vec3(0.0));
    
    // Safety check for NaN/Inf
    if (any(isnan(irradiance)) || any(isinf(irradiance))) {
        irradiance = vec3(0.0);
    }
    
    FragColor = vec4(irradiance, 1.0);
}