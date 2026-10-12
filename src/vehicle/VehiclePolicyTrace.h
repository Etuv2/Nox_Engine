#pragma once
// Policy-call recorder and open-loop replay (NoxEngineCore).
//
// NOX_VEHICLE_POLICY_TRACE=<file.jsonl> (DriveMode) wraps the car's policy in a Recorder. Every
// IVehiclePolicy call is written as one JSON line with its complete inputs (telemetry, pedals, phase
// inputs) and outputs, plus the policy debug state after the call. Values use %.9g, which round-trips
// float exactly. Replay feeds the recorded inputs call by call into another policy and compares that
// policy's outputs with the recorded ones. The replayed policy's state evolves only from the replayed
// calls (open loop: no vehicle physics runs), so each call is judged on identical inputs.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "VehiclePolicy.h"

namespace VehiclePolicyTrace {

	using LineSink = std::function<void(const std::string& line)>;

	// Forwards every call to `inner` and emits one record per call.
	class Recorder final : public IVehiclePolicy {
	public:
		Recorder(IVehiclePolicy& inner, LineSink sink);
		void OnTeleport() override;
		void OnRespawn() override;
		void OnHold() override;
		void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) override;
		void YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) override;
		void VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) override;
		void PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) override;
		VehiclePolicyDebug Debug() const override { return m_inner.Debug(); }

	private:
		void Emit(const char* call, const VehicleInput* in, const VehicleStepTelemetry* t, const std::string& body);
		IVehiclePolicy& m_inner;
		LineSink m_sink;
		uint64_t m_seq = 0;
	};

	// Per-field comparison of replayed outputs against the recorded ones.
	struct FieldError {
		double maxAbs = 0.0;     // largest |replayed - recorded|
		double maxRel = 0.0;     // largest |replayed - recorded| / |recorded| over |recorded| > 1e-3
		uint64_t count = 0;      // comparisons made
		uint64_t above = 0;      // comparisons with |diff| > 0
	};

	struct Report {
		uint64_t calls = 0;
		std::map<std::string, FieldError> fields;   // numeric outputs (phase.field)
		uint64_t exactMismatches = 0;               // bool and integer outputs that differ
		uint64_t gearMismatches = 0;                // subset of exactMismatches: gear selections
		std::string firstMismatch;                  // description of the first exact mismatch
	};

	// Replays recorded lines into `policy`. Returns false with *error on malformed input.
	bool Replay(const std::vector<std::string>& lines, IVehiclePolicy& policy, Report& report, std::string* error = nullptr);

	// Formats the report as a fixed-width table (one row per numeric field).
	std::string FormatReport(const Report& report);
}
