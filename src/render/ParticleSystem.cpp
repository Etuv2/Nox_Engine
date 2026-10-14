#include "ParticleSystem.h"

#include "../SceneNode.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>

namespace nox::render {

namespace {
constexpr float kMaxStep = 0.1f;
constexpr float kTwoPi = 6.28318530718f;
}

float RandomUnit(std::mt19937& rng) {
	return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

ParticleSystem::ParticleSystem(uint32_t seed) : m_rng(seed) {}

void ParticleSystem::Update(float dt, const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings) {
	if (!(dt > 0.0f)) return;
	dt = std::min(dt, kMaxStep);

	// Integrate first so particles spawned this frame start at age zero and move from the next step on.
	const glm::mat3 rotation = glm::mat3(emitterWorld);
	const float det = glm::determinant(rotation);
	const glm::vec3 localGravity = (settings.worldSpace || std::abs(det) < 1e-8f) ? settings.gravity
		: glm::inverse(rotation) * settings.gravity;
	size_t kept = 0;
	for (size_t i = 0; i < m_particles.size(); ++i) {
		Particle p = m_particles[i];
		p.age += dt;
		if (p.age >= p.lifetime) continue;
		p.velocity += localGravity * dt;
		p.position += p.velocity * dt;
		m_particles[kept++] = p;
	}
	m_particles.resize(kept);

	if (!settings.emitting) return;
	const float maxParticles = static_cast<float>(std::max(settings.maxParticles, 0));
	m_emitAccumulator += std::max(settings.rate, 0.0f) * dt;
	while (m_emitAccumulator >= 1.0f) {
		m_emitAccumulator -= 1.0f;
		if (static_cast<float>(m_particles.size()) >= maxParticles) {
			m_emitAccumulator = 0.0f;   // at the cap: emission is dropped, not queued
			break;
		}
		Spawn(emitterWorld, settings);
	}
}

void ParticleSystem::Burst(int count, const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings) {
	for (int i = 0; i < count; ++i) {
		if (static_cast<int>(m_particles.size()) >= std::max(settings.maxParticles, 0)) break;
		Spawn(emitterWorld, settings);
	}
}

void ParticleSystem::Restart() {
	m_particles.clear();
	m_emitAccumulator = 0.0f;
	m_totalSpawned = 0;
}

void ParticleSystem::Clear() {
	Restart();
}

void ParticleSystem::Spawn(const glm::mat4& emitterWorld, const ParticleEmitterSettings& settings) {
	// Direction: uniform over the spherical cap around +Y, half angle = spreadAngle / 2.
	const float halfAngle = glm::radians(glm::clamp(settings.spreadAngle, 0.0f, 180.0f)) * 0.5f;
	const float phi = RandomUnit(m_rng) * kTwoPi;
	const float theta = std::asin(std::sin(halfAngle) * RandomUnit(m_rng));
	const glm::vec3 axisLocal(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));

	Particle p;
	const float lo = std::min(settings.lifetimeMin, settings.lifetimeMax);
	const float hi = std::max(settings.lifetimeMin, settings.lifetimeMax);
	p.lifetime = std::max(0.001f, lo + (hi - lo) * RandomUnit(m_rng));
	p.seed = RandomUnit(m_rng);
	if (settings.worldSpace) {
		p.position = glm::vec3(emitterWorld[3]);
		const glm::mat3 rotation = glm::mat3(emitterWorld);
		glm::vec3 dir = rotation * axisLocal;
		const float len = glm::length(dir);
		dir = len > 1e-6f ? dir / len : axisLocal;   // degenerate (zero-scale) emitter: keep local frame
		p.velocity = dir * settings.startSpeed;
	} else {
		p.position = glm::vec3(0.0f);
		p.velocity = axisLocal * settings.startSpeed;
	}
	m_particles.push_back(p);
	++m_totalSpawned;
}

void UpdateSceneEmitters(SceneNode& root, float dt) {
	if (const std::shared_ptr<ParticleEmitterComponent> emitter = root.GetParticleEmitter()) {
		emitter->system.Update(dt, root.GetWorldPosition4x4(), emitter->settings);
	}
	for (const std::shared_ptr<SceneNode>& child : root.children) {
		if (child) UpdateSceneEmitters(*child, dt);
	}
}

} // namespace nox::render
