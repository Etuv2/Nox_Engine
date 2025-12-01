#pragma once
#include <string>
#include <glm/glm.hpp>

struct SimulationConfig {
    float timeStep = 0.01667f;  // 60 FPS default
    glm::vec3 gravity = { 0.0f, -9.81f, 0.0f };  // Earth gravity in m/s^2
    std::string integrator = "euler";  // euler is faster and more stable for game physics
    float restitution = 0.5f;  // Moderate bounciness
    int maxIterations = 10;

    bool loadConfig(const std::string& configJsonFile);
    
    // Validate configuration values
    void validate() {
        // Clamp timestep to reasonable range (30-240 FPS)
        if (timeStep < 0.004f) timeStep = 0.004f;  // Max 240 FPS
        if (timeStep > 0.0333f) timeStep = 0.0333f;  // Min 30 FPS
        
        // Clamp restitution to [0, 1]
        if (restitution < 0.0f) restitution = 0.0f;
        if (restitution > 1.0f) restitution = 1.0f;
        
        // Ensure valid integrator
        if (integrator != "euler" && integrator != "rk2" && 
            integrator != "rk4" && integrator != "verlet") {
            integrator = "euler";  // Default to euler
        }
 
        // Clamp iterations to reasonable range
        if (maxIterations < 1) maxIterations = 1;
        if (maxIterations > 20) maxIterations = 20;
    }
};
