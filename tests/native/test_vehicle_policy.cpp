// Policy-call recorder and open-loop replay (vehicle/VehiclePolicyTrace). Headless: a test-local policy
// with varied outputs drives VehicleService; the recorded calls are replayed into fresh policies.
#include "TestFramework.h"
#include "vehicle/VehiclePolicyTrace.h"
#include "vehicle/VehicleService.h"

#include <cmath>
#include <string>
#include <vector>

namespace {
	// Deterministic policy with every actuation field in use and state that changes over time.
	class VariedPolicy final : public IVehiclePolicy {
	public:
		explicit VariedPolicy(float bias = 0.0f) : m_bias(bias) {}
		void OnTeleport() override { m_nitro = 1.0f; }
		void OnRespawn() override { m_nitro = 1.0f; }
		void OnHold() override {}
		void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) override {
			out.gear = t.vx < 5.0f ? 1 : 2;
			out.steerAngle = 0.2f * std::sin(t.time * 3.0f) + m_bias;
			out.driveThrottle = in.throttle;
			out.reverseDrive = in.brake > 0.1f ? in.brake : 0.0f;
			out.brakeCommand = in.brake * 0.5f;
			out.brakeFrontShare = 0.6f;
			out.brakeRearShare = in.throttle > 0.5f ? 0.0f : 0.4f;
			out.rearLock = in.handbrake;
			out.holdAtRest = in.throttle == 0.0f && in.brake == 0.0f;
			out.nitroDriveScale = in.nitrous ? 1.2f : 1.0f;
			out.dragScale = in.nitrous ? 0.5f : 1.0f;
			m_nitro = in.nitrous ? 0.5f : 1.0f;
		}
		void YawAssist(const VehicleInput&, const VehicleStepTelemetry& t, VehicleYawAssist& io) override {
			io.yawAcc += 0.5f * std::sin(t.beta * 4.0f);
			io.ax += 0.1f;
		}
		void VelocityCorrection(const VehicleInput&, const VehicleStepTelemetry&, VehicleVelocity& io) override {
			io.vx *= 0.9995f;
		}
		void PostStep(const VehicleInput&, const VehicleStepTelemetry& t, VehicleGearRequest& io) override {
			io.gearboxShifting = t.time < 0.5f;
			io.gear = t.gear;
		}
		VehiclePolicyDebug Debug() const override {
			VehiclePolicyDebug d;
			d.nitro = m_nitro;
			d.shiftTimer = 0.25f;
			return d;
		}
	private:
		float m_bias;
		float m_nitro = 1.0f;
	};

	VehicleDefinition TraceCar() {
		VehicleDefinition d;
		d.name = "trace";
		d.tuning.mass = 1200.0f;
		d.tuning.driveAcceleration = 6.0f;
		d.tuning.topSpeedKmh = 160.0f;
		d.tuning.bumpAmplitude = 0.0f;
		d.tuning.gearRatios = { 3.0f, 2.0f, 1.5f, 1.2f, 1.0f };
		return d;
	}

	// Drives a car through pedals, handbrake, nitrous, a frozen frame and a teleport; returns the trace.
	std::vector<std::string> RecordScenario(IVehiclePolicy& policy) {
		std::vector<std::string> lines;
		VehiclePolicyTrace::Recorder rec(policy, [&lines](const std::string& l) { lines.push_back(l); });
		VehicleService svc;
		std::string error;
		VehicleHandle h = svc.Create(TraceCar(), &rec, &error);
		NOX_REQUIRE(h.Valid());
		for (int frame = 0; frame < 300; ++frame) {
			VehicleInput in;
			in.throttle = 0.5f + 0.5f * std::sin(frame * 0.1f);
			in.brake = frame % 80 < 10 ? 0.8f : 0.0f;
			in.steer = std::sin(frame * 0.05f);
			in.handbrake = frame >= 200 && frame < 230;
			in.nitrous = frame >= 100 && frame < 160;
			if (frame == 250) svc.Teleport(h, glm::vec2(10.0f, -20.0f), 0.5f);
			svc.Advance(h, 1.0f / 60.0f, in, frame == 150);
		}
		return lines;
	}
}

// Recorded calls replayed into an identical policy reproduce every output exactly.
NOX_TEST(VehiclePolicy, ReplayIsExactForSamePolicy) {
	VariedPolicy recorded;
	const std::vector<std::string> lines = RecordScenario(recorded);
	NOX_REQUIRE(lines.size() > 1000);
	VariedPolicy replayed;
	VehiclePolicyTrace::Report report;
	std::string error;
	NOX_REQUIRE(VehiclePolicyTrace::Replay(lines, replayed, report, &error));
	NOX_CHECK_EQ(report.calls, static_cast<uint64_t>(lines.size()));
	NOX_CHECK_EQ(report.exactMismatches, static_cast<uint64_t>(0));
	for (const auto& kv : report.fields) {
		NOX_CHECK(kv.second.count > 0);
		NOX_CHECK_EQ(kv.second.maxAbs, 0.0);
	}
	// Every phase was exercised.
	NOX_CHECK(report.fields.count("control.steerAngle") == 1);
	NOX_CHECK(report.fields.count("yaw.yawAcc") == 1);
	NOX_CHECK(report.fields.count("velocity.vx") == 1);
	NOX_CHECK(report.fields.count("debug.nitro") == 1);
}

// A policy with a different output is detected by the replay (the comparison is not vacuous).
NOX_TEST(VehiclePolicy, ReplayDetectsDifferentPolicy) {
	VariedPolicy recorded;
	const std::vector<std::string> lines = RecordScenario(recorded);
	VariedPolicy biased(0.01f);
	VehiclePolicyTrace::Report report;
	NOX_REQUIRE(VehiclePolicyTrace::Replay(lines, biased, report));
	NOX_CHECK(report.fields.at("control.steerAngle").maxAbs > 0.0);
	NOX_CHECK(report.fields.at("control.steerAngle").above > 0u);
	NOX_CHECK_EQ(report.fields.at("velocity.vx").maxAbs, 0.0);
}

// Each record is self-describing; a line without a call is rejected.
NOX_TEST(VehiclePolicy, MalformedTraceIsRejected) {
	VariedPolicy p;
	VehiclePolicyTrace::Report report;
	std::string error;
	NOX_CHECK(!VehiclePolicyTrace::Replay({ "{\"seq\":1}" }, p, report, &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(!VehiclePolicyTrace::Replay({ "{\"seq\":1,\"call\":\"bogus\"}" }, p, report, &error));
}

// Non-finite outputs are recorded as null and replayed as NaN (a NaN output is a mismatch, not a pass).
NOX_TEST(VehiclePolicy, NonFiniteValuesRoundTrip) {
	class NanPolicy final : public IVehiclePolicy {
	public:
		void OnTeleport() override {}
		void OnRespawn() override {}
		void OnHold() override {}
		void Control(const VehicleInput&, const VehicleStepTelemetry&, VehicleActuation& out) override { out.steerAngle = std::nanf(""); }
		void YawAssist(const VehicleInput&, const VehicleStepTelemetry&, VehicleYawAssist&) override {}
		void VelocityCorrection(const VehicleInput&, const VehicleStepTelemetry&, VehicleVelocity&) override {}
		void PostStep(const VehicleInput&, const VehicleStepTelemetry&, VehicleGearRequest&) override {}
		VehiclePolicyDebug Debug() const override { return VehiclePolicyDebug(); }
	};
	NanPolicy recordedPolicy;
	const std::vector<std::string> lines = RecordScenario(recordedPolicy);
	bool sawNull = false;
	for (const std::string& l : lines) sawNull = sawNull || l.find("\"steerAngle\":null") != std::string::npos;
	NOX_CHECK(sawNull);
	VariedPolicy other;
	VehiclePolicyTrace::Report report;
	NOX_REQUIRE(VehiclePolicyTrace::Replay(lines, other, report));
	NOX_CHECK(report.fields.at("control.steerAngle").above > 0u);
	NOX_CHECK(!VehiclePolicyTrace::FormatReport(report).empty());
}
