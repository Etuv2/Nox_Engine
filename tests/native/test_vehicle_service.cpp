// VehicleService: independent instances, stale handles, definition rejection, fixed-step accumulator
// and phase order. Headless: flat planar mode (no roads, collision, areas or rig nodes).
#include "TestFramework.h"
#include "vehicle/VehicleService.h"

#include <cmath>
#include <string>
#include <vector>

namespace {
	// Scripted policy: fixed actuation, records the call order. Holds no state shared with other instances.
	class ScriptedPolicy final : public IVehiclePolicy {
	public:
		explicit ScriptedPolicy(float steer = 0.0f) : m_steer(steer) {}
		std::string calls;      // one letter per phase call: C, Y, V, P, T (teleport), R (respawn), H (hold)
		int controls = 0;
		void OnTeleport() override { calls += 'T'; }
		void OnRespawn() override { calls += 'R'; }
		void OnHold() override { calls += 'H'; }
		void Control(const VehicleInput& in, const VehicleStepTelemetry&, VehicleActuation& out) override {
			calls += 'C';
			++controls;
			out.gear = 1;
			out.steerAngle = m_steer;
			// Plain pedal mapping (the backend applies no pedal policy of its own).
			out.driveThrottle = in.throttle;
			out.brakeCommand = in.brake;
			out.rearLock = in.handbrake;
			out.holdAtRest = in.throttle == 0.0f && in.brake == 0.0f;
		}
		void YawAssist(const VehicleInput&, const VehicleStepTelemetry&, VehicleYawAssist&) override { calls += 'Y'; }
		void VelocityCorrection(const VehicleInput&, const VehicleStepTelemetry&, VehicleVelocity&) override { calls += 'V'; }
		void PostStep(const VehicleInput&, const VehicleStepTelemetry&, VehicleGearRequest& io) override {
			calls += 'P';
			io.gear = 1;
			io.gearboxShifting = false;
		}
		VehiclePolicyDebug Debug() const override { return VehiclePolicyDebug(); }
	private:
		float m_steer;
	};

	VehicleDefinition FlatCar(const std::string& name, float mass, float driveAccel) {
		VehicleDefinition d;
		d.name = name;
		d.tuning.mass = mass;
		d.tuning.driveAcceleration = driveAccel;
		d.tuning.topSpeedKmh = 150.0f;
		d.tuning.bumpAmplitude = 0.0f;          // flat plane
		d.tuning.gearRatios = { 3.0f, 2.0f, 1.5f, 1.2f, 1.0f };
		d.spawnPos = glm::vec2(0.0f, 0.0f);
		d.spawnHeading = 0.0f;                  // heading 0 = +Z
		return d;
	}

	VehicleInput Throttle(float t) {
		VehicleInput in;
		in.throttle = t;
		return in;
	}

	bool SameState(const VehicleState& a, const VehicleState& b) {
		if (a.pos != b.pos || a.vel != b.vel || a.heading != b.heading || a.vx != b.vx || a.vy != b.vy) return false;
		if (a.yawRate != b.yawRate || a.rpm != b.rpm || a.gear != b.gear || a.time != b.time) return false;
		for (int i = 0; i < 4; ++i) {
			if (a.wheels[i].contact != b.wheels[i].contact || a.wheels[i].slip != b.wheels[i].slip) return false;
		}
		return true;
	}
}

NOX_TEST(Vehicle, InstancesAreIndependent) {
	// Two distinct definitions in one service, each with its own policy.
	VehicleService service;
	ScriptedPolicy policyA, policyB;
	VehicleHandle a = service.Create(FlatCar("light", 1000.0f, 6.0f), &policyA);
	VehicleHandle b = service.Create(FlatCar("heavy", 1800.0f, 2.5f), &policyB);
	NOX_REQUIRE(a.Valid() && b.Valid());
	NOX_CHECK(a != b);
	NOX_CHECK_EQ(service.Count(), size_t(2));

	// Drive only A: B must not move.
	const VehicleState bBefore = service.State(b);
	for (int i = 0; i < 60; ++i) service.Advance(a, 1.0f / 60.0f, Throttle(1.0f), false);
	const VehicleState aDriven = service.State(a);
	NOX_CHECK(aDriven.pos.y > 1.0f);                 // light car accelerated along +Z
	NOX_CHECK(SameState(service.State(b), bBefore));

	// Drive B with the same input: it is slower than A (distinct mass and drive capacity).
	for (int i = 0; i < 60; ++i) service.Advance(b, 1.0f / 60.0f, Throttle(1.0f), false);
	NOX_CHECK(service.State(b).pos.y < aDriven.pos.y);
	NOX_CHECK(service.State(b).pos.y > 0.0f);

	// No shared state: B alone in a fresh service matches the B run above bit for bit.
	VehicleService solo;
	ScriptedPolicy soloPolicy;
	VehicleHandle s = solo.Create(FlatCar("heavy", 1800.0f, 2.5f), &soloPolicy);
	for (int i = 0; i < 60; ++i) solo.Advance(s, 1.0f / 60.0f, Throttle(1.0f), false);
	NOX_CHECK(SameState(solo.State(s), service.State(b)));
	NOX_CHECK(policyA.controls > 0 && policyB.controls > 0);
}

NOX_TEST(Vehicle, StaleHandleAfterDestroyIsRejected) {
	VehicleService service;
	ScriptedPolicy policy;
	VehicleHandle h = service.Create(FlatCar("car", 1000.0f, 5.0f), &policy);
	NOX_REQUIRE(h.Valid());
	service.Destroy(h);
	NOX_CHECK(!service.IsValid(h));
	NOX_CHECK(!service.Advance(h, 0.1f, Throttle(1.0f), false));
	NOX_CHECK(!service.TakeTeleported(h));
	NOX_CHECK_EQ(service.State(h).gear, VehicleState().gear);
	NOX_CHECK_EQ(service.Count(), size_t(0));
	service.Destroy(h);                              // double destroy is harmless
	NOX_CHECK_EQ(service.Count(), size_t(0));

	// The slot is reused under a new incarnation: the old handle must not resolve to it.
	ScriptedPolicy policy2;
	VehicleHandle h2 = service.Create(FlatCar("car2", 1000.0f, 5.0f), &policy2);
	NOX_REQUIRE(h2.Valid());
	NOX_CHECK_EQ(h2.id, h.id);
	NOX_CHECK(h2.incarnation != h.incarnation);
	NOX_CHECK(!service.IsValid(h));
	NOX_CHECK(service.IsValid(h2));
	NOX_CHECK(!service.Advance(h, 0.1f, Throttle(1.0f), false));
	NOX_CHECK(policy2.controls == 0);
}

NOX_TEST(Vehicle, UnsupportedDefinitionsAreRejected) {
	VehicleService service;
	ScriptedPolicy policy;
	std::string error;

	VehicleDefinition wrongBackend = FlatCar("four_axle", 1000.0f, 5.0f);
	wrongBackend.backend = "four_axle_articulated";
	NOX_CHECK(!service.Create(wrongBackend, &policy, &error).Valid());
	NOX_CHECK(error.find("four_axle") != std::string::npos);
	NOX_CHECK(error.find("unsupported backend") != std::string::npos);

	VehicleDefinition frontDrive = FlatCar("fwd", 1000.0f, 5.0f);
	frontDrive.drive = "front";
	NOX_CHECK(!service.Create(frontDrive, &policy, &error).Valid());
	NOX_CHECK(error.find("unsupported drive") != std::string::npos);

	VehicleDefinition allWheel = FlatCar("awd", 1000.0f, 5.0f);
	allWheel.drive = "all";
	NOX_CHECK(!service.Create(allWheel, &policy, &error).Valid());

	VehicleDefinition noPolicy = FlatCar("nopolicy", 1000.0f, 5.0f);
	NOX_CHECK(!service.Create(noPolicy, nullptr, &error).Valid());
	NOX_CHECK(error.find("policy") != std::string::npos);

	VehicleDefinition badRadius = FlatCar("radius", 1000.0f, 5.0f);
	badRadius.wheelRadius[2] = -0.3f;
	NOX_CHECK(!service.Create(badRadius, &policy, &error).Valid());

	VehicleDefinition risingGears = FlatCar("gears", 1000.0f, 5.0f);
	risingGears.tuning.gearRatios = { 1.0f, 2.0f };  // must fall from first to top
	NOX_CHECK(!service.Create(risingGears, &policy, &error).Valid());

	VehicleDefinition noGears = FlatCar("nogears", 1000.0f, 5.0f);
	noGears.tuning.gearRatios.clear();
	NOX_CHECK(!service.Create(noGears, &policy, &error).Valid());

	NOX_CHECK_EQ(service.Count(), size_t(0));
	// A valid definition still works after the rejections.
	VehicleHandle ok = service.Create(FlatCar("ok", 1000.0f, 5.0f), &policy, &error);
	NOX_CHECK(ok.Valid());
	NOX_CHECK(error.empty());

	// Tuning updates are validated too; a rejected update keeps the previous tuning.
	VehicleTuning bad = FlatCar("ok", 1000.0f, 5.0f).tuning;
	bad.gearRatios = { 1.0f, 2.0f };
	NOX_CHECK(!service.SetTuning(ok, bad, &error));
	NOX_CHECK(!service.SetTuning(VehicleHandle(), FlatCar("x", 1.0f, 1.0f).tuning, &error));
}

NOX_TEST(Vehicle, FixedStepAccumulatorAndDtClamp) {
	VehicleService service;
	ScriptedPolicy policy;
	VehicleHandle h = service.Create(FlatCar("car", 1000.0f, 5.0f), &policy);
	NOX_REQUIRE(h.Valid());
	const size_t afterCreate = policy.controls;

	// Zero sub-steps: 1/1000 s is below the 1/240 s step.
	service.Advance(h, 0.001f, VehicleInput(), false);
	NOX_CHECK_EQ(policy.controls, afterCreate);

	// The debt carries over: 0.001 + 0.004 s is one sub-step.
	service.Advance(h, 0.004f, VehicleInput(), false);
	NOX_CHECK_EQ(policy.controls, afterCreate + 1);

	// Multiple sub-steps: 1/60 s is exactly four 1/240 s steps.
	const size_t beforeFrame = policy.controls;
	service.Advance(h, 1.0f / 60.0f, VehicleInput(), false);
	NOX_CHECK_EQ(policy.controls, beforeFrame + 4);

	// Frame dt clamp: a 1 s hitch simulates no more than a 0.05 s frame. Two identical cars, one fed
	// the raw hitch and one the clamped value, must end in the same state with the same step count.
	VehicleService a, b;
	ScriptedPolicy pa, pb;
	VehicleHandle ha = a.Create(FlatCar("car", 1000.0f, 5.0f), &pa);
	VehicleHandle hb = b.Create(FlatCar("car", 1000.0f, 5.0f), &pb);
	a.Advance(ha, 1.0f, Throttle(1.0f), false);
	b.Advance(hb, 0.05f, Throttle(1.0f), false);
	NOX_CHECK_EQ(pa.controls, pb.controls);
	NOX_CHECK(pa.controls <= 12 && pa.controls >= 11);
	NOX_CHECK(SameState(a.State(ha), b.State(hb)));

	// Frozen frames hold the car still and do not run the policy's control phase.
	const size_t beforeFrozen = pa.controls;
	a.Advance(ha, 0.05f, Throttle(1.0f), true);
	NOX_CHECK_EQ(pa.controls, beforeFrozen);
	NOX_CHECK(pa.calls.find('H') != std::string::npos);
}

NOX_TEST(Vehicle, PhaseOrderPerSubstep) {
	VehicleService service;
	ScriptedPolicy policy;
	VehicleHandle h = service.Create(FlatCar("car", 1000.0f, 5.0f), &policy);
	NOX_REQUIRE(h.Valid());
	policy.calls.clear();
	service.Advance(h, 1.0f / 240.0f, Throttle(0.5f), false);
	// One sub-step: control, yaw assist, velocity correction, post step, in that order.
	NOX_CHECK_EQ(policy.calls, std::string("CYVP"));

	// Observer: one record per sub-step, taken after the step.
	std::vector<float> seenTimes;
	service.SetSubstepObserver(h, [&](const VehicleSubstepRecord& r) { seenTimes.push_back(r.state.time); NOX_CHECK_EQ(r.h, VehicleService::kFixedStep); });
	service.Advance(h, 2.0f / 240.0f, Throttle(0.5f), false);
	NOX_CHECK_EQ(seenTimes.size(), size_t(2));
	if (seenTimes.size() == 2) NOX_CHECK(seenTimes[1] > seenTimes[0]);
}

NOX_TEST(Vehicle, TeleportAndRespawnNotifyPolicy) {
	VehicleService service;
	ScriptedPolicy policy;
	VehicleHandle h = service.Create(FlatCar("car", 1000.0f, 5.0f), &policy);
	NOX_REQUIRE(h.Valid());
	// Creation places the car at spawn: teleport then respawn.
	NOX_CHECK(policy.calls.find("TR") != std::string::npos);
	const uint32_t serial = service.State(h).teleportSerial;
	service.Teleport(h, glm::vec2(10.0f, 20.0f), 0.5f);
	NOX_CHECK(service.State(h).teleportSerial == serial + 1);
	NOX_CHECK(service.TakeTeleported(h));
	NOX_CHECK(!service.TakeTeleported(h));
	NOX_CHECK(service.State(h).pos == glm::vec2(10.0f, 20.0f));
}
