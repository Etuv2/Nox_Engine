// LuaVehiclePolicy: fault handling (load, Lua error, budget, invalid or missing output), two independent
// cars driven by project controllers, and the Lua call cost per sub-step. Headless (no GPU, no AssetIO).
// The NFS controller is loaded from projects/nox-racing/scripts through a disk-backed ScriptSource.
#include "TestFramework.h"
#include "scripting/LuaRuntime.h"
#include "scripting/LuaVehicleBindings.h"
#include "scripting/LuaVehiclePolicy.h"
#include "vehicle/VehicleService.h"
#include "vehicle/VehiclePolicyTrace.h"

#include "json.hpp"
#include "lua.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

using nlohmann::json;

namespace {
	class MemorySource final : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			auto it = files.find(logicalPath);
			if (it == files.end()) { error = "no such file: " + logicalPath; return false; }
			text = it->second;
			return true;
		}
	};

	class DiskSource final : public NoxLua::ScriptSource {
	public:
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const std::filesystem::path p = NoxTest::RepoRoot() / std::filesystem::path(logicalPath);
			std::ifstream in(p, std::ios::binary);
			if (!in) { error = "cannot open " + p.string(); return false; }
			std::ostringstream s;
			s << in.rdbuf();
			text = s.str();
			return true;
		}
	};

	json LoadR33() {
		std::ifstream in(NoxTest::RepoRoot() / "projects/nox-racing/data/vehicles/r33.json", std::ios::binary);
		std::ostringstream s;
		s << in.rdbuf();
		return json::parse(s.str());
	}

	// The controller's params object of the data file (handling + policy).
	json ControllerParams(const json& r33) {
		return r33.at("controller").at("params");
	}

	// Planar car with the R33 tuning (flat plane, no road network).
	VehicleDefinition NfsCar(const json& r33, const std::string& name) {
		const json& tu = r33.at("tuning");
		VehicleDefinition d;
		d.name = name;
		VehicleTuning& t = d.tuning;
		t.topSpeedKmh = tu.at("top_speed_kmh").get<float>();
		t.driveAcceleration = tu.at("drive_accel").get<float>();
		t.brakeDecel = tu.at("brake_decel").get<float>();
		t.reverseMaxKmh = tu.at("reverse_max_kmh").get<float>();
		t.mass = tu.at("mass").get<float>();
		t.cgHeight = tu.at("cg_height").get<float>();
		t.tireGrip = tu.at("tire_grip").get<float>();
		t.tireGripLong = tu.at("tire_grip_long").get<float>();
		t.rpmIdle = tu.at("rpm_idle").get<float>();
		t.rpmLimiter = tu.at("rpm_limiter").get<float>();
		t.gearRatios.clear();
		for (const json& r : tu.at("gear_ratios")) t.gearRatios.push_back(r.get<float>());
		t.suspensionFreq = tu.at("suspension_freq").get<float>();
		t.suspensionDamping = tu.at("suspension_damping").get<float>();
		t.bumpAmplitude = 0.0f;
		t.roofHeight = tu.value("roof_height", 1.6f);
		return d;
	}

	// Pedal pattern that exercises every NFS policy: launch, brake tap, drift entry, reverse, nitrous, burnout.
	VehicleInput PatternAt(int frame, int variant) {
		VehicleInput in;
		const int f = frame + 97 * variant;
		in.throttle = (f % 600) < 420 ? 0.9f : 0.2f;
		in.brake = (f % 500) > 430 ? 0.8f : 0.0f;
		in.steer = std::sin(f * 0.02f) * (variant == 1 ? 1.0f : 0.6f);
		in.handbrake = (f % 900) > 780;
		in.aux["nitrous"] = (f % 700) > 500 ? 1.0f : 0.0f;
		return in;
	}

	bool SameState(const VehicleState& a, const VehicleState& b) {
		if (a.pos != b.pos || a.vel != b.vel || a.heading != b.heading || a.vx != b.vx || a.vy != b.vy) return false;
		if (a.yawRate != b.yawRate || a.rpm != b.rpm || a.gear != b.gear || a.time != b.time) return false;
		return true;
	}

	// Pass-through policy: the cost baseline (no Lua). Applies the same pedal mapping as the controller.
	class PassThroughPolicy final : public IVehiclePolicy {
	public:
		void OnTeleport() override {}
		void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) override {
			out.gear = t.gear;
			out.driveThrottle = in.throttle;
			out.brakeCommand = in.brake;
			out.rearLock = in.handbrake;
		}
		void YawAssist(const VehicleInput&, const VehicleStepTelemetry&, VehicleYawAssist&) override {}
		void VelocityCorrection(const VehicleInput&, const VehicleStepTelemetry&, VehicleVelocity&) override {}
		void PostStep(const VehicleInput&, const VehicleStepTelemetry&, VehicleGearRequest&) override {}
		VehiclePolicyDebug Debug() const override { return VehiclePolicyDebug(); }
	};

	// Controller source for the fault tests: a valid controller with one configurable faulty phase.
	std::string ControllerSource(const std::string& controlBody) {
		return std::string(
			"local C = {}\n"
			"C.__index = C\n"
			"function C.new(params) return setmetatable({ debug = { nitro = 1.0 } }, C) end\n"
			"function C:control(tel, inp, act)\n") + controlBody + "\nend\n"
			"function C:yaw_assist(tel, inp, io) end\n"
			"function C:velocity(tel, inp, io) end\n"
			"function C:post_step(tel, inp, io) io.gear_shifting = false end\n"
			"return C\n";
	}

	MemorySource MemoryWith(const std::string& controlBody) {
		MemorySource src;
		src.files["projects/test/scripts/vehicle/ctrl.lua"] = ControllerSource(controlBody);
		return src;
	}

	// Common act fields that a valid control sets (a faulty body replaces a subset).
	const char* kAllControl =
		"  act.gear = act.gear\n"
		"  act.steer_angle = 0.0\n  act.drive_scale = 1.0\n  act.drive_shift_cut = false\n  act.drag_scale = 1.0\n"
		"  act.rear_grip_scale = 1.0\n  act.drive_throttle = inp.throttle\n  act.reverse_drive = 0.0\n"
		"  act.brake_command = inp.brake\n  act.brake_front_share = 0.68\n  act.brake_rear_share = 0.32\n"
		"  act.brake_at_standstill = false\n  act.rear_drive_cut = false\n  act.rear_spin_floor = 0.0\n"
		"  act.rear_lock = inp.handbrake\n  act.hold_at_rest = false";

	NoxLua::RuntimeLimits SmallLimits() {
		NoxLua::RuntimeLimits l;
		l.memoryBytes = 4u * 1024u * 1024u;
		l.instructionBudget = 20000;
		return l;
	}

	// Drives a single flat car for `frames` 240 Hz frames with the pattern and returns its final state.
	VehicleState DriveCar(VehicleService& svc, VehicleHandle h, int frames, int variant) {
		for (int f = 0; f < frames; ++f) svc.Advance(h, VehicleService::kFixedStep, PatternAt(f, variant), false);
		return svc.State(h);
	}
}

// --- Fault handling ---------------------------------------------------------------------------

// A controller that cannot be loaded is faulted from the start: the car coasts on the safe actuation and no
// native policy takes over (the car does not drive with the throttle held).
NOX_TEST(VehiclePolicyLua, LoadFailureCoastsAndReports) {
	MemorySource empty;
	NoxLua::Runtime rt(SmallLimits(), &empty);
	rt.SetScriptRoots({ "projects/test/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.missing", json::object());
	NOX_CHECK(!policy.Ok());
	NOX_CHECK(policy.Faulted());
	NOX_REQUIRE(!policy.Faults().empty());
	NOX_CHECK_EQ(policy.Faults().front().phase, std::string("load"));

	VehicleService svc;
	std::string error;
	VehicleHandle h = svc.Create(NfsCar(LoadR33(), "load-fail"), &policy, &error);
	NOX_REQUIRE(h.Valid());
	VehicleInput throttle;
	throttle.throttle = 1.0f;
	for (int f = 0; f < 240; ++f) svc.Advance(h, VehicleService::kFixedStep, throttle, false);
	NOX_CHECK(std::abs(svc.State(h).vx) < 1e-3f);
	NOX_CHECK(std::abs(svc.State(h).pos.y) < 1e-3f);
}

// A Lua error in control: the call faults, the vehicle gets the safe actuation, later calls do not reach Lua
// until RetryFaulted, and a repeated error is counted, not duplicated.
NOX_TEST(VehiclePolicyLua, ControlErrorFaultsAndSafeActuationApplies) {
	MemorySource src = MemoryWith("  error('boom in control')");
	NoxLua::Runtime rt(SmallLimits(), &src);
	rt.SetScriptRoots({ "projects/test/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.ctrl", json::object());
	NOX_REQUIRE(policy.Ok());

	VehicleStepTelemetry t;
	VehicleInput in;
	in.throttle = 1.0f;
	VehicleActuation out;
	out.gear = 3;
	policy.Control(in, t, out);
	NOX_CHECK(policy.Faulted());
	NOX_REQUIRE(policy.Faults().size() == 1);
	NOX_CHECK_EQ(policy.Faults()[0].phase, std::string("control"));
	NOX_CHECK(policy.Faults()[0].message.find("boom in control") != std::string::npos);
	NOX_CHECK_EQ(out.gear, 3);
	NOX_CHECK_EQ(out.driveThrottle, 0.0f);
	NOX_CHECK_EQ(out.brakeCommand, NoxLua::LuaVehiclePolicy::kSafeBrake);
	NOX_CHECK_EQ(out.steerAngle, 0.0f);

	const uint64_t callsBefore = policy.Stats(1).calls;
	policy.Control(in, t, out);
	NOX_CHECK_EQ(policy.Stats(1).calls, callsBefore);   // faulted: no Lua call

	policy.RetryFaulted();
	policy.Control(in, t, out);
	NOX_CHECK(policy.Faulted());
	NOX_REQUIRE(!policy.Faults().empty());
	NOX_CHECK_EQ(policy.Faults()[0].count, static_cast<uint64_t>(2));
	NOX_CHECK_EQ(out.driveThrottle, 0.0f);
}

// A runaway controller is stopped by the per-call instruction budget and faults.
NOX_TEST(VehiclePolicyLua, InstructionBudgetFaults) {
	MemorySource src = MemoryWith("  while true do end");
	NoxLua::Runtime rt(SmallLimits(), &src);
	rt.SetScriptRoots({ "projects/test/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.ctrl", json::object());
	NOX_REQUIRE(policy.Ok());
	VehicleStepTelemetry t;
	VehicleInput in;
	VehicleActuation out;
	policy.Control(in, t, out);
	NOX_REQUIRE(policy.Faulted());
	NOX_CHECK(policy.Faults()[0].message.find("instruction budget") != std::string::npos);
}

// Outputs are validated: an out-of-range gear faults the controller (the shift is not applied).
NOX_TEST(VehiclePolicyLua, InvalidOutputFaults) {
	MemorySource src = MemoryWith(std::string(kAllControl) + "\n  act.gear = 9");
	NoxLua::Runtime rt(SmallLimits(), &src);
	rt.SetScriptRoots({ "projects/test/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.ctrl", json::object());
	NOX_REQUIRE(policy.Ok());
	VehicleStepTelemetry t;
	t.gearCount = 6;
	VehicleInput in;
	VehicleActuation out;
	out.gear = 2;
	policy.Control(in, t, out);
	NOX_REQUIRE(policy.Faulted());
	NOX_CHECK(policy.Faults()[0].message.find("gear") != std::string::npos);
	NOX_CHECK_EQ(out.gear, 2);
}

// A missing output field is a fault (the actuation table is cleared before every call).
NOX_TEST(VehiclePolicyLua, MissingOutputFaults) {
	MemorySource src = MemoryWith("  act.steer_angle = 0.0");
	NoxLua::Runtime rt(SmallLimits(), &src);
	rt.SetScriptRoots({ "projects/test/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.ctrl", json::object());
	NOX_REQUIRE(policy.Ok());
	VehicleStepTelemetry t;
	VehicleInput in;
	VehicleActuation out;
	policy.Control(in, t, out);
	NOX_REQUIRE(policy.Faulted());
	NOX_CHECK(policy.Faults()[0].message.find("drive_scale") != std::string::npos);
}

// --- Two independent cars --------------------------------------------------------------------

// Two cars with their own controllers in one VM: each car's trajectory is bit-identical to the same car run
// alone, and destroying one car leaves the other untouched (stale handles stay invalid).
NOX_TEST(VehiclePolicyLua, TwoCarsAreIndependent) {
	const json r33 = LoadR33();
	const int frames = 720;   // 3 s at 240 Hz

	VehicleState soloA, soloB;
	{
		DiskSource src;
		NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
		rt.SetScriptRoots({ "projects/nox-racing/scripts" });
		NoxLua::LuaVehiclePolicy polA(rt, "vehicle.nfs_controller", ControllerParams(r33));
		NOX_REQUIRE(polA.Ok());
		VehicleService svc;
		VehicleHandle h = svc.Create(NfsCar(r33, "solo-a"), &polA);
		NOX_REQUIRE(h.Valid());
		soloA = DriveCar(svc, h, frames, 0);
		NOX_CHECK(!polA.Faulted());
	}
	{
		DiskSource src;
		NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
		rt.SetScriptRoots({ "projects/nox-racing/scripts" });
		NoxLua::LuaVehiclePolicy polB(rt, "vehicle.nfs_controller", ControllerParams(r33));
		NOX_REQUIRE(polB.Ok());
		VehicleService svc;
		VehicleHandle h = svc.Create(NfsCar(r33, "solo-b"), &polB);
		NOX_REQUIRE(h.Valid());
		soloB = DriveCar(svc, h, frames, 1);
	}

	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::LuaVehiclePolicy polA(rt, "vehicle.nfs_controller", ControllerParams(r33));
	NoxLua::LuaVehiclePolicy polB(rt, "vehicle.nfs_controller", ControllerParams(r33));
	NOX_REQUIRE(polA.Ok() && polB.Ok());
	VehicleService svc;
	VehicleHandle hA = svc.Create(NfsCar(r33, "a"), &polA);
	VehicleHandle hB = svc.Create(NfsCar(r33, "b"), &polB);
	NOX_REQUIRE(hA.Valid() && hB.Valid());
	NOX_CHECK(hA != hB);
	for (int f = 0; f < frames; ++f) {
		svc.Advance(hA, VehicleService::kFixedStep, PatternAt(f, 0), false);
		svc.Advance(hB, VehicleService::kFixedStep, PatternAt(f, 1), false);
	}
	NOX_CHECK(SameState(svc.State(hA), soloA));
	NOX_CHECK(SameState(svc.State(hB), soloB));
	NOX_CHECK(!SameState(soloA, soloB));

	svc.Destroy(hB);
	NOX_CHECK(!svc.IsValid(hB));
	NOX_CHECK(svc.IsValid(hA));
	for (int f = 0; f < 120; ++f) svc.Advance(hA, VehicleService::kFixedStep, PatternAt(frames + f, 0), false);
	NOX_CHECK(!polA.Faulted());
	NOX_CHECK(!polB.Faulted());
}

// The data file is one source: tuning and the controller's handling agree on shared keys, and both agree with the
// NFS scene's game.handling (the scene the legacy recordings were made from).
NOX_TEST(VehiclePolicyLua, R33DataMatchesSceneHandling) {
	const json r33 = LoadR33();
	std::ifstream in(NoxTest::RepoRoot() / "scenes/nfs_sprint.json", std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	const json scene = json::parse(s.str());
	const json& sceneHandling = scene.at("game").at("handling");
	const json& handling = r33.at("controller").at("params").at("handling");
	const json& tuning = r33.at("tuning");
	for (auto it = sceneHandling.begin(); it != sceneHandling.end(); ++it) {
		NOX_CHECK(handling.contains(it.key()));
		if (handling.contains(it.key()) && it.value().is_number()) {
			NOX_CHECK_EQ(static_cast<float>(handling.at(it.key()).get<double>()), static_cast<float>(it.value().get<double>()));
		}
	}
	for (auto it = tuning.begin(); it != tuning.end(); ++it) {
		if (it.key() == "gear_ratios") continue;
		NOX_CHECK(handling.contains(it.key()));
		if (handling.contains(it.key())) {
			NOX_CHECK_EQ(static_cast<float>(handling.at(it.key()).get<double>()), static_cast<float>(it.value().get<double>()));
		}
	}
}

// ctx.vehicle from a Lua script: two cars from the data file, input and stepping, a destroyed car's handle raises,
// and profile changes: numeric accepted, geometry rejected with the restart message.
NOX_TEST(VehiclePolicyLua, VehicleApiFromScript) {
	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::VehicleHost host(rt, &src);
	lua_State* L = rt.State();
	lua_newtable(L);
	NoxLua::InstallVehicleApi(L, lua_gettop(L), &host);
	lua_setglobal(L, "ctx");

	const char* script = R"LUA(
		local def = "projects/nox-racing/data/vehicles/r33.json"
		a = ctx.vehicle.create({ definition = def, name = "a" })
		b = ctx.vehicle.create({ definition = def, name = "b", spawn = { x = 20, z = 0, heading_deg = 90 } })
		bad_controller_err = select(2, ctx.vehicle.create({ definition = def, controller = { module = "vehicle.missing" } }))
		no_controller_err = select(2, ctx.vehicle.create({ definition = def, controller = {} }))
		ctx.vehicle.set_input(a, { throttle = 1.0 })
		ctx.vehicle.set_input(b, { throttle = 0.0 })
		for i = 1, 240 do ctx.vehicle.step(1 / 240) end
		local ta = ctx.vehicle.telemetry(a)
		local tb = ctx.vehicle.telemetry(b)
		speed_a = ta.speed_kmh
		speed_b = tb.speed_kmh
		count_two = ctx.vehicle.count()
		ctx.vehicle.destroy(b)
		count_one = ctx.vehicle.count()
		stale_valid = ctx.vehicle.valid(b)
		stale_ok = pcall(ctx.vehicle.telemetry, b)
		geometry_ok, geometry_err = ctx.vehicle.set_profile(a, { front_axle_m = 1.0 })
		numeric_ok = ctx.vehicle.set_profile(a, { top_speed_kmh = 200.0 })
		unknown_ok, unknown_err = ctx.vehicle.set_profile(a, { gravity = 9.8 })
		swap_ok = ctx.vehicle.set_controller(a, "vehicle.nfs_controller")
		restart_ok, restart_err = ctx.vehicle.set_controller(a, "vehicle.missing")
		faults_a = #ctx.vehicle.faults(a)
		faults_msg = ctx.vehicle.faults(a)[1]
	)LUA";
	std::string error;
	int ref = 0;
	NOX_REQUIRE(rt.Compile(script, "projects/test/vehicle_api.lua", ref, error));
	NoxLua::RunError runError;
	const bool ran = rt.CallRef(ref, std::function<int(lua_State*)>(), 0, &runError);
	if (!ran) std::printf("[VehicleApi] %s\n", runError.message.c_str());
	NOX_REQUIRE(ran);
	rt.Unref(ref);

	auto global = [L](const char* name) { lua_getglobal(L, name); return lua_gettop(L); };
	auto numberOf = [&](const char* name) { const int i = global(name); const double v = lua_tonumber(L, i); lua_pop(L, 1); return v; };
	auto boolOf = [&](const char* name) { const int i = global(name); const bool v = lua_toboolean(L, i) != 0; lua_pop(L, 1); return v; };
	auto textOf = [&](const char* name) { const int i = global(name); std::string v = lua_type(L, i) == LUA_TSTRING ? lua_tostring(L, i) : std::string(); lua_pop(L, 1); return v; };

	NOX_CHECK(numberOf("speed_a") > 1.0);            // throttle: the car moves
	NOX_CHECK(numberOf("speed_b") < 0.01);           // no input: the car stays
	NOX_CHECK_EQ(static_cast<int>(numberOf("count_two")), 2);
	NOX_CHECK_EQ(static_cast<int>(numberOf("count_one")), 1);
	NOX_CHECK(!boolOf("stale_valid"));
	NOX_CHECK(!boolOf("stale_ok"));                  // telemetry of a destroyed car raises
	NOX_CHECK(!boolOf("geometry_ok"));
	NOX_CHECK(textOf("geometry_err").find("restart Play") != std::string::npos);
	NOX_CHECK(boolOf("numeric_ok"));
	NOX_CHECK(!boolOf("unknown_ok"));
	NOX_CHECK(textOf("unknown_err").find("unknown tuning key") != std::string::npos);
	NOX_CHECK(boolOf("swap_ok"));
	NOX_CHECK(!boolOf("restart_ok"));
	NOX_CHECK(textOf("restart_err").find("did not load") != std::string::npos);
	NOX_CHECK(textOf("bad_controller_err").find("did not load") != std::string::npos);
	NOX_CHECK(textOf("no_controller_err").find("controller module") != std::string::npos);
	NOX_CHECK_EQ(static_cast<int>(numberOf("faults_a")), 1);
	NOX_CHECK(textOf("faults_msg").find("load") != std::string::npos);
}

// --- Replay of an engine recording -------------------------------------------------------------

// NOX_VEHICLE_POLICY_REPLAY=<trace.jsonl> (written by NOX_VEHICLE_POLICY_TRACE=<file> in a legacy run): replays
// every recorded call into the NFS controller and prints the per-field error table. Skipped without the variable.
NOX_TEST(VehiclePolicyLua, ReplayRecordedTraceFromEnv) {
	const char* path = std::getenv("NOX_VEHICLE_POLICY_REPLAY");
	if (!path || !*path) {
		std::printf("[REPLAY] skipped (set NOX_VEHICLE_POLICY_REPLAY=<trace.jsonl> to compare the NFS controller)\n");
		return;
	}
	std::ifstream in(path);
	NOX_REQUIRE(in.good());
	std::vector<std::string> lines;
	for (std::string line; std::getline(in, line);) lines.push_back(line);
	NOX_REQUIRE(!lines.empty());
	const json r33 = LoadR33();
	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::LuaVehiclePolicy policy(rt, "vehicle.nfs_controller", ControllerParams(r33));
	NOX_REQUIRE(policy.Ok());
	VehiclePolicyTrace::Report report;
	std::string error;
	NOX_REQUIRE(VehiclePolicyTrace::Replay(lines, policy, report, &error));
	std::printf("[REPLAY] %s\n%s", path, VehiclePolicyTrace::FormatReport(report).c_str());
	NOX_CHECK(policy.Faults().empty());
}

// --- Cost -------------------------------------------------------------------------------------

// Lua cost per sub-step and per 240 Hz frame for one and four cars (one sub-step per frame at 240 Hz).
// Printed, not asserted: the numbers are the evidence for the migration report. Baseline: the same cars with a
// pass-through C++ policy (no Lua), so the difference is the controller cost.
NOX_TEST(VehiclePolicyLua, CostPerSubstep) {
	const json r33 = LoadR33();
	const int frames = 2400;   // 10 s at 240 Hz
	for (int cars : { 1, 4 }) {
		// Lua controllers.
		DiskSource src;
		NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
		rt.SetScriptRoots({ "projects/nox-racing/scripts" });
		std::vector<std::unique_ptr<NoxLua::LuaVehiclePolicy>> pols;
		VehicleService svc;
		std::vector<VehicleHandle> handles;
		for (int c = 0; c < cars; ++c) {
			pols.push_back(std::make_unique<NoxLua::LuaVehiclePolicy>(rt, "vehicle.nfs_controller", ControllerParams(r33)));
			handles.push_back(svc.Create(NfsCar(r33, "cost"), pols.back().get()));
		}
		const auto t0 = std::chrono::steady_clock::now();
		for (int f = 0; f < frames; ++f) {
			for (int c = 0; c < cars; ++c) svc.Advance(handles[c], VehicleService::kFixedStep, PatternAt(f, c), false);
		}
		const double luaWallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		uint64_t luaNs = 0, luaCalls = 0, maxNs = 0;
		for (auto& p : pols) {
			for (int phase = 1; phase <= 4; ++phase) {
				luaNs += p->Stats(phase).totalNs;
				luaCalls += p->Stats(phase).calls;
				if (p->Stats(phase).maxNs > maxNs) maxNs = p->Stats(phase).maxNs;
			}
		}
		// Sub-steps actually run (simulated time over the fixed step; the accumulator is float).
		double substeps = 0.0;
		for (VehicleHandle h : handles) substeps += std::round(svc.State(h).time / VehicleService::kFixedStep);

		// Baseline: the same cars with the pass-through policy.
		std::vector<std::unique_ptr<PassThroughPolicy>> base;
		VehicleService svc2;
		std::vector<VehicleHandle> bh;
		for (int c = 0; c < cars; ++c) {
			base.push_back(std::make_unique<PassThroughPolicy>());
			bh.push_back(svc2.Create(NfsCar(r33, "base"), base.back().get()));
		}
		const auto t1 = std::chrono::steady_clock::now();
		for (int f = 0; f < frames; ++f) {
			for (int c = 0; c < cars; ++c) svc2.Advance(bh[c], VehicleService::kFixedStep, PatternAt(f, c), false);
		}
		const double baseWallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count();

		// Per frame = all cars together, one 240 Hz frame (one sub-step per car per frame).
		std::printf("[COST] cars=%d substeps=%.0f lua_calls_per_substep=%.2f lua_us_per_substep=%.3f max_call_us=%.3f "
			"| us_per_frame(all cars): wall_with_lua=%.3f wall_passthrough=%.3f lua_marshal_and_run=%.3f\n",
			cars, substeps, luaCalls / substeps, luaNs / 1000.0 / substeps, maxNs / 1000.0,
			luaWallUs / frames, baseWallUs / frames, luaNs / 1000.0 / frames);
		NOX_CHECK(luaCalls == static_cast<uint64_t>(std::llround(substeps)) * 4u);
		for (auto& p : pols) NOX_CHECK(!p->Faulted());
	}
}

// --- Host: reload transfer, handles, fixed fields ---------------------------------------------

namespace {
	// One telemetry record after one fixed sub-step with the pattern's input for frame `f`.
	json HostStep(NoxLua::VehicleHost& host, VehicleHandle h, int f) {
		host.SetInput(h, PatternAt(f, 0));
		host.Step(VehicleService::kFixedStep, false);
		return host.Telemetry(h);
	}
}

// A successful reload mid-drive: the running car (physics, handle, latched input) moves into the candidate host and
// its controller is rebuilt in the candidate VM with the declared state. The telemetry after the swap equals the run
// that never reloaded, record by record.
static void ExpectReloadTransferContinuous(const json& r33, bool expectNitro) {
	const int total = 600, at = 300;

	std::vector<json> reference;
	{
		DiskSource src;
		NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
		rt.SetScriptRoots({ "projects/nox-racing/scripts" });
		NoxLua::VehicleHost host(rt, &src);
		std::string err;
		const VehicleHandle h = host.Create(r33, nullptr, &err);
		NOX_REQUIRE(h.Valid());
		for (int f = 0; f < total; ++f) reference.push_back(HostStep(host, h, f));
	}

	std::vector<json> reloaded;
	{
		DiskSource srcA, srcB;
		NoxLua::Runtime rtA(NoxLua::RuntimeLimits(), &srcA);
		NoxLua::Runtime rtB(NoxLua::RuntimeLimits(), &srcB);
		rtA.SetScriptRoots({ "projects/nox-racing/scripts" });
		rtB.SetScriptRoots({ "projects/nox-racing/scripts" });
		NoxLua::VehicleHost hostA(rtA, &srcA);
		std::string err;
		const VehicleHandle h = hostA.Create(r33, nullptr, &err);
		NOX_REQUIRE(h.Valid());
		for (int f = 0; f < at; ++f) reloaded.push_back(HostStep(hostA, h, f));

		// The candidate has validated (staging); the swap moves the running vehicles into it.
		NoxLua::VehicleHost hostB(rtB, &srcB);
		hostB.AdoptFrom(hostA);
		NOX_CHECK(hostB.IsValid(h));
		NOX_CHECK(!hostA.IsValid(h));
		NOX_CHECK(hostA.Count() == 0);
		const std::vector<VehicleHandle> handles = hostB.Handles();
		NOX_REQUIRE(handles.size() == 1 && handles[0] == h);
		NOX_CHECK(hostB.Faults(h).empty());
		for (int f = at; f < total; ++f) reloaded.push_back(HostStep(hostB, h, f));
	}

	int firstDiff = -1;
	for (int f = 0; f < total && firstDiff < 0; ++f) {
		if (reference[f] != reloaded[f]) firstDiff = f;
	}
	NOX_CHECK(firstDiff < 0);
	if (firstDiff >= 0) {
		std::fprintf(stderr, "first telemetry difference at frame %d\n  ref %s\n  got %s\n", firstDiff,
			reference[firstDiff].dump().c_str(), reloaded[firstDiff].dump().c_str());
	}
	// The drive exercised the controller (nitrous and a gear change happen within the window).
	bool nitroSeen = false;
	for (const json& t : reference) nitroSeen = nitroSeen || (t.contains("channels") && t["channels"].value("nitro_active", 0.0) > 0.5);
	if (expectNitro) NOX_CHECK(nitroSeen);
}

NOX_TEST(VehiclePolicyLua, ReloadTransferKeepsTelemetryContinuous) {
	ExpectReloadTransferContinuous(LoadR33(), true);
}

// The same reload with static collision (surface mode): the candidate host takes the moved car's CollisionWorld
// ownership, so the service's collision pointer stays valid after the old host releases its entries.
NOX_TEST(VehiclePolicyLua, ReloadTransferKeepsStaticCollisionAlive) {
	json r33 = LoadR33();
	r33["world"]["collision"] = { { "files", json::array({ "models/shuto/rep_section/collision/-1_3_col.glb" }) } };
	ExpectReloadTransferContinuous(r33, false);
}

// A failed candidate never touches the running host: the candidate's controller does not load, the staging is
// abandoned (no transfer), and the running car's telemetry is the no-reload run.
NOX_TEST(VehiclePolicyLua, FailedCandidateLeavesRunningCarUntouched) {
	const json r33 = LoadR33();
	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::VehicleHost host(rt, &src);
	std::string err;
	const VehicleHandle h = host.Create(r33, nullptr, &err);
	NOX_REQUIRE(h.Valid());
	std::vector<json> before;
	for (int f = 0; f < 120; ++f) before.push_back(HostStep(host, h, f));

	// Candidate with a controller that does not exist: rejected before any swap.
	json bad = r33;
	bad["controller"]["module"] = "vehicle.no_such_controller";
	NoxLua::VehicleHost candidate(rt, &src);
	const VehicleHandle none = candidate.Create(bad, nullptr, &err);
	NOX_CHECK(!none.Valid());
	NOX_CHECK(err.find("did not load") != std::string::npos);
	NOX_CHECK(candidate.Count() == 0);

	for (int f = 120; f < 240; ++f) {
		const json t = HostStep(host, h, f);
		NOX_CHECK(t.value("faulted", true) == false);
	}
	NOX_CHECK(host.IsValid(h));
}

// Numeric tuning changes live; geometry and rig changes are refused with the restart message.
NOX_TEST(VehiclePolicyLua, LiveProfileRejectsTopologyChanges) {
	const json r33 = LoadR33();
	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::VehicleHost host(rt, &src);
	std::string err;
	const VehicleHandle h = host.Create(r33, nullptr, &err);
	NOX_REQUIRE(h.Valid());
	NOX_CHECK(host.SetProfile(h, json{ { "mass", 1200.0 } }, &err));
	NOX_CHECK(!host.SetProfile(h, json{ { "track_m", 1.7 } }, &err));
	NOX_CHECK(err.find("restart Play") != std::string::npos);
	NOX_CHECK(!host.SetProfile(h, json{ { "backend", "other" } }, &err));
	NOX_CHECK(err.find("restart Play") != std::string::npos);
	NOX_CHECK(!host.SetProfile(h, json{ { "rig", json::object() } }, &err));
	NOX_CHECK(err.find("restart Play") != std::string::npos);
}

// Declared controller state round-trips through state()/restore() with its JSON types intact.
NOX_TEST(VehiclePolicyLua, DeclaredStateExportsAndRestores) {
	const json r33 = LoadR33();
	DiskSource src;
	NoxLua::Runtime rt(NoxLua::RuntimeLimits(), &src);
	rt.SetScriptRoots({ "projects/nox-racing/scripts" });
	NoxLua::VehicleHost host(rt, &src);
	std::string err;
	const VehicleHandle h = host.Create(r33, nullptr, &err);
	NOX_REQUIRE(h.Valid());
	for (int f = 0; f < 400; ++f) HostStep(host, h, f);
	const NoxLua::LuaVehiclePolicy* pol = host.Controller(h);
	NOX_REQUIRE(pol != nullptr);
	NoxLua::LuaVehiclePolicy* mut = const_cast<NoxLua::LuaVehiclePolicy*>(pol);
	json state;
	NOX_REQUIRE(mut->ExportState(state));
	NOX_CHECK(state.contains("nitro") && state.contains("gear") && state.contains("drift") && state.contains("burn"));
	NOX_CHECK(state["gear"].contains("shift_timer"));
	NOX_CHECK(mut->ImportState(state));
	json again;
	NOX_CHECK(mut->ExportState(again));
	NOX_CHECK(again == state);
}
