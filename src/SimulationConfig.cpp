#include "SimulationConfig.h"
#include <fstream>
#include <iostream>
#include "json.hpp"

using json = nlohmann::json;

bool SimulationConfig::loadConfig(const std::string& configJsonFile)
{
    std::ifstream in(configJsonFile);
    if (!in.is_open()) {
        std::cerr << "[SimulationConfig] Failed to open: " << configJsonFile << "\n";
        return false;
    }

    json rootJson;
    try {
        in >> rootJson;
    }
    catch (const std::exception& e) {
        std::cerr << "[SimulationConfig] Failed to parse JSON: " << e.what() << "\n";
        return false;
    }

    if (!rootJson.contains("physics") || !rootJson["physics"].is_object()) {
        std::cerr << "[SimulationConfig] Missing or malformed 'physics' block in config.\n";
        return false;
    }

    json physics = rootJson["physics"];

    if (physics.contains("gravity") && physics["gravity"].is_array() && physics["gravity"].size() == 3) {
        gravity = glm::vec3(
            physics["gravity"][0],
            physics["gravity"][1],
            physics["gravity"][2]);
        std::cout << "[Config] Gravity: " << gravity.x << ", " << gravity.y << ", " << gravity.z << "\n";
    }

    if (physics.contains("integration_method")) {
        integrator = physics["integration_method"].get<std::string>();
        std::cout << "[Config] Integrator: " << integrator << "\n";
    }

    if (physics.contains("integration_step")) {
        timeStep = physics["integration_step"].get<float>();
        std::cout << "[Config] Time step: " << timeStep << "\n";
    }

    if (physics.contains("restitution")) {
        restitution = physics["restitution"].get<float>();
        std::cout << "[Config] Restitution: " << restitution << "\n";
    }

    if (physics.contains("maxIterations")) {
        maxIterations = physics["maxIterations"].get<int>();
        std::cout << "[Config] Max iterations: " << maxIterations << "\n";
    }

    return true;
}
