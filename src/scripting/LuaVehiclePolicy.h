#pragma once
// IVehiclePolicy implemented by a project Lua controller (generic engine binding; docs/lua_api.md
// "Vehicles"). The controller is a module loaded with require from the runtime's script roots; the
// engine keeps no game-specific policy here.
//
// Protocol (per vehicle, one VM each; four Lua calls per fixed sub-step at most, plus rare hooks):
//   local C = {}
//   function C.new(params) return setmetatable({ debug = {...} }, { __index = C }) end
//   function C:on_teleport() / C:on_respawn() / C:on_hold()          optional
//   function C:control(tel, inp, act)     -- every field of act must be set (see kControlFields)
//   function C:yaw_assist(tel, inp, io)   -- io.yaw_acc, io.ax: read and written
//   function C:velocity(tel, inp, io)     -- io.vx, io.vy: read and written
//   function C:post_step(tel, inp, io)    -- io.gear read and written, io.gear_shifting written
//   function C:on_refuel()                optional: ctx.vehicle.refuel(h) (the controller refills its own state)
// The telemetry, input and io tables are reused across calls: read them only during the call.
// Output tables are validated after every call. Any error (Lua error, instruction budget, missing or
// out-of-range output) FAULTS the controller: the vehicle receives the safe actuation (coast with a
// light brake, steering centred, no shift) and no other policy takes over. RetryFaulted() re-enables
// the controller (the old state is kept); a reload is the normal recovery.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"
#include "vehicle/VehiclePolicy.h"

namespace NoxLua {
	class Runtime;

	struct VehicleFault {
		std::string phase;        // "load", "on_teleport", "control", "yaw_assist", "velocity", "post_step", "output"
		std::string message;      // first line of the error (with the Lua location when available)
		uint64_t count = 0;       // faults of this phase since the controller was created
	};

	// Wall-clock cost of the controller calls (steady clock; includes the table marshalling).
	struct VehicleCallStats {
		uint64_t calls = 0;
		uint64_t totalNs = 0;
		uint64_t maxNs = 0;
	};

	class LuaVehiclePolicy final : public IVehiclePolicy {
	public:
		// Safe actuation applied while faulted (documented in docs/lua_api.md).
		static constexpr float kSafeBrake = 0.25f;

		// `runtime` must outlive the policy and have its script roots set. `moduleName` is the require name
		// ("vehicle.controller"). `params` is converted to a Lua table and passed to controller.new.
		// Load failures leave Ok() false (Error() explains) and the policy is faulted from the start.
		LuaVehiclePolicy(Runtime& runtime, const std::string& moduleName, const nlohmann::json& params);
		~LuaVehiclePolicy() override;
		LuaVehiclePolicy(const LuaVehiclePolicy&) = delete;
		LuaVehiclePolicy& operator=(const LuaVehiclePolicy&) = delete;

		bool Ok() const { return m_loaded; }
		const std::string& Error() const { return m_error; }
		bool Faulted() const { return m_faulted; }
		const std::vector<VehicleFault>& Faults() const { return m_faults; }
		void RetryFaulted() { m_faulted = !m_loaded; }
		const std::string& ModuleName() const { return m_moduleName; }
		const VehicleCallStats& Stats(int phase) const;   // 0 teleport/respawn/hold, 1 control, 2 yaw, 3 velocity, 4 post_step
		void ResetStats();

		void OnTeleport() override;
		void OnRespawn() override;
		void OnHold() override;
		// Called by ctx.vehicle.refuel(h). Not part of the per-step protocol; no-op without the optional hook.
		void OnRefuel();
		void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) override;
		void YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) override;
		void VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) override;
		void PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) override;
		VehiclePolicyDebug Debug() const override { return m_debug; }
		// Declared controller state (optional hooks: C:state() returns a table of plain data, C:restore(t) installs it).
		// ExportState returns false when there is no state hook (nothing to carry over); errors fault the controller.
		bool ExportState(nlohmann::json& out);
		bool ImportState(const nlohmann::json& state);
		// The debug table that telemetry reads, carried over at a reload (the new controller has not run yet).
		void AdoptDebug(const VehiclePolicyDebug& d) { m_debug = d; }
		// One-shot: true once after a shift or direction change reported by the controller (debug.shift_event).
		bool ConsumeShift() { const bool s = m_shiftEvent; m_shiftEvent = false; return s; }

	private:
		struct Impl;
		enum Hook { kTeleport, kRespawn, kHold, kControl, kYaw, kVelocity, kPostStep, kRefuel, kHookCount };
		enum StatPhase { kStatHook, kStatControl, kStatYaw, kStatVelocity, kStatPost, kStatCount };
		struct StateCall;

		bool Call(Hook hook, StatPhase stat, const char* phase);
		void Fault(const char* phase, const std::string& message);

		std::unique_ptr<Impl> m_impl;
		std::string m_moduleName;
		std::string m_error;
		bool m_loaded = false;
		bool m_faulted = false;
		std::vector<VehicleFault> m_faults;
		VehiclePolicyDebug m_debug;
		bool m_shiftEvent = false;
		VehicleCallStats m_stats[kStatCount];
	};
}
