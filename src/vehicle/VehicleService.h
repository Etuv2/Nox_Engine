#pragma once
// Native vehicle backend (NoxEngineCore): independent instances, each with its own state, policy
// binding and fixed-step accumulator. See VehiclePolicy.h for the phase order.
//
// Native (this service): tyre curves and friction ellipse, load transfer, drive/brake force
// evaluation, integration, collision circles against walls/areas/obstacles/surface, suspension,
// wheel sampling, rig posing and the mechanical gearbox (ratios, final drive, rpm, limiter).
// Policy (IVehiclePolicy): direction and gear choice, shift timing, nitrous, drift, steering and
// yaw assists, velocity correction and the drift swing. The service never chooses those itself.
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "VehiclePolicy.h"
#include "VehicleTypes.h"

// One observation per fixed sub-step, taken right after the step (before suspension update).
struct VehicleSubstepRecord {
	float h = 0.0f;
	VehicleInput in;
	VehicleState state;
	VehiclePolicyDebug policy;
};

class VehicleInstance;

class VehicleService {
public:
	using SubstepObserver = std::function<void(const VehicleSubstepRecord&)>;

	static constexpr float kFixedStep = 1.0f / 240.0f; // vehicle sub-step (s)
	static constexpr float kMaxFrameDt = 0.05f;        // frame dt clamp (s)
	static constexpr float kMaxAccumulator = 0.1f;     // accumulated sub-step debt cap (s)

	VehicleService();
	~VehicleService();
	VehicleService(const VehicleService&) = delete;
	VehicleService& operator=(const VehicleService&) = delete;

	// Creates an instance. The policy is not owned and must outlive the instance. Returns an invalid
	// handle and writes the reason to *error when the definition is not supported.
	VehicleHandle Create(const VehicleDefinition& def, IVehiclePolicy* policy, std::string* error = nullptr);
	// Takes over every instance of `other` with its handles (ids and incarnations unchanged). The instances held here
	// are destroyed; `other` is left empty. Policies are not touched: the caller rebinds them with SetPolicy.
	void MoveFrom(VehicleService& other);
	// Stale or invalid handles are ignored everywhere in this class.
	void Destroy(VehicleHandle h);
	bool IsValid(VehicleHandle h) const;
	size_t Count() const;

	// Replaces the numeric tuning (structure checks only: gear count and ratios, mass, top speed).
	bool SetTuning(VehicleHandle h, const VehicleTuning& tuning, std::string* error = nullptr);
	void SetSpawn(VehicleHandle h, const glm::vec2& pos, float heading);
	// Moves the centre without touching the motion state (activation at the authored root pose).
	void SetPlanarPosition(VehicleHandle h, const glm::vec2& pos);
	void SetSubstepObserver(VehicleHandle h, SubstepObserver observer);
	// Replaces the policy of an instance (not owned; the old policy is no longer called). Takes effect at the
	// next sub-step. Null and invalid handles are ignored.
	void SetPolicy(VehicleHandle h, IVehiclePolicy* policy);

	// Frame update: clamps dt, decays the impact, then either runs fixed sub-steps or holds the car
	// (frozen: no motion, engine revs), applies pending recovery and poses the rig.
	bool Advance(VehicleHandle h, float dt, const VehicleInput& in, bool frozen);
	void Teleport(VehicleHandle h, const glm::vec3& pos, float heading);
	void Teleport(VehicleHandle h, const glm::vec2& pos, float heading);
	void ResetToSpawn(VehicleHandle h);
	void RecoverToRoad(VehicleHandle h);
	// One-shot flags set by teleports (read and cleared).
	bool TakeTeleported(VehicleHandle h);
	bool TakeTeleportFx(VehicleHandle h);

	// Read-only snapshot. Invalid or stale handles return a default state.
	VehicleState State(VehicleHandle h) const;
	float GroundHeight(VehicleHandle h, const glm::vec2& p) const;
	bool SurfaceHeight(VehicleHandle h, const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal = nullptr) const;
	// Pushes a planar point out of the instance's walls, areas and obstacles (camera collision).
	glm::vec2 ResolveCircle(VehicleHandle h, glm::vec2& c, float radius) const;

private:
	struct Slot {
		uint32_t incarnation = 0;
		std::unique_ptr<VehicleInstance> instance;
	};
	VehicleInstance* Find(VehicleHandle h);
	const VehicleInstance* Find(VehicleHandle h) const;
	std::vector<Slot> m_slots;
	size_t m_live = 0;
};
