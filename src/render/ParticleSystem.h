#pragma once
// Generic CPU particle system behind the ParticleEmitter component (src/reflect/components/RenderComponents
// descriptors, drawn by ParticlePass). No scene or game knowledge: an emitter is a settings struct plus a
// list of particles that a transform spawns and integrates.
//
// Simulation: emission accumulates rate * dt and spawns whole particles (no drift at low frame rates).
// Each particle has a lifetime drawn from [lifetimeMin, lifetimeMax], a velocity inside a cone around the
// emitter's local +Y (spreadAngle is the full cone angle), and gravity. World-space emitters keep particles
// where they were spawned; local-space emitters keep them attached to the moving emitter.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <glm/glm.hpp>

class SceneNode;

namespace nox::render {

enum class ParticleBlend : uint8_t { Alpha = 0, Additive = 1 };

struct ParticleEmitterSettings {
	bool emitting = true;
	float rate = 20.0f;              // particles per second
	float lifetimeMin = 1.0f;        // seconds
	float lifetimeMax = 2.0f;        // seconds
	float startSpeed = 1.0f;         // m/s along the cone axis
	float spreadAngle = 30.0f;       // degrees, full cone angle (0 = straight up, 180 = hemisphere)
	glm::vec3 gravity{ 0.0f };       // m/s^2, world units
	float startSize = 0.2f;          // billboard radius (m) at birth
	float endSize = 0.6f;            // billboard radius (m) at death
	glm::vec4 startColor{ 1.0f };    // rgb + alpha at birth (straight alpha)
	glm::vec4 endColor{ 1.0f, 1.0f, 1.0f, 0.0f };
	std::string texture;             // logical image path; empty = soft disc
	int maxParticles = 256;
	bool worldSpace = false;
	ParticleBlend blend = ParticleBlend::Alpha;
};

struct Particle {
	glm::vec3 position{ 0.0f };      // world (world-space emitters) or emitter-local
	glm::vec3 velocity{ 0.0f };      // same space as position
	float age = 0.0f;                // seconds
	float lifetime = 1.0f;           // seconds
	float seed = 0.0f;               // [0,1) per-particle random value (roll, texture choice)
};

class ParticleSystem {
public:
	explicit ParticleSystem(uint32_t seed = 1u);

	// Advances the simulation by dt (clamped to 0.1 s so a hitch does not flood the emitter) and emits
	// according to settings.emitting. emitterWorld places the emitter this frame.
	void Update(float dt, const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings);
	// Spawns count particles now (ignores settings.emitting, respects maxParticles).
	void Burst(int count, const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings);
	// Removes every particle and the emission remainder; emission continues when enabled.
	void Restart();
	void Clear();

	const std::vector<Particle>& Particles() const { return m_particles; }
	size_t Count() const { return m_particles.size(); }
	// Particles spawned since construction or the last Restart (emission tests and diagnostics).
	uint64_t TotalSpawned() const { return m_totalSpawned; }

private:
	void Spawn(const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings);

	std::vector<Particle> m_particles;
	std::mt19937 m_rng;
	float m_emitAccumulator = 0.0f;
	uint64_t m_totalSpawned = 0;
};

// Scene-side state of a ParticleEmitter component (owned by its SceneNode).
struct ParticleEmitterComponent {
	ParticleEmitterSettings settings;
	ParticleSystem system;
};

// Helpers shared by the component descriptors and ParticlePass.
float RandomUnit(std::mt19937& rng);   // [0,1)

// Advances every ParticleEmitter in the subtree by dt, placed at its node's world transform. Core calls this
// once per frame; ParticlePass only reads the particles.
void UpdateSceneEmitters(SceneNode& root, float dt);

} // namespace nox::render
