#pragma once
#include <string>
#include <glm/glm.hpp>

struct SimulationConfig {
    float timeStep = 0.0167f;
    glm::vec3 gravity = { 0.0f, -9.81f, 0.0f };
    std::string integrator = "rk4";
    float restitution = 0.4f;
    int maxIterations = 10;

    bool loadConfig(const std::string& configJsonFile);
};
