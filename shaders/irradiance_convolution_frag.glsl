#version 450 core
out vec4 FragColor;
in vec3 WorldPos;

uniform samplerCube environmentMap;

const float PI = 3.14159265359;

void main()
{		
    // The world vector acts as the normal of a tangent surface
    // from the origin, aligned to WorldPos. Given this normal, calculate all
    // incoming radiance of the environment. The result of this radiance
    // is the radiance of light coming from -Normal direction, which is what
    // we use in the PBR shader to sample irradiance.
    vec3 N = normalize(WorldPos);

    vec3 irradiance = vec3(0.0);   
    
    // tangent space calculation from origin point
    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up         = normalize(cross(N, right));
       
    // Sampling parameters - balanced quality vs performance
    float sampleDelta = 0.025; // Reasonable sample count
    float nrSamples = 0.0;
    
    // Hemisphere convolution for diffuse irradiance
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // spherical to cartesian (in tangent space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            // tangent space to world
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N; 

            // Sample the environment map
            vec3 envSample = texture(environmentMap, sampleVec).rgb;
        
            // CRITICAL FIX: Ensure samples are positive (clamp to prevent negative artifacts)
            envSample = max(envSample, vec3(0.0));
            
            // Apply cosine-weighted hemisphere integration
            // irradiance = integral( L * cos(theta) * sin(theta) )
            float cosTheta = max(cos(theta), 0.0);
            float sinTheta = max(sin(theta), 0.0);
       
            irradiance += envSample * cosTheta * sinTheta;
            nrSamples++;
        }
    }
    
    // CRITICAL FIX: Proper normalization for hemisphere integration
    // The PI factor is part of the Lambert BRDF (albedo / PI)
    // We integrate: integral(L * cos(theta) * sin(theta) * d_theta * d_phi)
    // Normalization: PI / total_samples
    irradiance = (PI * irradiance) / max(nrSamples, 1.0);
    
    // CRITICAL FIX: Final clamp to ensure strictly positive output
    irradiance = max(irradiance, vec3(0.0));
    
    // Additional safety check for NaN/Inf
    if (any(isnan(irradiance)) || any(isinf(irradiance))) {
        irradiance = vec3(0.0);
    }
    
    FragColor = vec4(irradiance, 1.0);
}