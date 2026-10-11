#include "VehicleFX.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace VehicleFX {
namespace {

constexpr size_t kMaxParticles = 4096;
constexpr size_t kSlotScanLimit = 64;     // bounded search for a free slot before overwriting
constexpr float kMaxStep = 0.05f;         // seconds; caps integration and emission after hitches
constexpr float kTwoPi = 6.28318530718f;

constexpr float kSmokeRate = 55.0f;       // particles/s at intensity 1, per call
constexpr float kSprayRate = 260.0f;      // particles/s at intensity 1, per call
constexpr float kMistShare = 0.3f;        // fraction of spray particles that are floating mist

enum class Kind : uint8_t { Smoke = 0, Droplet = 1, Mist = 2 };

struct Particle {
	glm::vec3 pos{ 0.0f };
	glm::vec3 vel{ 0.0f };
	float age = 0.0f;
	float life = 1.0f;
	float radius0 = 0.1f;
	float radius1 = 0.1f;
	float opacity = 0.0f;   // peak opacity, already scaled by emitter intensity
	float roll = 0.0f;
	float rollRate = 0.0f;
	float seed = 0.0f;
	float floorY = -1e30f;  // ground under the emitter
	Kind kind = Kind::Smoke;
	bool alive = false;
};

std::vector<Particle> g_particles(kMaxParticles);
std::vector<RenderParticle> g_render;
size_t g_cursor = 0;
size_t g_aliveCount = 0;
bool g_enabled = true;
float g_time = 0.0f;
uint32_t g_rng = 0x9E3779B9u;
uint64_t g_updateCount = 0;
uint64_t g_harnessSeenUpdates = 0;

uint32_t NextU32()
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

float Rand01()
{
	return static_cast<float>(NextU32() & 0xFFFFFFu) / 16777216.0f;
}

float RandRange(float a, float b)
{
	return a + (b - a) * Rand01();
}

float Smoothstep(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Expected count rounded stochastically so the mean matches rate * dt at any frame rate.
int StochasticCount(float expected)
{
	if (expected <= 0.0f) {
		return 0;
	}
	int n = static_cast<int>(expected);
	if (Rand01() < expected - static_cast<float>(n)) {
		++n;
	}
	return n;
}

Particle& AllocateSlot()
{
	for (size_t i = 0; i < kSlotScanLimit; ++i) {
		const size_t idx = (g_cursor + i) % kMaxParticles;
		if (!g_particles[idx].alive) {
			g_cursor = (idx + 1) % kMaxParticles;
			return g_particles[idx];
		}
	}
	// Pool is saturated: recycle the cursor slot.
	const size_t idx = g_cursor;
	g_cursor = (g_cursor + 1) % kMaxParticles;
	return g_particles[idx];
}

void SpawnSmoke(const glm::vec3& pos, const glm::vec3& baseVel, float intensity, float dt, float floorY)
{
	const int count = StochasticCount(kSmokeRate * intensity * dt);
	for (int i = 0; i < count; ++i) {
		Particle& p = AllocateSlot();
		const float a = RandRange(0.0f, kTwoPi);
		const float r = std::sqrt(Rand01()) * 0.22f;
		p.pos = pos + glm::vec3(std::cos(a) * r, RandRange(0.02f, 0.12f), std::sin(a) * r);
		// Smoke keeps part of the car's motion, then buoyancy and drag take over.
		p.vel = baseVel * 0.4f + glm::vec3(RandRange(-0.6f, 0.6f), RandRange(0.5f, 1.2f), RandRange(-0.6f, 0.6f));
		p.age = 0.0f;
		p.life = RandRange(1.4f, 2.4f);
		p.radius0 = RandRange(0.25f, 0.4f);
		p.radius1 = RandRange(1.0f, 1.6f);
		p.opacity = 0.42f * intensity * RandRange(0.6f, 1.0f);
		p.roll = RandRange(0.0f, kTwoPi);
		p.rollRate = RandRange(-0.6f, 0.6f);
		p.seed = Rand01();
		p.floorY = floorY;
		p.kind = Kind::Smoke;
		p.alive = true;
	}
}

void SpawnSpray(const glm::vec3& pos, const glm::vec3& baseVel, float intensity, float dt, float floorY)
{
	const int count = StochasticCount(kSprayRate * intensity * dt);
	for (int i = 0; i < count; ++i) {
		Particle& p = AllocateSlot();
		const float a = RandRange(0.0f, kTwoPi);
		const float spread = RandRange(0.5f, 2.0f);
		p.pos = pos + glm::vec3(std::cos(a) * RandRange(0.0f, 0.3f), RandRange(0.02f, 0.1f), std::sin(a) * RandRange(0.0f, 0.3f));
		p.vel = baseVel * 0.7f + glm::vec3(std::cos(a) * spread, RandRange(1.5f, 4.0f), std::sin(a) * spread);
		p.age = 0.0f;
		p.roll = RandRange(0.0f, kTwoPi);
		p.rollRate = RandRange(-4.0f, 4.0f);
		p.seed = Rand01();
		p.floorY = floorY;
		if (Rand01() < kMistShare) {
			p.kind = Kind::Mist;
			p.life = RandRange(1.0f, 1.6f);
			p.radius0 = 0.25f;
			p.radius1 = RandRange(0.5f, 0.7f);
			p.opacity = 0.22f * intensity * RandRange(0.7f, 1.0f);
			p.vel.y = std::min(p.vel.y, 0.8f);
		} else {
			p.kind = Kind::Droplet;
			p.life = RandRange(0.35f, 0.8f);
			p.radius0 = RandRange(0.035f, 0.06f);
			p.radius1 = p.radius0;
			p.opacity = 0.8f * intensity * RandRange(0.5f, 1.0f);
		}
		p.alive = true;
	}
}

void Integrate(Particle& p, float dt)
{
	p.age += dt;
	if (p.age >= p.life) {
		p.alive = false;
		return;
	}
	switch (p.kind) {
	case Kind::Smoke:
		p.vel.y += 0.8f * dt;                      // buoyancy
		p.vel *= std::exp(-1.2f * dt);             // drag towards still air
		break;
	case Kind::Droplet:
		p.vel.y -= 9.81f * dt;                     // gravity
		p.vel *= std::exp(-0.25f * dt);
		break;
	case Kind::Mist:
		p.vel.y += 0.15f * dt;
		p.vel *= std::exp(-1.5f * dt);
		break;
	}
	p.pos += p.vel * dt;
	p.roll += p.rollRate * dt;
	// Droplets that fall back onto the ground are gone; mist and smoke never sink below it.
	if (p.kind == Kind::Droplet && p.pos.y < p.floorY) {
		p.alive = false;
		return;
	}
	p.pos.y = std::max(p.pos.y, p.floorY);
}

void BuildRenderList()
{
	g_render.clear();
	g_aliveCount = 0;
	if (!g_enabled) {
		return;
	}
	g_render.reserve(kMaxParticles);
	for (const Particle& p : g_particles) {
		if (!p.alive) {
			continue;
		}
		++g_aliveCount;
		const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
		float radius = p.radius0;
		float fade = 1.0f;
		switch (p.kind) {
		case Kind::Smoke:
			// Puffs expand quickly at first, then slowly; they fade in fast and thin out late.
			radius = p.radius0 + (p.radius1 - p.radius0) * std::sqrt(t);
			fade = Smoothstep(0.0f, 0.1f, t) * (1.0f - Smoothstep(0.35f, 1.0f, t));
			break;
		case Kind::Droplet:
			fade = Smoothstep(0.0f, 0.05f, t) * (1.0f - t);
			break;
		case Kind::Mist:
			radius = p.radius0 + (p.radius1 - p.radius0) * t;
			fade = Smoothstep(0.0f, 0.2f, t) * (1.0f - Smoothstep(0.5f, 1.0f, t));
			break;
		}
		const float opacity = p.opacity * fade;
		if (opacity <= 0.002f) {
			continue;
		}
		RenderParticle r;
		r.posRadius = glm::vec4(p.pos, radius);
		r.shading = glm::vec4(opacity, p.roll, p.seed, static_cast<float>(p.kind));
		r.velocity = glm::vec4(p.vel, t);
		g_render.push_back(r);
	}
}

// Test harness: the sources supplied by the game module (SetTestEmitters).
std::vector<TestEmitter> g_testEmitters;

void RunTestEmitters(float dt)
{
	const float wobble = 0.5f + 0.5f * std::sin(g_time * 1.7f);
	for (const TestEmitter& e : g_testEmitters) {
		const float intensity = e.intensity + e.wobble * wobble;
		if (e.spray) {
			EmitSpray(e.pos, e.velocity, intensity, dt, e.floorY);
		}
		else {
			EmitSmoke(e.pos, e.velocity, intensity, dt, e.floorY);
		}
	}
}

} // namespace

void EmitSmoke(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt, float floorY)
{
	if (!g_enabled) {
		return;
	}
	intensity = std::clamp(intensity, 0.0f, 1.0f);
	dt = std::clamp(dt, 0.0f, kMaxStep);
	SpawnSmoke(pos, baseVelocity, intensity, dt, floorY);
}

void EmitSpray(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt, float floorY)
{
	if (!g_enabled) {
		return;
	}
	intensity = std::clamp(intensity, 0.0f, 1.0f);
	dt = std::clamp(dt, 0.0f, kMaxStep);
	SpawnSpray(pos, baseVelocity, intensity, dt, floorY);
}

void Update(float dt)
{
	++g_updateCount;
	dt = std::clamp(dt, 0.0f, kMaxStep);
	g_time += dt;
	if (g_enabled && TestHarnessActive()) {
		RunTestEmitters(dt);
	}
	for (Particle& p : g_particles) {
		if (p.alive) {
			Integrate(p, dt);
		}
	}
	BuildRenderList();
}

void Clear()
{
	for (Particle& p : g_particles) {
		p.alive = false;
	}
	g_render.clear();
	g_aliveCount = 0;
}

void SetEnabled(bool on)
{
	g_enabled = on;
	if (!on) {
		Clear();
	}
}

bool IsEnabled()
{
	return g_enabled;
}

size_t ParticleCount()
{
	return g_aliveCount;
}

const std::vector<RenderParticle>& GetRenderParticles()
{
	return g_render;
}

void SetTestEmitters(const std::vector<TestEmitter>& emitters)
{
	g_testEmitters = emitters;
}

bool TestHarnessActive()
{
	static const bool active = [] {
		const char* v = std::getenv("NOX_VFX_TEST");
		return v && v[0] == '1';
	}();
	return active;
}

void EnsureTestHarnessTick(float dt)
{
	if (!TestHarnessActive()) {
		return;
	}
	// The game is ticking Update() itself: nothing to do. Otherwise tick once per render frame.
	if (g_updateCount == g_harnessSeenUpdates) {
		Update(dt);
	}
	g_harnessSeenUpdates = g_updateCount;
}

} // namespace VehicleFX
