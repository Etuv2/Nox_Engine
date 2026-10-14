#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <iostream>
#include "json.hpp"
#include "assets/AssetIO.h"
#include <optional>

/**
 * SimulationConfig - Physics simulation configuration parameters
 *
 * Contains all tunable parameters for the physics engine including:
 * - Fixed timestep settings
 * - Solver iteration counts
 * - Gravity and damping
 * - Collision detection parameters
 * - Sleep thresholds
 * - World collision layer matrix
 *
 * One key table serves three readers: config/config.json "physics" (the defaults), the scene JSON
 * "physics" block (only the values that differ from those defaults are written), and the Lua
 * nox.physics API. Keys are the snake_case names used in config.json.
 */
struct SimulationConfig {
    static constexpr int kLayerCount = 32;
    using LayerMatrix = std::array<uint32_t, kLayerCount>;
    using LayerNames = std::array<std::string, kLayerCount>;

    static LayerMatrix AllLayersMatrix() {
        LayerMatrix m;
        m.fill(0xFFFFFFFFu);
        return m;
    }

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

    // Defaults for newly created bodies (SceneLoader colliders and Add Node use these; authored values win).
    // Restitution 0.3 is what a RigidBody starts with, so these defaults change nothing until edited.
    float defaultFriction = 0.5f;
    float defaultRestitution = 0.3f;

    // BVH/Broadphase parameters
    float fatAABBMargin = 0.1f;                // Fat AABB expansion margin
    float aabbPrediction = 0.0f;               // AABB expansion for velocity prediction

    // World collision layers. A body pair is tested only when its layer/mask filter passes (RigidBody) and
    // some layer of body A and some layer of body B are allowed by layerMatrix. Row i holds the layers layer i may
    // collide with; the default (every bit set) changes nothing. Names are editor labels only.
    LayerNames layerNames{};
    LayerMatrix layerMatrix = AllLayersMatrix();

    // Debug options (read from config.json; the editor's Physics window keeps its own view toggles in
    // PhysicsEngine::DebugView, so these are not part of the scene block)
    bool enableDebugDraw = false;
    bool verboseLogging = false;
    bool debugDrawContacts = false;            // Draw contact points and normals
    bool debugDrawBroadphase = false;          // Draw broadphase AABBs
    bool debugDrawVelocities = false;          // Draw velocity vectors

    bool LayerMatrixIsDefault() const {
        for (uint32_t row : layerMatrix) {
            if (row != 0xFFFFFFFFu) return false;
        }
        return true;
    }

    // True when some layer bit of a is allowed with some layer bit of b in both directions.
    bool LayersCollide(uint32_t a, uint32_t b) const {
        for (int i = 0; i < kLayerCount; ++i) {
            if (!(a & (1u << i))) continue;
            for (int j = 0; j < kLayerCount; ++j) {
                if ((b & (1u << j)) && (layerMatrix[i] & (1u << j)) && (layerMatrix[j] & (1u << i))) return true;
            }
        }
        return false;
    }

    /**
     * Load configuration from JSON file
     * @param filepath Path to config file
     * @return true if loaded successfully
     */
    bool loadConfig(const std::string& filepath) {
        const std::optional<std::string> text = AssetIO::ReadText(filepath);
        if (!text) {
            std::cerr << "[SimulationConfig] Could not open config file: " << filepath << std::endl;
            return false;
        }
        nlohmann::json config = nlohmann::json::parse(*text, nullptr, false);
        return loadConfigJson(config);
    }

    /**
     * Load configuration from an already parsed config document (the "physics" object).
     */
    bool loadConfigJson(const nlohmann::json& config) {
        try {
            if (!config.is_object()) {
                std::cerr << "[SimulationConfig] Config is not a JSON object" << std::endl;
                return false;
            }
            if (config.contains("physics")) {
                applyJson(config["physics"]);
            }

            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[SimulationConfig] Error loading config: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * Applies the keys present in a "physics" object (config.json or a scene block) on top of the current values.
     * layer_names and layer_matrix are replacement objects: rows not listed take their default. Values are clamped.
     * Throws nlohmann exceptions on wrong JSON types; callers catch.
     */
    void applyJson(const nlohmann::json& physics) {
        if (!physics.is_object()) return;
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

        // Defaults for new bodies
        defaultFriction = physics.value("default_friction", defaultFriction);
        defaultRestitution = physics.value("default_restitution", defaultRestitution);

        // BVH
        fatAABBMargin = physics.value("fat_aabb_margin", fatAABBMargin);
        aabbPrediction = physics.value("aabb_prediction", aabbPrediction);

        // Collision layers (replacement semantics)
        if (physics.contains("layer_names") && physics["layer_names"].is_object()) {
            layerNames = LayerNames{};
            for (auto it = physics["layer_names"].begin(); it != physics["layer_names"].end(); ++it) {
                const int index = std::atoi(it.key().c_str());
                if (index >= 0 && index < kLayerCount && it.value().is_string()) layerNames[index] = it.value().get<std::string>();
            }
        }
        if (physics.contains("layer_matrix") && physics["layer_matrix"].is_object()) {
            layerMatrix = AllLayersMatrix();
            for (auto it = physics["layer_matrix"].begin(); it != physics["layer_matrix"].end(); ++it) {
                const int index = std::atoi(it.key().c_str());
                if (index >= 0 && index < kLayerCount && it.value().is_number_integer() && it.value().get<int64_t>() >= 0) {
                    layerMatrix[index] = it.value().get<uint32_t>();
                }
            }
        }

        // Debug
        enableDebugDraw = physics.value("enable_debug_draw", enableDebugDraw);
        verboseLogging = physics.value("verbose_logging", verboseLogging);
        debugDrawContacts = physics.value("debug_draw_contacts", debugDrawContacts);
        debugDrawBroadphase = physics.value("debug_draw_broadphase", debugDrawBroadphase);
        debugDrawVelocities = physics.value("debug_draw_velocities", debugDrawVelocities);
        Sanitize();
    }

    /**
     * Clamps every tunable into the range the solver supports. NaN falls back to the lower bound.
     */
    void Sanitize() {
        auto clampF = [](float v, float lo, float hi) { return std::isnan(v) ? lo : std::min(std::max(v, lo), hi); };
        auto clampI = [](int v, int lo, int hi) { return std::min(std::max(v, lo), hi); };
        fixedDeltaTime = clampF(fixedDeltaTime, 0.001f, 0.1f);
        maxSubSteps = clampI(maxSubSteps, 1, 64);
        maxAccumulatedTime = clampF(maxAccumulatedTime, fixedDeltaTime, 10.0f);
        gravityX = clampF(gravityX, -1000.0f, 1000.0f);
        gravityY = clampF(gravityY, -1000.0f, 1000.0f);
        gravityZ = clampF(gravityZ, -1000.0f, 1000.0f);
        velocityIterations = clampI(velocityIterations, 1, 100);
        positionIterations = clampI(positionIterations, 0, 100);
        baumgarteFactor = clampF(baumgarteFactor, 0.0f, 1.0f);
        allowedPenetration = clampF(allowedPenetration, 0.0f, 1.0f);
        restitutionThreshold = clampF(restitutionThreshold, 0.0f, 1000.0f);
        contactBreakingThreshold = clampF(contactBreakingThreshold, 0.0f, 1.0f);
        warmStartingFactor = clampF(warmStartingFactor, 0.0f, 1.0f);
        maxContactPoints = clampI(maxContactPoints, 1, 4);
        sleepLinearThreshold = clampF(sleepLinearThreshold, 0.0f, 100.0f);
        sleepAngularThreshold = clampF(sleepAngularThreshold, 0.0f, 100.0f);
        sleepTimeThreshold = clampF(sleepTimeThreshold, 0.0f, 1000.0f);
        defaultFriction = clampF(defaultFriction, 0.0f, 1.0f);
        defaultRestitution = clampF(defaultRestitution, 0.0f, 1.0f);
        fatAABBMargin = clampF(fatAABBMargin, 0.0f, 10.0f);
        aabbPrediction = clampF(aabbPrediction, 0.0f, 10.0f);
    }

    /**
     * Every tuning key with its current value (no debug flags). Layer objects list only the non-default entries.
     */
    nlohmann::json ToJson() const {
        nlohmann::json j = nlohmann::json::object();
        j["fixed_delta_time"] = FloatJson(fixedDeltaTime);
        j["max_substeps"] = maxSubSteps;
        j["max_accumulated_time"] = FloatJson(maxAccumulatedTime);
        j["gravity"] = nlohmann::json::array({ FloatJson(gravityX), FloatJson(gravityY), FloatJson(gravityZ) });
        j["velocity_iterations"] = velocityIterations;
        j["position_iterations"] = positionIterations;
        j["baumgarte_factor"] = FloatJson(baumgarteFactor);
        j["allowed_penetration"] = FloatJson(allowedPenetration);
        j["restitution_threshold"] = FloatJson(restitutionThreshold);
        j["contact_breaking_threshold"] = FloatJson(contactBreakingThreshold);
        j["warm_starting_factor"] = FloatJson(warmStartingFactor);
        j["max_contact_points"] = maxContactPoints;
        j["enable_sleeping"] = enableSleeping;
        j["sleep_linear_threshold"] = FloatJson(sleepLinearThreshold);
        j["sleep_angular_threshold"] = FloatJson(sleepAngularThreshold);
        j["sleep_time_threshold"] = FloatJson(sleepTimeThreshold);
        j["default_friction"] = FloatJson(defaultFriction);
        j["default_restitution"] = FloatJson(defaultRestitution);
        j["fat_aabb_margin"] = FloatJson(fatAABBMargin);
        j["aabb_prediction"] = FloatJson(aabbPrediction);
        nlohmann::json names = nlohmann::json::object();
        for (int i = 0; i < kLayerCount; ++i) {
            if (!layerNames[i].empty()) names[std::to_string(i)] = layerNames[i];
        }
        j["layer_names"] = names;
        nlohmann::json matrix = nlohmann::json::object();
        for (int i = 0; i < kLayerCount; ++i) {
            if (layerMatrix[i] != 0xFFFFFFFFu) matrix[std::to_string(i)] = layerMatrix[i];
        }
        j["layer_matrix"] = matrix;
        return j;
    }

    /**
     * The scene "physics" block: only the keys whose values differ from defaults. Empty when nothing differs, in
     * which case the block is not written.
     */
    nlohmann::json ToSceneJson(const SimulationConfig& defaults) const {
        const nlohmann::json current = ToJson();
        const nlohmann::json base = defaults.ToJson();
        nlohmann::json out = nlohmann::json::object();
        for (auto it = current.begin(); it != current.end(); ++it) {
            if (!base.contains(it.key()) || base[it.key()] != it.value()) out[it.key()] = it.value();
        }
        return out;
    }

    /**
     * Shortest decimal that reads back as the same float, so values survive a save and load unchanged.
     */
    static nlohmann::json FloatJson(float value) {
        for (int digits = 6; digits <= 9; ++digits) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.*g", digits, static_cast<double>(value));
            const double parsed = std::strtod(buf, nullptr);
            if (static_cast<float>(parsed) == value) return nlohmann::json(parsed);
        }
        return nlohmann::json(static_cast<double>(value));
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
