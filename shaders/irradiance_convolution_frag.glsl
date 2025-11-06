#version 460 core
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
    // CRITICAL FIX: Use finer sampling for better quality (was 0.025, now 0.015)
    float sampleDelta = 0.015;
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
        
            // Ensure samples are positive
            envSample = max(envSample, vec3(0.0));
   
        // CRITICAL FIX: Proper cosine-weighted hemisphere integration
         // Lambert BRDF already has 1/PI, so irradiance integral is:
            // integral(L * cos(theta) * sin(theta) * d_theta * d_phi)
     float cosTheta = max(cos(theta), 0.0);
          float sinTheta = sin(theta);
       
       irradiance += envSample * cosTheta * sinTheta;
            nrSamples++;
        }
    }
  
    // CRITICAL FIX: Correct normalization
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