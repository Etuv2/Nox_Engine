#pragma once
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

/*
* Vehicle particle effects: tyre smoke (burnouts, wheelspin, drifting) and wet-road spray/mist.
*
* Gameplay calls EmitSmoke / EmitSpray once per wheel per frame with world-space positions. 'amount'
* is rate-independent: the expected particle count is rate * intensity * dt (stochastically rounded),
* so calling with a smaller dt spawns proportionally fewer particles. Update(dt) integrates the pool
* once per frame and rebuilds the GPU-facing list read by VehicleFXPass (src/passes/VehicleFXPass.*).
*
* Test harness: NOX_VFX_TEST=1 emits the sources given to SetTestEmitters (the game module supplies
* positions; none are set by default). When the game
* does not call Update(), VehicleFXPass ticks the harness itself (EnsureTestHarnessTick).
*/
namespace VehicleFX {

// One instance per live particle; three vec4s, read by shaders/vehicle_fx_vert.glsl.
struct RenderParticle {
	glm::vec4 posRadius;  // xyz world position (m), w billboard radius (m)
	glm::vec4 shading;    // x opacity [0,1], y roll angle (rad), z per-particle seed [0,1), w kind (0 smoke, 1 droplet, 2 mist)
	glm::vec4 velocity;   // xyz velocity (m/s), w normalised age [0,1]
};

// Emitters. intensity is clamped to [0,1]; dt is clamped to 50 ms. floorY is the ground height under
// the emitter: droplets that fall below it disappear and smoke/mist stay above it (no floor by default).
void EmitSmoke(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt, float floorY = -1e30f);
void EmitSpray(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt, float floorY = -1e30f);

void Update(float dt);      // advance particles (called once per frame by the game)
void Clear();               // e.g. on teleport/scene change
void SetEnabled(bool on);   // disabling stops emission and hides existing particles
bool IsEnabled();
size_t ParticleCount();     // live particles

// Test-harness sources (used only while NOX_VFX_TEST=1). Positions are world space (m).
struct TestEmitter {
	bool spray = false;        // false: tyre smoke, true: wet-road spray
	glm::vec3 pos{ 0.0f };
	glm::vec3 velocity{ 0.0f }; // base velocity (m/s)
	float intensity = 1.0f;     // 0..1
	float wobble = 0.0f;        // added as wobble * (0.5 + 0.5 sin(1.7 t)), so intensity oscillates
	float floorY = -1e30f;      // ground height under the source (see EmitSmoke)
};
void SetTestEmitters(const std::vector<TestEmitter>& emitters);

// Render-side access (VehicleFXPass). Rebuilt at the end of every Update().
const std::vector<RenderParticle>& GetRenderParticles();

// Drives the NOX_VFX_TEST emitters from the render pass when the game has not called Update() since the
// previous render. No-op unless NOX_VFX_TEST=1.
void EnsureTestHarnessTick(float dt);
bool TestHarnessActive();

} // namespace VehicleFX
