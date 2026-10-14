#include "LuaVehiclePolicy.h"

#include "LuaRuntime.h"

#include "lauxlib.h"
#include "lua.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>

using nlohmann::json;

namespace NoxLua {
	namespace {
		using Clock = std::chrono::steady_clock;

		// Field names of the telemetry table (snake_case; see docs/lua_api.md "Vehicles").
		struct NumField { const char* key; float VehicleStepTelemetry::* member; };
		const NumField kTelNumbers[] = {
			{ "time", &VehicleStepTelemetry::time }, { "h", &VehicleStepTelemetry::h },
			{ "pos_x", nullptr }, { "pos_y", nullptr },   // filled from pos (handled below)
			{ "vel_x", nullptr }, { "vel_y", nullptr },
			{ "heading", &VehicleStepTelemetry::heading },
			{ "vx", &VehicleStepTelemetry::vx }, { "vy", &VehicleStepTelemetry::vy },
			{ "yaw_rate", &VehicleStepTelemetry::yawRate },
			{ "ax", &VehicleStepTelemetry::ax }, { "ay", &VehicleStepTelemetry::ay },
			{ "steer_angle", &VehicleStepTelemetry::steerAngle },
			{ "speed", &VehicleStepTelemetry::speed }, { "speed01", &VehicleStepTelemetry::speed01 },
			{ "beta", &VehicleStepTelemetry::beta }, { "vx_abs", &VehicleStepTelemetry::vxAbs },
			{ "wheelbase", &VehicleStepTelemetry::wheelbase }, { "track", &VehicleStepTelemetry::track },
			{ "top_speed", &VehicleStepTelemetry::topSpeed }, { "rear_spin", &VehicleStepTelemetry::rearSpin },
			{ "rpm", &VehicleStepTelemetry::rpm }, { "wheel_speed", &VehicleStepTelemetry::wheelSpeed },
			{ "ground_pitch", &VehicleStepTelemetry::groundPitch }, { "ground_roll", &VehicleStepTelemetry::groundRoll },
			{ "rpm_idle", &VehicleStepTelemetry::rpmIdle }, { "final_drive", &VehicleStepTelemetry::finalDrive },
			{ "rear_wheel_radius", &VehicleStepTelemetry::rearWheelRadius },
		};

		const char* const kControlFields[] = {
			"gear", "steer_angle", "drive_scale", "drive_shift_cut", "drag_scale", "rear_grip_scale",
			"drive_throttle", "reverse_drive", "brake_command", "brake_front_share", "brake_rear_share",
			"brake_at_standstill", "rear_drive_cut", "rear_spin_floor", "rear_lock", "hold_at_rest",
		};

		// Lua reads of the controller's tables are protected (ProtectedCall) and validated in C++.
		double ReqNum(lua_State* L, int idx, const char* key) {
			lua_getfield(L, idx, key);
			if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "field '%s' must be a number", key);
			const double v = lua_tonumber(L, -1);
			lua_pop(L, 1);
			return v;
		}
		bool ReqBool(lua_State* L, int idx, const char* key) {
			lua_getfield(L, idx, key);
			if (lua_type(L, -1) != LUA_TBOOLEAN) luaL_error(L, "field '%s' must be a boolean", key);
			const bool v = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return v;
		}
		bool OptNum(lua_State* L, int idx, const char* key, double& out) {
			lua_getfield(L, idx, key);
			bool present = false;
			if (lua_type(L, -1) == LUA_TNUMBER) { out = lua_tonumber(L, -1); present = true; }
			else if (lua_type(L, -1) != LUA_TNIL) luaL_error(L, "field '%s' must be a number", key);
			lua_pop(L, 1);
			return present;
		}
		bool OptBool(lua_State* L, int idx, const char* key, bool& out) {
			lua_getfield(L, idx, key);
			bool present = false;
			if (lua_type(L, -1) == LUA_TBOOLEAN) { out = lua_toboolean(L, -1) != 0; present = true; }
			else if (lua_type(L, -1) != LUA_TNIL) luaL_error(L, "field '%s' must be a boolean", key);
			lua_pop(L, 1);
			return present;
		}

		void SetNil(lua_State* L, const char* key) { lua_pushnil(L); lua_setfield(L, -2, key); }
		void SetNum(lua_State* L, const char* key, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, key); }
		void SetBool(lua_State* L, const char* key, bool v) { lua_pushboolean(L, v ? 1 : 0); lua_setfield(L, -2, key); }

		// Recursive JSON -> Lua conversion for controller.new(params). Depth-limited.
		void PushJson(lua_State* L, const json& j, int depth) {
			if (depth > 8) luaL_error(L, "params nested too deeply");
			switch (j.type()) {
			case json::value_t::null: lua_pushnil(L); return;
			case json::value_t::boolean: lua_pushboolean(L, j.get<bool>() ? 1 : 0); return;
			case json::value_t::number_integer:
			case json::value_t::number_unsigned:
			case json::value_t::number_float: lua_pushnumber(L, j.get<double>()); return;
			case json::value_t::string: {
				const std::string s = j.get<std::string>();
				lua_pushlstring(L, s.data(), s.size());
				return;
			}
			case json::value_t::array: {
				lua_createtable(L, static_cast<int>(j.size()), 0);
				int i = 1;
				for (const json& e : j) {
					PushJson(L, e, depth + 1);
					lua_rawseti(L, -2, i++);
				}
				return;
			}
			case json::value_t::object: {
				lua_createtable(L, 0, static_cast<int>(j.size()));
				for (auto it = j.begin(); it != j.end(); ++it) {
					PushJson(L, it.value(), depth + 1);
					lua_setfield(L, -2, it.key().c_str());
				}
				return;
			}
			default: lua_pushnil(L); return;
			}
		}

		// Last line of a message (the traceback is kept out of the per-frame diagnostics).
		std::string FirstLine(const std::string& s) {
			const size_t nl = s.find('\n');
			return nl == std::string::npos ? s : s.substr(0, nl);
		}

		bool Finite(float v) { return std::isfinite(v); }
		bool InRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
	}

	namespace {
		// Lua table -> JSON for declared controller state: plain data only (strings keys, numbers, booleans, tables).
		json TableToJson(lua_State* L, int idx, int depth) {
			idx = lua_absindex(L, idx);
			if (depth > 8) luaL_error(L, "controller state nested too deeply");
			switch (lua_type(L, idx)) {
			case LUA_TNIL: return nullptr;
			case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
			case LUA_TNUMBER:
				if (lua_isinteger(L, idx)) return static_cast<int64_t>(lua_tointeger(L, idx));
				return lua_tonumber(L, idx);
			case LUA_TSTRING: return std::string(lua_tostring(L, idx));
			case LUA_TTABLE: {
				json out = json::object();
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "controller state keys must be strings");
					out[lua_tostring(L, -2)] = TableToJson(L, -1, depth + 1);
					lua_pop(L, 1);
				}
				return out;
			}
			default:
				luaL_error(L, "controller state must hold plain data (no functions, userdata or threads)");
				return nullptr;
			}
		}
	}

	// -----------------------------------------------------------------------------------------
	// Implementation state: registry references, reused tables and the current call's pointers.
	// -----------------------------------------------------------------------------------------
	struct LuaVehiclePolicy::StateCall {
		LuaVehiclePolicy::Impl* impl;
		json* out;
		const json* in;
		bool hook;
	};

	struct LuaVehiclePolicy::Impl {
		enum { kHooks = 8 };   // on_teleport, on_respawn, on_hold, control, yaw_assist, velocity, post_step, on_refuel
		explicit Impl(Runtime& r) : rt(r), L(r.State()) {
			pushArgs = [this](lua_State* state) { return PushArgs(state); };
		}

		Runtime& rt;
		lua_State* L;
		std::function<int(lua_State*)> pushArgs;   // built once: CallRef takes it by reference (no per-call allocation)
		int self = LUA_NOREF;
		int refs[kHooks] = { LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF };
		int telTable = LUA_NOREF, inpTable = LUA_NOREF, actTable = LUA_NOREF;
		int yawTable = LUA_NOREF, velTable = LUA_NOREF, gearTable = LUA_NOREF;

		// Current call (set by the policy before CallRef / ProtectedCall).
		int hook = 0;
		const VehicleStepTelemetry* t = nullptr;
		const VehicleInput* in = nullptr;
		const VehicleActuation* actIn = nullptr;
		const VehicleYawAssist* yaIn = nullptr;
		const VehicleVelocity* velIn = nullptr;
		const VehicleGearRequest* grIn = nullptr;

		// Results of the last call (filled by ReadOutputs inside a protected region).
		struct Read {
			Impl* impl = nullptr;
			int hook = 0;
			VehicleActuation act;
			VehicleYawAssist ya;
			VehicleVelocity vel;
			VehicleGearRequest gr;
			VehiclePolicyDebug dbg;
			bool shiftEvent = false;
		} read;

		void FillTelemetry(lua_State* L) const {
			const VehicleStepTelemetry& s = *t;
			for (const NumField& f : kTelNumbers) {
				if (f.member) SetNum(L, f.key, s.*(f.member));
			}
			SetNum(L, "pos_x", s.pos.x); SetNum(L, "pos_y", s.pos.y);
			SetNum(L, "vel_x", s.vel.x); SetNum(L, "vel_y", s.vel.y);
			lua_pushinteger(L, s.gear); lua_setfield(L, -2, "gear");
			lua_pushinteger(L, s.gearCount); lua_setfield(L, -2, "gear_count");
			SetBool(L, "limiter", s.limiter); SetBool(L, "airborne", s.airborne); SetBool(L, "surface", s.surface);
		}

		void FillInput(lua_State* L) const {
			SetNum(L, "throttle", in->throttle); SetNum(L, "brake", in->brake); SetNum(L, "steer", in->steer);
			SetBool(L, "handbrake", in->handbrake);
			// Named auxiliary inputs: inp.aux.<name> (number; buttons are 1 or 0). Rebuilt per call.
			lua_newtable(L);
			for (const auto& kv : in->aux) SetNum(L, kv.first.c_str(), kv.second);
			lua_setfield(L, -2, "aux");
		}

		// Pushes the arguments of the current hook (self first). Runs inside the protected call.
		int PushArgs(lua_State* L) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, self);
			if (hook <= 2 || hook == 7) return 1;   // hooks 0..2 and on_refuel take only self
			lua_rawgeti(L, LUA_REGISTRYINDEX, telTable);
			FillTelemetry(L);
			lua_rawgeti(L, LUA_REGISTRYINDEX, inpTable);
			FillInput(L);
			switch (hook) {
			case 3: {   // control: act.gear carries the current gear, every other field must be set by the controller
				lua_rawgeti(L, LUA_REGISTRYINDEX, actTable);
				lua_pushinteger(L, actIn->gear); lua_setfield(L, -2, "gear");
				for (size_t i = 1; i < sizeof(kControlFields) / sizeof(kControlFields[0]); ++i) SetNil(L, kControlFields[i]);
				return 4;
			}
			case 4:
				lua_rawgeti(L, LUA_REGISTRYINDEX, yawTable);
				SetNum(L, "yaw_acc", yaIn->yawAcc); SetNum(L, "ax", yaIn->ax);
				return 4;
			case 5:
				lua_rawgeti(L, LUA_REGISTRYINDEX, velTable);
				SetNum(L, "vx", velIn->vx); SetNum(L, "vy", velIn->vy);
				return 4;
			default:
				lua_rawgeti(L, LUA_REGISTRYINDEX, gearTable);
				lua_pushinteger(L, grIn->gear); lua_setfield(L, -2, "gear");
				SetNil(L, "gear_shifting");
				return 4;
			}
		}

		// Reads the controller's results and debug table (runs inside ProtectedCall).
		static int ReadOutputs(lua_State* L) {
			Read& r = *static_cast<Read*>(lua_touserdata(L, 1));
			Impl& I = *r.impl;
			switch (r.hook) {
			case 3: {
				lua_rawgeti(L, LUA_REGISTRYINDEX, I.actTable);
				const int idx = lua_gettop(L);
				r.act.gear = static_cast<int>(ReqNum(L, idx, "gear"));
				r.act.steerAngle = static_cast<float>(ReqNum(L, idx, "steer_angle"));
				r.act.driveScale = static_cast<float>(ReqNum(L, idx, "drive_scale"));
				r.act.driveShiftCut = ReqBool(L, idx, "drive_shift_cut");
				r.act.dragScale = static_cast<float>(ReqNum(L, idx, "drag_scale"));
				r.act.rearGripScale = static_cast<float>(ReqNum(L, idx, "rear_grip_scale"));
				r.act.driveThrottle = static_cast<float>(ReqNum(L, idx, "drive_throttle"));
				r.act.reverseDrive = static_cast<float>(ReqNum(L, idx, "reverse_drive"));
				r.act.brakeCommand = static_cast<float>(ReqNum(L, idx, "brake_command"));
				r.act.brakeFrontShare = static_cast<float>(ReqNum(L, idx, "brake_front_share"));
				r.act.brakeRearShare = static_cast<float>(ReqNum(L, idx, "brake_rear_share"));
				r.act.brakeAtStandstill = ReqBool(L, idx, "brake_at_standstill");
				r.act.rearDriveCut = ReqBool(L, idx, "rear_drive_cut");
				r.act.rearSpinFloor = static_cast<float>(ReqNum(L, idx, "rear_spin_floor"));
				r.act.rearLock = ReqBool(L, idx, "rear_lock");
				r.act.holdAtRest = ReqBool(L, idx, "hold_at_rest");
				lua_pop(L, 1);
				break;
			}
			case 4: {
				lua_rawgeti(L, LUA_REGISTRYINDEX, I.yawTable);
				const int idx = lua_gettop(L);
				r.ya.yawAcc = static_cast<float>(ReqNum(L, idx, "yaw_acc"));
				r.ya.ax = static_cast<float>(ReqNum(L, idx, "ax"));
				lua_pop(L, 1);
				break;
			}
			case 5: {
				lua_rawgeti(L, LUA_REGISTRYINDEX, I.velTable);
				const int idx = lua_gettop(L);
				r.vel.vx = static_cast<float>(ReqNum(L, idx, "vx"));
				r.vel.vy = static_cast<float>(ReqNum(L, idx, "vy"));
				lua_pop(L, 1);
				break;
			}
			case 6: {
				lua_rawgeti(L, LUA_REGISTRYINDEX, I.gearTable);
				const int idx = lua_gettop(L);
				r.gr.gear = static_cast<int>(ReqNum(L, idx, "gear"));
				r.gr.gearboxShifting = ReqBool(L, idx, "gear_shifting");
				lua_pop(L, 1);
				break;
			}
			default: break;
			}
			// Optional debug table of the controller (state for the facade and telemetry). Every number and
			// boolean field becomes a named channel (booleans as 1 / 0); shift_timer and shift_event are the
			// gearbox fields the backend and the facade read by name. Channels absent from the table keep their value.
			lua_rawgeti(L, LUA_REGISTRYINDEX, I.self);
			lua_getfield(L, -1, "debug");
			if (lua_type(L, -1) == LUA_TTABLE) {
				const int idx = lua_gettop(L);
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					if (lua_type(L, -2) == LUA_TSTRING) {
						const std::string key = lua_tostring(L, -2);
						const int vt = lua_type(L, -1);
						if (key == "shift_timer" && vt == LUA_TNUMBER) r.dbg.shiftTimer = static_cast<float>(lua_tonumber(L, -1));
						else if (key == "shift_event" && vt == LUA_TBOOLEAN) r.shiftEvent = lua_toboolean(L, -1) != 0;
						else if (vt == LUA_TNUMBER) r.dbg.channels[key] = static_cast<float>(lua_tonumber(L, -1));
						else if (vt == LUA_TBOOLEAN) r.dbg.channels[key] = lua_toboolean(L, -1) ? 1.0f : 0.0f;
					}
					lua_pop(L, 1);
				}
			}
			lua_settop(L, 0);
			return 0;
		}

		// Loads the controller: require(module), controller.new(params), method lookup, reused tables.
		struct Init {
			Impl* impl;
			const std::string* module;
			const json* params;
		};
		// State hooks (ProtectedCall argument: a StateCall*).
		static int ExportFn(lua_State* L) {
			StateCall& c = *static_cast<StateCall*>(lua_touserdata(L, 1));
			lua_rawgeti(L, LUA_REGISTRYINDEX, c.impl->self);
			lua_getfield(L, -1, "state");
			if (lua_type(L, -1) != LUA_TFUNCTION) { c.hook = false; return 0; }
			c.hook = true;
			lua_pushvalue(L, -2);
			lua_call(L, 1, 1);
			*c.out = TableToJson(L, -1, 0);
			return 0;
		}
		static int ImportFn(lua_State* L) {
			StateCall& c = *static_cast<StateCall*>(lua_touserdata(L, 1));
			lua_rawgeti(L, LUA_REGISTRYINDEX, c.impl->self);
			lua_getfield(L, -1, "restore");
			if (lua_type(L, -1) != LUA_TFUNCTION) { c.hook = false; return 0; }
			c.hook = true;
			lua_pushvalue(L, -2);
			PushJson(L, *c.in, 0);
			lua_call(L, 2, 0);
			return 0;
		}

		static int InitFn(lua_State* L) {
			Init& ctx = *static_cast<Init*>(lua_touserdata(L, 1));
			Impl& I = *ctx.impl;
			lua_getglobal(L, "require");
			lua_pushlstring(L, ctx.module->data(), ctx.module->size());
			lua_call(L, 1, 1);
			if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "module '%s' must return a table", ctx.module->c_str());
			lua_getfield(L, -1, "new");
			if (lua_type(L, -1) != LUA_TFUNCTION) luaL_error(L, "module '%s' needs new(params)", ctx.module->c_str());
			PushJson(L, *ctx.params, 0);
			lua_call(L, 1, 1);
			if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "new(params) must return a table");
			I.self = luaL_ref(L, LUA_REGISTRYINDEX);

			static const char* const kHookNames[kHooks] = { "on_teleport", "on_respawn", "on_hold", "control", "yaw_assist", "velocity", "post_step", "on_refuel" };
			const bool required[kHooks] = { false, false, false, true, true, true, true, false };
			for (int h = 0; h < kHooks; ++h) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, I.self);
				lua_getfield(L, -1, kHookNames[h]);
				if (lua_type(L, -1) == LUA_TFUNCTION) {
					I.refs[h] = luaL_ref(L, LUA_REGISTRYINDEX);
					lua_pop(L, 1);
				}
				else {
					if (required[h]) luaL_error(L, "controller is missing function '%s'", kHookNames[h]);
					lua_pop(L, 2);
				}
			}
			auto newTable = [&](int& ref) {
				lua_createtable(L, 0, 40);
				ref = luaL_ref(L, LUA_REGISTRYINDEX);
			};
			newTable(I.telTable);
			newTable(I.inpTable);
			newTable(I.actTable);
			newTable(I.yawTable);
			newTable(I.velTable);
			newTable(I.gearTable);
			// Telemetry and input tables get their keys now, so later writes never allocate.
			lua_rawgeti(L, LUA_REGISTRYINDEX, I.telTable);
			for (const NumField& f : kTelNumbers) SetNum(L, f.key, 0.0);
			SetNum(L, "pos_x", 0.0); SetNum(L, "pos_y", 0.0); SetNum(L, "vel_x", 0.0); SetNum(L, "vel_y", 0.0);
			lua_pushinteger(L, 0); lua_setfield(L, -2, "gear");
			lua_pushinteger(L, 0); lua_setfield(L, -2, "gear_count");
			SetBool(L, "limiter", false); SetBool(L, "airborne", false); SetBool(L, "surface", false);
			lua_pop(L, 1);
			lua_rawgeti(L, LUA_REGISTRYINDEX, I.inpTable);
			SetNum(L, "throttle", 0.0); SetNum(L, "brake", 0.0); SetNum(L, "steer", 0.0);
			SetBool(L, "handbrake", false);
			lua_newtable(L); lua_setfield(L, -2, "aux");
			lua_pop(L, 1);
			lua_settop(L, 0);
			return 0;
		}
	};

	// -----------------------------------------------------------------------------------------
	// Policy
	// -----------------------------------------------------------------------------------------
	LuaVehiclePolicy::LuaVehiclePolicy(Runtime& runtime, const std::string& moduleName, const json& params)
		: m_impl(std::make_unique<Impl>(runtime))
		, m_moduleName(moduleName) {
		if (!runtime.Ok()) {
			m_error = "Lua runtime unavailable: " + runtime.InitError();
			m_faulted = true;
			return;
		}
		Impl::Init init{ m_impl.get(), &m_moduleName, &params };
		RunError err;
		if (!runtime.ProtectedCall(&Impl::InitFn, &init, &err)) {
			m_error = FirstLine(err.message);
			Fault("load", m_error);
			return;
		}
		m_loaded = true;
		m_debug = VehiclePolicyDebug();
	}

	LuaVehiclePolicy::~LuaVehiclePolicy() {
		Runtime& rt = m_impl->rt;
		for (int r : m_impl->refs) rt.Unref(r);
		rt.Unref(m_impl->self);
		rt.Unref(m_impl->telTable);
		rt.Unref(m_impl->inpTable);
		rt.Unref(m_impl->actTable);
		rt.Unref(m_impl->yawTable);
		rt.Unref(m_impl->velTable);
		rt.Unref(m_impl->gearTable);
	}

	const VehicleCallStats& LuaVehiclePolicy::Stats(int phase) const {
		return m_stats[phase >= 0 && phase < kStatCount ? phase : 0];
	}

	void LuaVehiclePolicy::ResetStats() {
		for (VehicleCallStats& s : m_stats) s = VehicleCallStats();
	}

	void LuaVehiclePolicy::Fault(const char* phase, const std::string& message) {
		m_faulted = true;
		for (VehicleFault& f : m_faults) {
			if (f.phase == phase && f.message == message) { ++f.count; return; }
		}
		VehicleFault f;
		f.phase = phase;
		f.message = FirstLine(message);
		f.count = 1;
		m_faults.push_back(std::move(f));
	}

	// Runs one hook. Returns false when the controller is faulted or the call failed (already recorded).
	// On success the validated results are in m_impl->read.
	bool LuaVehiclePolicy::Call(Hook hook, StatPhase stat, const char* phase) {
		if (!m_loaded || m_faulted) return false;
		const Clock::time_point t0 = Clock::now();
		Impl& I = *m_impl;
		I.hook = hook == kTeleport ? 0 : hook == kRespawn ? 1 : hook == kHold ? 2 : hook == kControl ? 3 : hook == kYaw ? 4 : hook == kVelocity ? 5 : hook == kRefuel ? 7 : 6;
		I.read = Impl::Read();
		I.read.impl = &I;
		I.read.hook = I.hook;
		I.read.dbg = m_debug;
		RunError err;
		bool ok = I.rt.CallRef(I.refs[hook], I.pushArgs, 0, &err);
		if (ok) {
			ok = I.rt.ProtectedCall(&Impl::ReadOutputs, &I.read, &err);
			if (!ok) Fault("output", err.message);
			else if (I.read.shiftEvent) m_shiftEvent = true;
		}
		else {
			Fault(phase, err.budgetExceeded ? "instruction budget exceeded: " + FirstLine(err.message) : FirstLine(err.message));
		}
		const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
		VehicleCallStats& s = m_stats[stat];
		++s.calls;
		s.totalNs += ns;
		if (ns > s.maxNs) s.maxNs = ns;
		return ok;
	}

	void LuaVehiclePolicy::OnTeleport() {
		if (Call(kTeleport, kStatHook, "on_teleport")) m_debug = m_impl->read.dbg;
	}

	void LuaVehiclePolicy::OnRespawn() {
		if (Call(kRespawn, kStatHook, "on_respawn")) m_debug = m_impl->read.dbg;
	}

	void LuaVehiclePolicy::OnHold() {
		if (Call(kHold, kStatHook, "on_hold")) m_debug = m_impl->read.dbg;
	}

	bool LuaVehiclePolicy::ExportState(json& out) {
		if (!m_loaded || m_faulted) return false;
		StateCall c{ m_impl.get(), &out, nullptr, false };
		RunError err;
		if (!m_impl->rt.ProtectedCall(&Impl::ExportFn, &c, &err)) {
			Fault("state", err.message);
			return false;
		}
		return c.hook;
	}

	bool LuaVehiclePolicy::ImportState(const json& state) {
		if (!m_loaded) return false;
		StateCall c{ m_impl.get(), nullptr, &state, false };
		RunError err;
		if (!m_impl->rt.ProtectedCall(&Impl::ImportFn, &c, &err)) {
			Fault("state", err.message);
			return false;
		}
		return c.hook;
	}

	void LuaVehiclePolicy::OnRefuel() {
		if (Call(kRefuel, kStatHook, "on_refuel")) m_debug = m_impl->read.dbg;
	}

	namespace {
		// Actuation used while faulted: steering centred, no drive, a light brake (coast to a stop), no
		// shift, no boost. The gear already in `out` (the current gear) is kept.
		void SafeActuation(VehicleActuation& out) {
			out.steerAngle = 0.0f;
			out.driveScale = 1.0f;
			out.driveShiftCut = false;
			out.dragScale = 1.0f;
			out.rearGripScale = 1.0f;
			out.driveThrottle = 0.0f;
			out.reverseDrive = 0.0f;
			out.brakeCommand = LuaVehiclePolicy::kSafeBrake;
			out.brakeFrontShare = 0.68f;
			out.brakeRearShare = 0.32f;
			out.brakeAtStandstill = false;
			out.rearDriveCut = false;
			out.rearSpinFloor = 0.0f;
			out.rearLock = false;
			out.holdAtRest = false;
		}

		// Range checks of one actuation. Returns an empty string when valid.
		std::string ValidateActuation(const VehicleActuation& a, const VehicleStepTelemetry& t) {
			if (a.gear == 0 || a.gear < -1 || a.gear > t.gearCount) return "gear must be -1 or 1..gear_count";
			if (!InRange(a.steerAngle, -1.6f, 1.6f)) return "steer_angle must be a finite angle within +-1.6 rad";
			if (!InRange(a.driveScale, 0.0f, 4.0f)) return "drive_scale must be in [0, 4]";
			if (!InRange(a.dragScale, 0.0f, 4.0f)) return "drag_scale must be in [0, 4]";
			if (!InRange(a.rearGripScale, 0.0f, 2.0f)) return "rear_grip_scale must be in [0, 2]";
			if (!InRange(a.driveThrottle, 0.0f, 1.0f)) return "drive_throttle must be in [0, 1]";
			if (!InRange(a.reverseDrive, 0.0f, 1.0f)) return "reverse_drive must be in [0, 1]";
			if (!InRange(a.brakeCommand, 0.0f, 1.0f)) return "brake_command must be in [0, 1]";
			if (!InRange(a.brakeFrontShare, 0.0f, 1.0f) || !InRange(a.brakeRearShare, 0.0f, 1.0f)) return "brake shares must be in [0, 1]";
			if (!InRange(a.rearSpinFloor, 0.0f, 1.0f)) return "rear_spin_floor must be in [0, 1]";
			return std::string();
		}
	}

	void LuaVehiclePolicy::Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) {
		Impl& I = *m_impl;
		I.t = &t;
		I.in = &in;
		I.actIn = &out;
		if (Call(kControl, kStatControl, "control")) {
			const std::string why = ValidateActuation(I.read.act, t);
			if (why.empty()) {
				out = I.read.act;
				m_debug = I.read.dbg;
				return;
			}
			Fault("output", "control: " + why);
		}
		// Faulted (now or earlier): the car coasts on the safe actuation; no other policy takes over.
		SafeActuation(out);
	}

	void LuaVehiclePolicy::YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) {
		Impl& I = *m_impl;
		I.t = &t;
		I.in = &in;
		I.yaIn = &io;
		if (!Call(kYaw, kStatYaw, "yaw_assist")) return;
		const VehicleYawAssist& r = I.read.ya;
		if (!Finite(r.yawAcc) || std::abs(r.yawAcc) > 1e4f || !Finite(r.ax) || std::abs(r.ax) > 1e3f) {
			Fault("output", "yaw_assist: yaw_acc and ax must be finite and bounded");
			return;
		}
		io = r;
		m_debug = I.read.dbg;
	}

	void LuaVehiclePolicy::VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) {
		Impl& I = *m_impl;
		I.t = &t;
		I.in = &in;
		I.velIn = &io;
		if (!Call(kVelocity, kStatVelocity, "velocity")) return;
		const VehicleVelocity& r = I.read.vel;
		if (!Finite(r.vx) || !Finite(r.vy) || std::abs(r.vx) > 1000.0f || std::abs(r.vy) > 1000.0f) {
			Fault("output", "velocity: vx and vy must be finite and bounded");
			return;
		}
		io = r;
		m_debug = I.read.dbg;
	}

	void LuaVehiclePolicy::PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) {
		Impl& I = *m_impl;
		I.t = &t;
		I.in = &in;
		I.grIn = &io;
		if (!Call(kPostStep, kStatPost, "post_step")) return;
		const VehicleGearRequest& r = I.read.gr;
		if (r.gear == 0 || r.gear < -1 || r.gear > t.gearCount) {
			Fault("output", "post_step: gear must be -1 or 1..gear_count");
			return;
		}
		io = r;
		m_debug = I.read.dbg;
	}
}
