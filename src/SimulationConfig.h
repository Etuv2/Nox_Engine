#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include "json.hpp"

/**
 * SimulationConfig - Physics simulation configuration parameters
 * 
 * Contains all tunable parameters for the physics engine including:
 * - Fixed timestep settings
 * - Solver iteration counts
 * - Gravity and damping
 * - Collision detection parameters
 * - Sleep thresholds
 */
struct SimulationConfig {
    // Fixed timestep configuration
    float fixedDeltaTime = 1.0f / 60.0f;      // Fixed timestep (default 60 Hz)
    int maxSubSteps = 4;                       // Maximum substeps per frame
    float maxAccumulatedTime = 0.1f;           // Clamp accumulated time to prevent spiral of death
    
    // Gravity
    float gravityX = 0.0f;
    float gravityY = -9.81f;
    float gravityZ = 0.0f;
    
    // Solver parameters
    int velocityIterations = 10;               // Velocity constraint solver iterations
    int positionIterations = 4;                // Position correction iterations
    float baumgarteFactor = 0.2f;              // Positional correction factor (0.1-0.3 typical)
    float allowedPenetration = 0.005f;         // Tighter slop (was 0.01)
    float restitutionThreshold = 0.5f;         // Lower threshold (was 1.0)
    
    // Contact parameters
    float contactBreakingThreshold = 0.02f;    // Distance for contact point invalidation
    float warmStartingFactor = 0.8f;           // Impulse caching factor (0.0 = no warm start)
    int maxContactPoints = 4;                  // Maximum contact points per manifold
    
    // Sleep parameters
    bool enableSleeping = true;
    float sleepLinearThreshold = 0.05f;        // Tighter threshold (was 0.1)
    float sleepAngularThreshold = 0.05f;       // Tighter threshold (was 0.1)
    float sleepTimeThreshold = 0.5f;           // Time below threshold before sleep
    
    // Damping defaults
    float defaultLinearDamping = 0.01f;        // Add small damping (was 0.0)
    float defaultAngularDamping = 0.05f;
    float defaultFriction = 0.5f;
    float defaultRestitution = 0.0f;           // No bounce by default (was 0.3)
    
    // BVH/Broadphase parameters
    float fatAABBMargin = 0.1f;                // Fat AABB expansion margin
    float aabbPrediction = 0.0f;               // AABB expansion for velocity prediction
    
    // Debug options
    bool enableDebugDraw = false;
    bool verboseLogging = false;
    bool debugDrawContacts = false;            // Draw contact points and normals
    bool debugDrawBroadphase = false;          // Draw broadphase AABBs
    bool debugDrawVelocities = false;          // Draw velocity vectors
    
    /**
     * Load configuration from JSON file
     * @param filepath Path to config file
     * @return true if loaded successfully
     */
    bool loadConfig(const std::string& filepath) {
        try {
            std::ifstream file(filepath);
            if (!file.is_open()) {
                std::cerr << "[SimulationConfig] Could not open config file: " << filepath << std::endl;
                return false;
            }
            
            nlohmann::json config;
            file >> config;
            
            if (config.contains("physics")) {
                auto& physics = config["physics"];
                
                // Timestep settings
                fixedDeltaTime = physics.value("fixed_delta_time", fixedDeltaTime);
                maxSubSteps = physics.value("max_substeps", maxSubSteps);
                maxAccumulatedTime = physics.value("max_accumulated_time", maxAccumulatedTime);
                
                // Gravity
                if (physics.contains("gravity") && physics["gravity"].is_array() && physics["gravity"].size() >= 3) {
                    gravityX = physics["gravity"][0];
                    gravityY = physics["gravity"][1];
                    gravityZ = physics["gravity"][2];
                }
                
                // Solver
                velocityIterations = physics.value("velocity_iterations", velocityIterations);
                positionIterations = physics.value("position_iterations", positionIterations);
                baumgarteFactor = physics.value("baumgarte_factor", baumgarteFactor);
                allowedPenetration = physics.value("allowed_penetration", allowedPenetration);
                restitutionThreshold = physics.value("restitution_threshold", restitutionThreshold);
                
                // Contact
                contactBreakingThreshold = physics.value("contact_breaking_threshold", contactBreakingThreshold);
                warmStartingFactor = physics.value("warm_starting_factor", warmStartingFactor);
                maxContactPoints = physics.value("max_contact_points", maxContactPoints);
                
                // Sleep
                enableSleeping = physics.value("enable_sleeping", enableSleeping);
                sleepLinearThreshold = physics.value("sleep_linear_threshold", sleepLinearThreshold);
                sleepAngularThreshold = physics.value("sleep_angular_threshold", sleepAngularThreshold);
                sleepTimeThreshold = physics.value("sleep_time_threshold", sleepTimeThreshold);
                
                // Damping defaults
                defaultLinearDamping = physics.value("default_linear_damping", defaultLinearDamping);
                defaultAngularDamping = physics.value("default_angular_damping", defaultAngularDamping);
                defaultFriction = physics.value("default_friction", defaultFriction);
                defaultRestitution = physics.value("default_restitution", defaultRestitution);
                
                // BVH
                fatAABBMargin = physics.value("fat_aabb_margin", fatAABBMargin);
                aabbPrediction = physics.value("aabb_prediction", aabbPrediction);
                
                // Debug
                enableDebugDraw = physics.value("enable_debug_draw", enableDebugDraw);
                verboseLogging = physics.value("verbose_logging", verboseLogging);
                debugDrawContacts = physics.value("debug_draw_contacts", debugDrawContacts);
                debugDrawBroadphase = physics.value("debug_draw_broadphase", debugDrawBroadphase);
                debugDrawVelocities = physics.value("debug_draw_velocities", debugDrawVelocities);
            }
            
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[SimulationConfig] Error loading config: " << e.what() << std::endl;
            return false;
        }
    }
    
    /**
     * Get gravity as a glm::vec3-compatible array
     */
    float* getGravity() {
        static float gravity[3];
        gravity[0] = gravityX;
        gravity[1] = gravityY;
        gravity[2] = gravityZ;
        return gravity;
    }
};
