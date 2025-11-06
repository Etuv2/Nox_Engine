#version 460 core
out vec4 FragColor;
in vec3 TexCoords;

uniform samplerCube environmentMap;
uniform float skyboxExposure = 1.0; // Exposure control for skybox background

void main()
{
    // Sample the environment map
    vec3 envColor = texture(environmentMap, normalize(TexCoords)).rgb;
    
    // Apply exposure control to skybox background
    // This allows independent control of skybox brightness vs IBL lighting
    envColor *= skyboxExposure;
    
    // Since we're rendering into the HDR buffer, keep values linear
    // Remove tone mapping and gamma correction - these happen in post-processing
    
    // Only clamp extreme values to prevent NaN/Inf
    envColor = clamp(envColor, vec3(0.0), vec3(65000.0));
    
    // Check for NaN values and replace with safe fallback
    if (any(isnan(envColor)) || any(isinf(envColor))) {
        envColor = vec3(0.0, 0.0, 0.0); // Black fallback for HDR
    }
    
    // Output linear HDR values - tone mapping happens in post-processing
    FragColor = vec4(envColor, 1.0);
}
