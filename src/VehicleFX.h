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
* Test harness: NOX_VFX_TEST=1 emits fixed smoke/spray sources near the nfs_sprint spawn. When the game
* does not call Update(), VehicleFXPass ticks the harness itself (EnsureTestHarnessTick).
*/
namespace VehicleFX {

// One instance per live particle; three vec4s, read by shaders/vehicle_fx_vert.glsl.
struct RenderParticle {
	glm::vec4 posRadius;  // xyz world position (m), w billboard radius (m)
	glm::vec4 shading;    // x opacity [0,1], y roll angle (rad), z per-particle seed [0,1), w kind (0 smoke, 1 droplet, 2 mist)
	glm::vec4 velocity;   // xyz velocity (m/s), w normalised age [0,1]
};

// Emitters. intensity is clamped to [0,1]; dt is clamped to 50 ms.
void EmitSmoke(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt);
void EmitSpray(const glm::vec3& pos, const glm::vec3& baseVelocity, float intensity, float dt);

void Update(float dt);      // advance particles (called once per frame by the game)
void Clear();               // e.g. on teleport/scene change
void SetEnabled(bool on);   // disabling stops emission and hides existing particles
bool IsEnabled();
size_t ParticleCount();     // live particles

// Render-side access (VehicleFXPass). Rebuilt at the end of every Update().
const std::vector<RenderParticle>& GetRenderParticles();

// Drives the NOX_VFX_TEST emitters from the render pass when the game has not called Update() since the
// previous render. No-op unless NOX_VFX_TEST=1.
void EnsureTestHarnessTick(float dt);
bool TestHarnessActive();

} // namespace VehicleFX
