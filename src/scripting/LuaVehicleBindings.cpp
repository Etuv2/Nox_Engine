#include "LuaVehicleBindings.h"

#include "LuaRuntime.h"
#include "LuaVehiclePolicy.h"
#include "RoadNetwork.h"
#include "CollisionWorld.h"
#include "assets/AssetIO.h"

#include "SceneNode.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

using nlohmann::json;

namespace NoxLua {
	namespace {
		constexpr const char* kHandleMeta = "NoxVehicleHandle";
		constexpr float kDegToRad = 3.14159265f / 180.0f;

		const std::set<std::string> kTuningKeys = {
			"top_speed_kmh", "drive_accel", "brake_decel", "reverse_max_kmh", "mass", "cg_height", "tire_grip",
			"tire_grip_long", "rpm_idle", "rpm_limiter", "gear_ratios", "suspension_freq", "suspension_damping",
			"bump_amplitude", "roof_height",
		};
		const std::set<std::string> kFixedKeys = {
			"backend", "drive", "geometry", "rig", "spawn", "front_axle_m", "rear_axle_m", "track_m", "wheel_radius_m",
			"wheel_rest_m", "collision_radius_m", "collision_offset_m", "wheels",
		};

		// Reads the numeric tuning keys present in `t` into `out`. Unknown keys and wrong types are errors.
		bool ReadTuning(const json& t, VehicleTuning& out, std::string& error) {
			if (!t.is_object()) { error = "tuning must be an object"; return false; }
			for (auto it = t.begin(); it != t.end(); ++it) {
				if (!kTuningKeys.count(it.key())) {
					error = "unknown tuning key '" + it.key() + "'";
					return false;
				}
			}
			auto num = [&](const char* key, float& dst) -> bool {
				if (!t.contains(key)) return true;
				if (!t.at(key).is_number()) { error = std::string("tuning '") + key + "' must be a number"; return false; }
				dst = t.at(key).get<float>();
				return true;
			};
			if (!num("top_speed_kmh", out.topSpeedKmh) || !num("drive_accel", out.driveAcceleration) ||
				!num("brake_decel", out.brakeDecel) || !num("reverse_max_kmh", out.reverseMaxKmh) ||
				!num("mass", out.mass) || !num("cg_height", out.cgHeight) || !num("tire_grip", out.tireGrip) ||
				!num("tire_grip_long", out.tireGripLong) || !num("rpm_idle", out.rpmIdle) ||
				!num("rpm_limiter", out.rpmLimiter) || !num("suspension_freq", out.suspensionFreq) ||
				!num("suspension_damping", out.suspensionDamping) || !num("bump_amplitude", out.bumpAmplitude) ||
				!num("roof_height", out.roofHeight)) {
				return false;
			}
			if (t.contains("gear_ratios")) {
				if (!t.at("gear_ratios").is_array()) { error = "tuning 'gear_ratios' must be an array"; return false; }
				out.gearRatios.clear();
				for (const json& r : t.at("gear_ratios")) {
					if (!r.is_number()) { error = "gear ratios must be numbers"; return false; }
					out.gearRatios.push_back(r.get<float>());
				}
			}
			return true;
		}

		bool ReadVec3(const json& j, glm::vec3& out) {
			if (!j.is_array() || j.size() != 3) return false;
			for (int i = 0; i < 3; ++i) if (!j[i].is_number()) return false;
			out = glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
			return true;
		}

		// Geometry keys of a definition (create only).
		bool ReadGeometry(const json& g, VehicleDefinition& d, std::string& error) {
			if (!g.is_object()) { error = "geometry must be an object"; return false; }
			static const std::set<std::string> known = { "front_axle_m", "rear_axle_m", "track_m", "wheel_radius_m",
				"wheel_rest_m", "collision_radius_m", "collision_offset_m" };
			for (auto it = g.begin(); it != g.end(); ++it) {
				if (!known.count(it.key())) { error = "unknown geometry key '" + it.key() + "'"; return false; }
			}
			if (g.contains("front_axle_m")) d.frontAxleDistance = g.at("front_axle_m").get<float>();
			if (g.contains("rear_axle_m")) d.rearAxleDistance = g.at("rear_axle_m").get<float>();
			if (g.contains("track_m")) d.track = g.at("track_m").get<float>();
			if (g.contains("collision_radius_m")) d.collisionRadius = g.at("collision_radius_m").get<float>();
			if (g.contains("collision_offset_m")) d.collisionOffset = g.at("collision_offset_m").get<float>();
			if (g.contains("wheel_radius_m")) {
				const json& r = g.at("wheel_radius_m");
				if (!r.is_array() || r.size() != 4) { error = "wheel_radius_m needs 4 values (FL FR RL RR)"; return false; }
				for (int i = 0; i < 4; ++i) d.wheelRadius[i] = r[i].get<float>();
			}
			if (g.contains("wheel_rest_m")) {
				const json& r = g.at("wheel_rest_m");
				if (!r.is_array() || r.size() != 4) { error = "wheel_rest_m needs 4 points (FL FR RL RR)"; return false; }
				for (int i = 0; i < 4; ++i) {
					if (!ReadVec3(r[i], d.wheelRest[i])) { error = "wheel_rest_m entries need 3 numbers"; return false; }
				}
			}
			return true;
		}

		json LuaToJson(lua_State* L, int idx, int depth) {
			idx = lua_absindex(L, idx);
			if (depth > 8) luaL_error(L, "vehicle definition nested too deeply");
			switch (lua_type(L, idx)) {
			case LUA_TNIL: return nullptr;
			case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
			case LUA_TNUMBER:
				if (lua_isinteger(L, idx)) return static_cast<int64_t>(lua_tointeger(L, idx));
				return lua_tonumber(L, idx);
			case LUA_TSTRING: return std::string(lua_tostring(L, idx));
			case LUA_TTABLE: {
				json out = json::object();
				json arr = json::array();
				bool sequence = true;
				int64_t expected = 1;
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					// key at -2, value at -1
					if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2) && lua_tointeger(L, -2) == expected) {
						arr.push_back(LuaToJson(L, -1, depth + 1));
						++expected;
					}
					else if (lua_type(L, -2) == LUA_TSTRING) {
						sequence = false;
						out[lua_tostring(L, -2)] = LuaToJson(L, -1, depth + 1);
					}
					else {
						luaL_error(L, "vehicle table keys must be strings or 1..n");
					}
					lua_pop(L, 1);
				}
				if (sequence && out.empty()) return arr;
				if (!arr.empty()) luaL_error(L, "vehicle table mixes array and named entries");
				return out;
			}
			default:
				luaL_error(L, "vehicle values must be numbers, strings, booleans or tables");
				return nullptr;
			}
		}

		void JsonToLua(lua_State* L, const json& j) {
			switch (j.type()) {
			case json::value_t::null: lua_pushnil(L); return;
			case json::value_t::boolean: lua_pushboolean(L, j.get<bool>() ? 1 : 0); return;
			case json::value_t::number_integer:
			case json::value_t::number_unsigned: lua_pushinteger(L, static_cast<lua_Integer>(j.get<int64_t>())); return;
			case json::value_t::number_float: lua_pushnumber(L, j.get<double>()); return;
			case json::value_t::string: {
				const std::string s = j.get<std::string>();
				lua_pushlstring(L, s.data(), s.size());
				return;
			}
			case json::value_t::array: {
				lua_createtable(L, static_cast<int>(j.size()), 0);
				int i = 1;
				for (const json& e : j) { JsonToLua(L, e); lua_rawseti(L, -2, i++); }
				return;
			}
			case json::value_t::object: {
				lua_createtable(L, 0, static_cast<int>(j.size()));
				for (auto it = j.begin(); it != j.end(); ++it) {
					JsonToLua(L, it.value());
					lua_setfield(L, -2, it.key().c_str());
				}
				return;
			}
			default: lua_pushnil(L); return;
			}
		}

		json WheelsJson(const VehicleState& s) {
			json out = json::array();
			for (const VehicleWheelState& w : s.wheels) {
				json j = json::object();
				j["contact"] = json::array({ w.contact.x, w.contact.y, w.contact.z });
				j["velocity"] = json::array({ w.velocity.x, w.velocity.z });
				j["slip"] = w.slip;
				j["wheelspin"] = w.wheelspin;
				j["load"] = w.load;
				j["grounded"] = w.grounded;
				j["suspension"] = w.suspension;
				j["spin_angle"] = w.spinAngle;
				j["steer_angle"] = w.steerAngle;
				out.push_back(std::move(j));
			}
			return out;
		}

		struct HandleBox { VehicleHandle handle; };

		void PushHandle(lua_State* L, VehicleHandle h) {
			HandleBox* box = static_cast<HandleBox*>(lua_newuserdatauv(L, sizeof(HandleBox), 0));
			box->handle = h;
			luaL_setmetatable(L, kHandleMeta);
		}

		VehicleHost* HostOf(lua_State* L) {
			return static_cast<VehicleHost*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		// A handle argument: must be a vehicle handle that is still live (stale handles raise, like the nox API).
		VehicleHandle CheckHandle(lua_State* L, VehicleHost* host, int idx) {
			HandleBox* box = static_cast<HandleBox*>(luaL_testudata(L, idx, kHandleMeta));
			if (!box) luaL_argerror(L, idx, "vehicle handle expected");
			if (!host->IsValid(box->handle)) luaL_error(L, "stale or invalid vehicle handle");
			return box->handle;
		}

		float NumField(lua_State* L, int idx, const char* key, float fallback, float lo, float hi) {
			lua_getfield(L, idx, key);
			float v = fallback;
			if (!lua_isnil(L, -1)) {
				if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "input field '%s' must be a number", key);
				v = static_cast<float>(lua_tonumber(L, -1));
				if (!(v >= lo && v <= hi)) luaL_error(L, "input field '%s' must be in [%g, %g]", key, lo, hi);
			}
			lua_pop(L, 1);
			return v;
		}
		bool BoolField(lua_State* L, int idx, const char* key) {
			lua_getfield(L, idx, key);
			const bool v = !lua_isnil(L, -1) && lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return v;
		}

		// --- Lua functions (upvalue 1: VehicleHost*) ---------------------------------------------
		int Vehicle_create(lua_State* L) {
			VehicleHost* host = HostOf(L);
			if (lua_type(L, 1) != LUA_TTABLE) return luaL_argerror(L, 1, "table expected");
			json opts = LuaToJson(L, 1, 0);
			json spec = json::object();
			if (opts.contains("definition")) {
				if (!opts["definition"].is_string()) return luaL_argerror(L, 1, "definition must be a logical path");
				const std::string path = opts["definition"].get<std::string>();
				std::string text, why;
				ScriptSource* files = host->Files();
				if (!files || !files->ReadText(path, text, why)) {
					lua_pushnil(L);
					lua_pushstring(L, ("cannot read definition '" + path + "': " + why).c_str());
					return 2;
				}
				spec = json::parse(text, nullptr, false);
				if (!spec.is_object()) {
					lua_pushnil(L);
					lua_pushstring(L, ("definition '" + path + "' is not a JSON object").c_str());
					return 2;
				}
			}
			for (auto it = opts.begin(); it != opts.end(); ++it) {
				if (it.key() != "definition") spec[it.key()] = it.value();   // shallow overlay of the call's fields
			}
			std::string error;
			const VehicleHandle h = host->Create(spec, host->Resolver(), &error);
			if (!h.Valid()) {
				lua_pushnil(L);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			PushHandle(L, h);
			return 1;
		}

		int Vehicle_destroy(lua_State* L) {
			VehicleHost* host = HostOf(L);
			HandleBox* box = static_cast<HandleBox*>(luaL_testudata(L, 1, kHandleMeta));
			if (box) host->Destroy(box->handle);   // stale handles: nothing to do
			return 0;
		}

		int Vehicle_valid(lua_State* L) {
			VehicleHost* host = HostOf(L);
			HandleBox* box = static_cast<HandleBox*>(luaL_testudata(L, 1, kHandleMeta));
			lua_pushboolean(L, box && host->IsValid(box->handle) ? 1 : 0);
			return 1;
		}

		int Vehicle_count(lua_State* L) {
			lua_pushinteger(L, static_cast<lua_Integer>(HostOf(L)->Count()));
			return 1;
		}

		int Vehicle_step(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const float dt = static_cast<float>(luaL_checknumber(L, 1));
			if (!(dt >= 0.0f && dt <= 1.0f)) return luaL_argerror(L, 1, "dt must be in [0, 1] seconds");
			host->Step(dt, lua_isboolean(L, 2) && lua_toboolean(L, 2) != 0);
			return 0;
		}

		int Vehicle_set_input(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			luaL_checktype(L, 2, LUA_TTABLE);
			VehicleInput in;
			in.throttle = NumField(L, 2, "throttle", 0.0f, 0.0f, 1.0f);
			in.brake = NumField(L, 2, "brake", 0.0f, 0.0f, 1.0f);
			in.steer = NumField(L, 2, "steer", 0.0f, -1.0f, 1.0f);
			in.handbrake = BoolField(L, 2, "handbrake");
			// aux = { name = number | boolean }: named auxiliary inputs read by the policy (booleans as 1 / 0).
			lua_getfield(L, 2, "aux");
			if (lua_type(L, -1) == LUA_TTABLE) {
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					const int vt = lua_type(L, -1);
					if (lua_type(L, -2) != LUA_TSTRING || (vt != LUA_TNUMBER && vt != LUA_TBOOLEAN)) {
						lua_settop(L, 0);
						return luaL_argerror(L, 2, "aux entries must be name = number or boolean");
					}
					in.aux[lua_tostring(L, -2)] = vt == LUA_TBOOLEAN ? (lua_toboolean(L, -1) ? 1.0f : 0.0f)
						: static_cast<float>(lua_tonumber(L, -1));
					lua_pop(L, 1);
				}
			}
			lua_settop(L, 2);
			host->SetInput(h, in);
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_telemetry(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			JsonToLua(L, host->Telemetry(h));
			return 1;
		}

		int Vehicle_teleport(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			const glm::vec2 pos(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)));
			const float heading = static_cast<float>(luaL_optnumber(L, 4, 0.0)) * kDegToRad;
			std::string error;
			if (!host->Teleport(h, pos, heading, &error)) luaL_error(L, "%s", error.c_str());
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_recover(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			host->RecoverToRoad(h);
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_refuel(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			host->Refuel(h);
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_wheels(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			JsonToLua(L, host->Wheels(h));
			return 1;
		}

		int Vehicle_list(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const std::vector<VehicleHandle> handles = host->Handles();
			lua_createtable(L, static_cast<int>(handles.size()), 0);
			int i = 1;
			for (const VehicleHandle& h : handles) {
				PushHandle(L, h);
				lua_rawseti(L, -2, i++);
			}
			return 1;
		}

		int Vehicle_reset(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			host->Reset(h);
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_set_controller(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			const std::string module = luaL_checkstring(L, 2);
			json params;
			bool given = false;
			if (!lua_isnoneornil(L, 3)) {
				if (lua_type(L, 3) != LUA_TTABLE) return luaL_argerror(L, 3, "params must be a table");
				params = LuaToJson(L, 3, 0);
				given = true;
			}
			std::string error;
			if (!host->SetController(h, module, given ? &params : nullptr, &error)) {
				lua_pushnil(L);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_set_profile(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			if (lua_type(L, 2) != LUA_TTABLE) return luaL_argerror(L, 2, "profile must be a table");
			std::string error;
			if (!host->SetProfile(h, LuaToJson(L, 2, 0), &error)) {
				lua_pushnil(L);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			lua_pushboolean(L, 1);
			return 1;
		}

		int Vehicle_faults(lua_State* L) {
			VehicleHost* host = HostOf(L);
			const VehicleHandle h = CheckHandle(L, host, 1);
			const std::vector<std::string> faults = host->Faults(h);
			lua_createtable(L, static_cast<int>(faults.size()), 0);
			int i = 1;
			for (const std::string& f : faults) {
				lua_pushlstring(L, f.data(), f.size());
				lua_rawseti(L, -2, i++);
			}
			return 1;
		}

		int Handle_tostring(lua_State* L) {
			HandleBox* box = static_cast<HandleBox*>(luaL_testudata(L, 1, kHandleMeta));
			if (!box) return luaL_error(L, "vehicle handle expected");
			lua_pushfstring(L, "vehicle(%d:%d)", static_cast<int>(box->handle.id), static_cast<int>(box->handle.incarnation));
			return 1;
		}
	}

	// -----------------------------------------------------------------------------------------
	// VehicleHost
	// -----------------------------------------------------------------------------------------
	// NOX_VEHICLE_TELEMETRY=<file.jsonl> (R-S16): one record per vehicle sub-step, the same line format and %.9g
	// values as DriveMode::RecordTelemetry, so a Lua-controlled run compares record by record with a native run.
	// The file is opened once per process (a reload keeps appending to it, it is not truncated again).
	static void RecordVehicleTelemetry(const VehicleSubstepRecord& r)
	{
		static std::FILE* file = [] {
			const char* path = std::getenv("NOX_VEHICLE_TELEMETRY");
			return (path && *path) ? std::fopen(path, "w") : static_cast<std::FILE*>(nullptr);
		}();
		if (!file) return;
		const VehicleState& s = r.state;
		const VehiclePolicyDebug& p = r.policy;
		// Record keys are the parity format (tools/VehicleParity.ps1 baselines): the legacy names are kept,
		// but their values come from the generic aux input and policy channels.
		std::fprintf(file,
			"{\"t\":%.9g,\"h\":%.9g,\"thr\":%.9g,\"brk\":%.9g,\"steer\":%.9g,\"hb\":%d,\"nos\":%d,"
			"\"x\":%.9g,\"y\":%.9g,\"z\":%.9g,\"head\":%.9g,\"vx\":%.9g,\"vy\":%.9g,\"yawRate\":%.9g,"
			"\"gear\":%d,\"rpm\":%.9g,\"drift\":%.9g,\"driftSign\":%.9g,\"nitro\":%.9g,\"nitroCooldown\":%.9g,"
			"\"slip\":%.9g,\"rearSpin\":%.9g,\"shiftTimer\":%.9g,\"airborne\":%d}\n",
			s.time, r.h, r.in.throttle, r.in.brake, r.in.steer, r.in.handbrake ? 1 : 0, r.in.Aux("nitrous") > 0.5f ? 1 : 0,
			s.pos.x, s.surface ? s.y : 0.0f, s.pos.y, s.heading, s.vx, s.vy, s.yawRate,
			s.gear, s.rpm, p.Channel("drift"), p.Channel("drift_sign", 1.0f), p.Channel("nitro", 1.0f), p.Channel("nitro_cooldown"),
			s.slip, s.rearSpin, p.shiftTimer, s.airborne ? 1 : 0);
	}

	VehicleHost::VehicleHost(Runtime& runtime, ScriptSource* files)
		: m_runtime(runtime), m_files(files) {}

	VehicleHost::~VehicleHost() {
		// Controllers reference the runtime's registry; release them before the service stops calling them.
		m_entries.clear();
	}

	VehicleHost::Entry* VehicleHost::Find(VehicleHandle h) {
		auto it = m_entries.find(h.id);
		if (it == m_entries.end() || it->second.handle != h) return nullptr;
		return &it->second;
	}

	const VehicleHost::Entry* VehicleHost::Find(VehicleHandle h) const {
		auto it = m_entries.find(h.id);
		if (it == m_entries.end() || it->second.handle != h) return nullptr;
		return &it->second;
	}

	std::unique_ptr<LuaVehiclePolicy> VehicleHost::MakePolicy(const std::string& module, const json& params) {
		return std::make_unique<LuaVehiclePolicy>(m_runtime, module, params);
	}

	// World inputs of the planar backend, the same data DriveMode::Configure reads from a scene (generic keys):
// "obstacles": logical path of a layout JSON with an "obstacles" array (box or circle entries, as the
// district layout files), "road_network": the inline network form (scene game.road_network), and
// "drivable_areas": [[x0, z0, x1, z1], ...]. The network is owned by the caller (the host entry).
static bool ReadWorld(const json& world, VehicleDefinition& def, std::shared_ptr<RoadNetwork>& roads,
	std::shared_ptr<CollisionWorld>& collision, std::string& why) {
	if (!world.is_object()) { why = "world must be an object"; return false; }
	for (auto it = world.begin(); it != world.end(); ++it) {
		static const std::set<std::string> known = { "obstacles", "road_network", "drivable_areas", "collision", "comment" };
		if (!known.count(it.key())) { why = "unknown world key '" + it.key() + "'"; return false; }
	}
	if (world.contains("obstacles")) {
		if (!world.at("obstacles").is_string()) { why = "world.obstacles must be a logical path"; return false; }
		const std::string path = world.at("obstacles").get<std::string>();
		const std::optional<std::string> text = AssetIO::ReadText(path);
		if (!text) { why = "world.obstacles: cannot read " + path; return false; }
		const json layout = json::parse(*text, nullptr, false);
		if (!layout.is_object() || !layout.contains("obstacles") || !layout["obstacles"].is_array()) {
			why = "world.obstacles: " + path + " has no obstacles array";
			return false;
		}
		for (const json& o : layout["obstacles"]) {
			VehicleObstacle ob;
			if (o.value("type", std::string("circle")) == "box") {
				ob.box = true;
				const json c = o.value("center", json::array({ 0.0f, 0.0f }));
				const json h = o.value("half", json::array({ 0.5f, 0.5f }));
				ob.center = glm::vec2(c[0].get<float>(), c[1].get<float>());
				ob.half = glm::vec2(h[0].get<float>(), h[1].get<float>());
				ob.yaw = o.value("yaw", 0.0f);
			}
			else {
				const json c = o.value("pos", json::array({ 0.0f, 0.0f }));
				ob.center = glm::vec2(c[0].get<float>(), c[1].get<float>());
				ob.radius = o.value("radius", 0.2f);
			}
			def.obstacles.push_back(ob);
		}
	}
	if (world.contains("road_network")) {
		if (!world.at("road_network").is_object()) { why = "world.road_network must be an object"; return false; }
		auto net = std::make_shared<RoadNetwork>();
		if (!net->Load(world.at("road_network"))) { why = "world.road_network has no usable nodes and edges"; return false; }
		roads = net;
		def.roads = net.get();
	}
	// Static collision (stacked roads, tunnels, barriers): the same glTF set and sliver rules native GameSession
	// builds from the scene's game.collision. The host owns the world; the service gets a non-owning pointer.
	if (world.contains("collision")) {
		const json& c = world.at("collision");
		if (!c.is_object()) { why = "world.collision must be an object"; return false; }
		auto col = std::make_shared<CollisionWorld>();
		CollisionWorld::TriangleFilter filter;
		filter.groundMinNormalY = c.value("ground_min_normal_y", filter.groundMinNormalY);
		filter.minWallHeight = c.value("wall_min_height", 0.15f);
		filter.minWallAspect = c.value("wall_min_aspect", 0.05f);
		col->filter = filter;
		if (c.contains("files") && c.at("files").is_array()) {
			for (const json& f : c.at("files")) {
				if (f.is_string()) col->LoadGLTF(f.get<std::string>());
			}
		}
		if (!col->Empty()) {
			collision = col;
			def.collision = col.get();
		}
	}
	if (world.contains("drivable_areas") && world.at("drivable_areas").is_array()) {
		for (const json& a : world.at("drivable_areas")) {
			if (a.is_array() && a.size() >= 4) {
				def.areas.push_back({ glm::vec2(a[0].get<float>(), a[1].get<float>()), glm::vec2(a[2].get<float>(), a[3].get<float>()) });
			}
		}
	}
	return true;
}

VehicleHandle VehicleHost::Create(const json& spec, const NodeResolver& resolve, std::string* error) {
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return VehicleHandle();
		};
		if (!spec.is_object()) return fail("vehicle definition must be an object");
		const json controller = spec.value("controller", json::object());
		const std::string module = controller.is_object() ? controller.value("module", std::string()) : std::string();
		if (module.empty()) return fail("a vehicle needs a controller module (there is no native default policy)");
		const json params = controller.value("params", json::object());

		VehicleDefinition def;
		def.name = spec.value("name", std::string("vehicle"));
		def.backend = spec.value("backend", std::string("two_axle_planar"));
		def.drive = spec.value("drive", std::string("rear"));
		std::string why;
		if (spec.contains("geometry") && !ReadGeometry(spec.at("geometry"), def, why)) return fail(why);
		if (spec.contains("tuning") && !ReadTuning(spec.at("tuning"), def.tuning, why)) return fail(why);
		for (auto it = spec.begin(); it != spec.end(); ++it) {
			// Metadata keys (format, id, comment, presentation) are project data: accepted, not read here.
			static const std::set<std::string> known = { "format", "id", "comment", "presentation", "name", "backend",
				"drive", "geometry", "tuning", "rig", "spawn", "controller", "definition", "world" };
			if (!known.count(it.key())) return fail("unknown definition key '" + it.key() + "'");
		}

		// Rig node references (names resolved by the project's scene). Missing names leave the slot empty.
		if (spec.contains("rig")) {
			const json rig = spec.at("rig");
			if (!rig.is_object()) return fail("rig must be an object");
			auto node = [&](const char* key) -> std::weak_ptr<SceneNode> {
				if (!rig.contains(key) || !rig.at(key).is_string() || !resolve) return {};
				return resolve(rig.at(key).get<std::string>());
			};
			def.rig.root = node("root");
			def.rig.model = node("model");
			def.rig.body = node("body");
			if (rig.contains("wheels")) {
				const json w = rig.at("wheels");
				if (!w.is_array() || w.size() != 4) return fail("rig.wheels needs 4 node names (FL FR RL RR)");
				for (int i = 0; i < 4; ++i) {
					if (!w[i].is_string() || !resolve) continue;
					def.rig.wheels[i] = resolve(w[i].get<std::string>());
				}
				const bool allWheels = !def.rig.wheels[0].expired() && !def.rig.wheels[1].expired() &&
					!def.rig.wheels[2].expired() && !def.rig.wheels[3].expired();
				def.wheelRig = allWheels && !def.rig.body.expired();
				if (def.wheelRig && !(spec.contains("geometry") && spec.at("geometry").contains("wheel_rest_m"))) {
					return fail("a wheel rig needs geometry.wheel_rest_m");
				}
			}
		}
		if (spec.contains("spawn")) {
			const json s = spec.at("spawn");
			if (!s.is_object()) return fail("spawn must be an object");
			def.spawnPos = glm::vec2(s.value("x", 0.0f), s.value("z", 0.0f));
			def.spawnY = s.value("y", 1e4f);
			def.spawnHeading = s.value("heading_deg", 0.0f) * kDegToRad;
		}

		std::shared_ptr<RoadNetwork> roads;
		std::shared_ptr<CollisionWorld> collision;
		if (spec.contains("world") && !ReadWorld(spec.at("world"), def, roads, collision, why)) return fail(why);

		std::unique_ptr<LuaVehiclePolicy> policy = MakePolicy(module, params);
		if (!policy->Ok()) return fail("controller '" + module + "' did not load: " + policy->Error());
		std::string serviceError;
		const VehicleHandle h = m_service.Create(def, policy.get(), &serviceError);
		if (!h.Valid()) return fail(serviceError);
		Entry entry;
		entry.handle = h;
		entry.controllerParams = params;
		entry.policy = std::move(policy);
		entry.tuning = def.tuning;
		entry.roads = roads;
		entry.collision = collision;
		static const bool recording = [] { const char* path = std::getenv("NOX_VEHICLE_TELEMETRY"); return path && *path; }();
		if (recording) m_service.SetSubstepObserver(h, &RecordVehicleTelemetry);
		m_entries[h.id] = std::move(entry);
		if (error) error->clear();
		return h;
	}

	void VehicleHost::Destroy(VehicleHandle h) {
		if (!Find(h)) return;
		m_service.Destroy(h);
		m_entries.erase(h.id);
	}

	bool VehicleHost::SetInput(VehicleHandle h, const VehicleInput& in) {
		Entry* e = Find(h);
		if (!e) return false;
		e->input = in;
		return true;
	}

	void VehicleHost::Step(float dt, bool frozen) {
		for (auto& kv : m_entries) m_service.Advance(kv.second.handle, dt, kv.second.input, frozen);
	}

	bool VehicleHost::Teleport(VehicleHandle h, const glm::vec2& pos, float headingRad, std::string* error) {
		if (!Find(h)) { if (error) *error = "stale or invalid vehicle handle"; return false; }
		m_service.Teleport(h, pos, headingRad);
		if (error) error->clear();
		return true;
	}

	bool VehicleHost::Reset(VehicleHandle h) {
		if (!Find(h)) return false;
		m_service.ResetToSpawn(h);
		return true;
	}

	bool VehicleHost::SetController(VehicleHandle h, const std::string& module, const json* params, std::string* error) {
		Entry* e = Find(h);
		if (!e) { if (error) *error = "stale or invalid vehicle handle"; return false; }
		if (params) e->controllerParams = *params;
		std::unique_ptr<LuaVehiclePolicy> policy = MakePolicy(module, e->controllerParams);
		m_service.SetPolicy(h, policy.get());
		e->policy = std::move(policy);   // the previous controller is released after the swap
		if (!e->policy->Ok()) {
			if (error) *error = "controller '" + module + "' did not load: " + e->policy->Error();
			return false;
		}
		if (error) error->clear();
		return true;
	}

	bool VehicleHost::RecoverToRoad(VehicleHandle h) {
		if (!Find(h)) return false;
		m_service.RecoverToRoad(h);
		return true;
	}

	bool VehicleHost::Refuel(VehicleHandle h) {
		Entry* e = Find(h);
		if (!e) return false;
		e->policy->OnRefuel();
		return true;
	}

	bool VehicleHost::SetProfile(VehicleHandle h, const json& profile, std::string* error) {
		Entry* e = Find(h);
		if (!e) { if (error) *error = "stale or invalid vehicle handle"; return false; }
		if (!profile.is_object()) { if (error) *error = "profile must be an object"; return false; }
		for (auto it = profile.begin(); it != profile.end(); ++it) {
			if (kFixedKeys.count(it.key())) {
				if (error) *error = "'" + it.key() + "' is fixed for a live vehicle: restart Play to change the geometry or rig";
				return false;
			}
		}
		VehicleTuning next = e->tuning;
		std::string why;
		if (!ReadTuning(profile, next, why)) { if (error) *error = why; return false; }
		if (!m_service.SetTuning(h, next, &why)) { if (error) *error = why; return false; }
		e->tuning = next;
		if (error) error->clear();
		return true;
	}

	json VehicleHost::Telemetry(VehicleHandle h) const {
		const Entry* e = Find(h);
		if (!e) return json::object();
		const VehicleState s = m_service.State(h);
		const VehiclePolicyDebug d = e->policy->Debug();
		json out = json::object();
		out["x"] = s.pos.x;
		out["z"] = s.pos.y;
		out["heading_deg"] = s.heading / kDegToRad;
		out["vx"] = s.vx;
		out["vy"] = s.vy;
		out["speed_kmh"] = glm::length(s.vel) * 3.6f;
		out["y"] = s.y;                                  // ground height under the CG (surface mode; 0 planar)
		out["yaw_rate"] = s.yawRate;
		out["beta"] = std::atan2(s.vy, s.vx);            // body slip angle (rad), left positive; 0 when still
		out["ax"] = s.ax;                                // filtered local accelerations (m/s^2)
		out["ay"] = s.ay;
		out["speed01"] = e->tuning.topSpeedKmh > 0.0f ? glm::length(s.vel) * 3.6f / e->tuning.topSpeedKmh : 0.0f;
		out["steer_deg"] = s.steerAngle / kDegToRad;
		out["gear"] = s.gear;
		out["rpm"] = s.rpm;
		out["limiter"] = s.limiter;
		out["slip"] = s.slip;
		out["load"] = s.load;
		out["airborne"] = s.airborne;
		out["rear_spin"] = s.rearSpin;
		out["throttle"] = s.throttle;
		out["impact"] = s.impact;
		out["heave"] = s.heave;
		out["wheels"] = WheelsJson(s);
		json channels = json::object();
		for (const auto& kv : d.channels) channels[kv.first] = kv.second;
		out["channels"] = channels;                      // policy channels by name (flags as 1 / 0)
		out["shift_timer"] = d.shiftTimer;
		out["faulted"] = e->policy->Faulted();
		return out;
	}

	void VehicleHost::AdoptFrom(VehicleHost& old) {
		if (this == &old) return;
		m_entries.clear();                       // candidate-created vehicles are replaced by the running ones
		m_service.MoveFrom(old.m_service);
		for (auto& kv : old.m_entries) {
			Entry& prev = kv.second;
			json state;
			const bool hasState = prev.policy && prev.policy->ExportState(state);
			std::unique_ptr<LuaVehiclePolicy> next = MakePolicy(prev.policy->ModuleName(), prev.controllerParams);
			if (hasState && next->Ok()) next->ImportState(state);
			if (prev.policy) next->AdoptDebug(prev.policy->Debug());
			m_service.SetPolicy(prev.handle, next.get());
			Entry e;
			e.handle = prev.handle;
			e.controllerParams = prev.controllerParams;
			e.policy = std::move(next);
			e.input = prev.input;
			e.tuning = prev.tuning;
			e.roads = prev.roads;                // the instance keeps pointing at these (non-owning in the service)
			e.collision = prev.collision;        // ...and at this collision world; dropping it frees both under the car
			m_entries[kv.first] = std::move(e);
		}
		old.m_entries.clear();                   // releases the old controllers while their VM is still alive
	}

	std::vector<VehicleHandle> VehicleHost::Handles() const {
		std::vector<VehicleHandle> out;
		for (const auto& kv : m_entries) out.push_back(kv.second.handle);
		return out;
	}

	json VehicleHost::Wheels(VehicleHandle h) const {
		if (!Find(h)) return json::array();
		return WheelsJson(m_service.State(h));
	}

	std::vector<std::string> VehicleHost::Faults(VehicleHandle h) const {
		std::vector<std::string> out;
		const Entry* e = Find(h);
		if (!e) return out;
		for (const VehicleFault& f : e->policy->Faults()) {
			out.push_back(f.phase + " (x" + std::to_string(f.count) + "): " + f.message);
		}
		return out;
	}

	const LuaVehiclePolicy* VehicleHost::Controller(VehicleHandle h) const {
		const Entry* e = Find(h);
		return e ? e->policy.get() : nullptr;
	}

	// -----------------------------------------------------------------------------------------
	// Lua installation
	// -----------------------------------------------------------------------------------------
	void InstallVehicleApi(lua_State* L, int ctxIndex, VehicleHost* host) {
		ctxIndex = lua_absindex(L, ctxIndex);
		if (luaL_newmetatable(L, kHandleMeta)) {
			lua_pushcfunction(L, Handle_tostring);
			lua_setfield(L, -2, "__tostring");
			lua_pushliteral(L, "vehicle");
			lua_setfield(L, -2, "__name");
		}
		lua_pop(L, 1);

		struct Fn { const char* name; lua_CFunction fn; };
		static const Fn kFns[] = {
			{ "create", Vehicle_create }, { "destroy", Vehicle_destroy }, { "valid", Vehicle_valid },
			{ "count", Vehicle_count }, { "step", Vehicle_step }, { "set_input", Vehicle_set_input },
			{ "telemetry", Vehicle_telemetry }, { "teleport", Vehicle_teleport }, { "reset", Vehicle_reset },
			{ "set_controller", Vehicle_set_controller }, { "set_profile", Vehicle_set_profile },
			{ "faults", Vehicle_faults }, { "recover", Vehicle_recover }, { "refuel", Vehicle_refuel },
			{ "wheels", Vehicle_wheels }, { "list", Vehicle_list },
		};
		lua_newtable(L);
		for (const Fn& f : kFns) {
			lua_pushlightuserdata(L, host);
			lua_pushcclosure(L, f.fn, 1);
			lua_setfield(L, -2, f.name);
		}
		lua_setfield(L, ctxIndex, "vehicle");
	}
}
