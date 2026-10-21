#include "LuaPhysics.h"

#include "../PhysicsEngine.h"
#include "../physics/ShapeSweep.h"
#include <glm/gtx/quaternion.hpp>
#include "../RigidBody.h"
#include "../SceneNode.h"
#include "../reflect/ReflectServices.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace NoxLua {

	namespace {

		constexpr const char* kHandleMeta = "nox.entity";   // the handle metatable LuaWorld installs
		constexpr double kMaxRayDistance = 100000.0;
		constexpr uint32_t kAllLayers = 0xFFFFFFFFu;
		const char* const kAxisKeys[3] = { "x", "y", "z" };

		LuaWorld* WorldOf(lua_State* L) {
			return static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		void SetFn(lua_State* L, LuaWorld* w, int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, w);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, tableIdx, name);
		}

		void PushHandle(lua_State* L, const EntityHandle& h) {
			void* mem = lua_newuserdatauv(L, sizeof(EntityHandle), 0);
			std::memcpy(mem, &h, sizeof(EntityHandle));
			luaL_setmetatable(L, kHandleMeta);
		}

		// Live handle of an entity of this world; false for INVALID_ENTITY or a dead entity.
		bool HandleFor(LuaWorld* w, EntityID entity, EntityHandle& out) {
			if (entity == INVALID_ENTITY || !w->Manager()) return false;
			out = w->Manager()->MakeHandle(entity);
			return w->Manager()->IsHandleValid(out);
		}

		// {x=, y=, z=} or a 1-based array of three numbers.
		bool ReadVec3(lua_State* L, int idx, glm::vec3& out) {
			idx = lua_absindex(L, idx);
			if (lua_type(L, idx) != LUA_TTABLE) return false;
			float c[3] = { 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, idx, kAxisKeys[i]);
				if (lua_type(L, -1) != LUA_TNUMBER) {
					lua_pop(L, 1);
					lua_geti(L, idx, i + 1);
				}
				if (lua_type(L, -1) != LUA_TNUMBER) {
					lua_pop(L, 1);
					return false;
				}
				c[i] = static_cast<float>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			out = glm::vec3(c[0], c[1], c[2]);
			return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
		}

		void PushVec3(lua_State* L, const glm::vec3& v) {
			lua_createtable(L, 0, 3);
			lua_pushnumber(L, v.x);
			lua_setfield(L, -2, "x");
			lua_pushnumber(L, v.y);
			lua_setfield(L, -2, "y");
			lua_pushnumber(L, v.z);
			lua_setfield(L, -2, "z");
		}

		// Optional layer mask argument: nil or absent means every layer.
		uint32_t MaskArg(lua_State* L, int idx) {
			if (lua_isnoneornil(L, idx)) return kAllLayers;
			const lua_Integer mask = luaL_checkinteger(L, idx);
			if (mask < 0 || mask > static_cast<lua_Integer>(kAllLayers)) luaL_argerror(L, idx, "mask must be in [0, 4294967295]");
			return static_cast<uint32_t>(mask);
		}

		// ctx:physics_raycast(origin, dir, max_dist, mask) -> { entity = handle|nil, distance, point, normal } or nil.
		// The nearest body hit whose layer intersects mask. Reads only: valid in any callback.
		int CtxPhysicsRaycast(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			glm::vec3 origin(0.0f), dir(0.0f);
			if (!ReadVec3(L, 2, origin)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			if (!ReadVec3(L, 3, dir)) return luaL_argerror(L, 3, "expected a vector {x, y, z}");
			const double maxDist = luaL_checknumber(L, 4);
			if (!(maxDist > 0.0) || maxDist > kMaxRayDistance) return luaL_argerror(L, 4, "max_dist must be in (0, 100000]");
			if (!(glm::length(dir) > 1e-6f)) return luaL_argerror(L, 3, "direction must not be zero");
			const uint32_t mask = MaskArg(L, 5);

			PhysicsEngine* physics = nox::reflect::Services().physics;
			PhysicsRayHit hit;
			if (!physics || !physics->RayCastClosest(origin, dir, static_cast<float>(maxDist), mask, hit)) {
				lua_pushnil(L);
				return 1;
			}
			lua_createtable(L, 0, 4);
			EntityHandle entity;
			const std::shared_ptr<SceneNode> node = hit.body ? hit.body->getAttachedNode() : nullptr;
			if (node && HandleFor(w, node->GetEntityID(), entity)) {
				PushHandle(L, entity);
				lua_setfield(L, -2, "entity");
			}
			lua_pushnumber(L, hit.distance);
			lua_setfield(L, -2, "distance");
			PushVec3(L, hit.point);
			lua_setfield(L, -2, "point");
			PushVec3(L, hit.normal);
			lua_setfield(L, -2, "normal");
			return 1;
		}

		// Shape casts: the first body a sphere, capsule or box touches when moved along dir. Returns
		// { entity = handle|nil, distance, point, normal } or nil. Read only, valid in any callback.
		void PushSweepHit(lua_State* L, LuaWorld* w, const ShapeSweep::Hit& hit) {
			lua_createtable(L, 0, 4);
			EntityHandle entity;
			const std::shared_ptr<SceneNode> node = hit.body ? hit.body->getAttachedNode() : nullptr;
			if (node && HandleFor(w, node->GetEntityID(), entity)) {
				PushHandle(L, entity);
				lua_setfield(L, -2, "entity");
			}
			lua_pushnumber(L, hit.distance);
			lua_setfield(L, -2, "distance");
			PushVec3(L, hit.point);
			lua_setfield(L, -2, "point");
			PushVec3(L, hit.normal);
			lua_setfield(L, -2, "normal");
		}

		// Common arguments of the sweeps: direction (any length, not zero), max distance and layer mask.
		bool ReadSweepArgs(lua_State* L, int dirIdx, int distIdx, int maskIdx, glm::vec3& dir, double& maxDist, uint32_t& mask) {
			if (!ReadVec3(L, dirIdx, dir)) return false;
			maxDist = luaL_checknumber(L, distIdx);
			if (!(maxDist > 0.0) || maxDist > kMaxRayDistance) luaL_argerror(L, distIdx, "max_dist must be in (0, 100000]");
			if (!(glm::length(dir) > 1e-6f)) luaL_argerror(L, dirIdx, "direction must not be zero");
			mask = MaskArg(L, maskIdx);
			return true;
		}

		int RunSweep(lua_State* L, const ShapeSweep::Probe& probe, const glm::vec3& dir, double maxDist, uint32_t mask) {
			LuaWorld* w = WorldOf(L);
			PhysicsEngine* physics = nox::reflect::Services().physics;
			ShapeSweep::Hit hit;
			if (!physics || !ShapeSweep::Sweep(*physics, probe, dir, static_cast<float>(maxDist), mask, hit)) {
				lua_pushnil(L);
				return 1;
			}
			PushSweepHit(L, w, hit);
			return 1;
		}

		// ctx:physics_sweep_sphere(center, radius, dir, max_dist, mask)
		int CtxPhysicsSweepSphere(lua_State* L) {
			glm::vec3 center(0.0f), dir(0.0f);
			if (!ReadVec3(L, 2, center)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			const double radius = luaL_checknumber(L, 3);
			if (!(radius > 0.0) || radius > 10000.0) return luaL_argerror(L, 3, "radius must be in (0, 10000]");
			double maxDist = 0.0;
			uint32_t mask = 0;
			if (!ReadSweepArgs(L, 4, 5, 6, dir, maxDist, mask)) return luaL_argerror(L, 4, "expected a vector {x, y, z}");
			ShapeSweep::Probe probe;
			probe.kind = ShapeSweep::Kind::SPHERE;
			probe.center = center;
			probe.radius = static_cast<float>(radius);
			return RunSweep(L, probe, dir, maxDist, mask);
		}

		// ctx:physics_sweep_capsule(a, b, radius, dir, max_dist, mask): a and b are the segment ends (the caps extend by
		// radius beyond them). Equal ends make a sphere.
		int CtxPhysicsSweepCapsule(lua_State* L) {
			glm::vec3 a(0.0f), b(0.0f), dir(0.0f);
			if (!ReadVec3(L, 2, a)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			if (!ReadVec3(L, 3, b)) return luaL_argerror(L, 3, "expected a vector {x, y, z}");
			const double radius = luaL_checknumber(L, 4);
			if (!(radius > 0.0) || radius > 10000.0) return luaL_argerror(L, 4, "radius must be in (0, 10000]");
			double maxDist = 0.0;
			uint32_t mask = 0;
			if (!ReadSweepArgs(L, 5, 6, 7, dir, maxDist, mask)) return luaL_argerror(L, 5, "expected a vector {x, y, z}");
			ShapeSweep::Probe probe;
			probe.radius = static_cast<float>(radius);
			probe.center = 0.5f * (a + b);
			const glm::vec3 axis = b - a;
			const float length = glm::length(axis);
			if (length < 1e-6f) {
				probe.kind = ShapeSweep::Kind::SPHERE;
			}
			else {
				probe.kind = ShapeSweep::Kind::CAPSULE;
				probe.halfSegment = 0.5f * length;
				probe.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), axis / length);
			}
			return RunSweep(L, probe, dir, maxDist, mask);
		}

		// ctx:physics_sweep_box(center, half_extents, dir, max_dist, mask): an axis-aligned box.
		int CtxPhysicsSweepBox(lua_State* L) {
			glm::vec3 center(0.0f), half(0.0f), dir(0.0f);
			if (!ReadVec3(L, 2, center)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			if (!ReadVec3(L, 3, half) || !(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f)) {
				return luaL_argerror(L, 3, "half_extents must be a positive vector {x, y, z}");
			}
			double maxDist = 0.0;
			uint32_t mask = 0;
			if (!ReadSweepArgs(L, 4, 5, 6, dir, maxDist, mask)) return luaL_argerror(L, 4, "expected a vector {x, y, z}");
			ShapeSweep::Probe probe;
			probe.kind = ShapeSweep::Kind::BOX;
			probe.center = center;
			probe.halfExtents = half;
			return RunSweep(L, probe, dir, maxDist, mask);
		}

		// Array of the entity handles of the bodies in `ids` that have a live entity, sorted by entity.
		void PushEntityArray(lua_State* L, LuaWorld* w, const PhysicsEngine& physics, const std::vector<PhysicsEngine::BodyID>& ids) {
			std::vector<EntityID> entities;
			for (PhysicsEngine::BodyID id : ids) {
				const EntityID entity = physics.GetEntityForBody(id);
				if (entity != INVALID_ENTITY) entities.push_back(entity);
			}
			std::sort(entities.begin(), entities.end());
			entities.erase(std::unique(entities.begin(), entities.end()), entities.end());

			lua_newtable(L);
			lua_Integer index = 1;
			for (EntityID entity : entities) {
				EntityHandle handle;
				if (!HandleFor(w, entity, handle)) continue;
				PushHandle(L, handle);
				lua_rawseti(L, -2, index++);
			}
		}

		// ctx:overlap_sphere(center, radius, mask) -> array of entity handles of the bodies overlapping the sphere.
		int CtxOverlapSphere(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			glm::vec3 center(0.0f);
			if (!ReadVec3(L, 2, center)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			const double radius = luaL_checknumber(L, 3);
			if (!(radius > 0.0) || radius > kMaxRayDistance) return luaL_argerror(L, 3, "radius must be in (0, 100000]");
			const uint32_t mask = MaskArg(L, 4);

			PhysicsEngine* physics = nox::reflect::Services().physics;
			if (!physics) {
				lua_newtable(L);
				return 1;
			}
			PushEntityArray(L, w, *physics, physics->OverlapSphere(center, static_cast<float>(radius), mask));
			return 1;
		}

		// ctx:overlap_box(center, half_extents, mask) -> array of entity handles (world-aligned box).
		int CtxOverlapBox(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			glm::vec3 center(0.0f), half(0.0f);
			if (!ReadVec3(L, 2, center)) return luaL_argerror(L, 2, "expected a vector {x, y, z}");
			if (!ReadVec3(L, 3, half) || !(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f)) {
				return luaL_argerror(L, 3, "half_extents must be three positive numbers");
			}
			const uint32_t mask = MaskArg(L, 4);

			PhysicsEngine* physics = nox::reflect::Services().physics;
			if (!physics) {
				lua_newtable(L);
				return 1;
			}
			PushEntityArray(L, w, *physics, physics->OverlapBox(center, half, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), mask));
			return 1;
		}

		// Queues one world event whose payload is { firstKey = first|nil, secondKey = second|nil }. Returns false
		// when the event queue is full.
		bool QueueWithPayload(LuaWorld& w, const char* name, EntityHandle* first, EntityHandle* second,
			const char* firstKey, const char* secondKey) {
			lua_State* L = w.GetRuntime().State();
			lua_createtable(L, 0, 2);
			if (first) PushHandle(L, *first);
			else lua_pushnil(L);
			lua_setfield(L, -2, firstKey);
			if (second) PushHandle(L, *second);
			else lua_pushnil(L);
			lua_setfield(L, -2, secondKey);
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			if (!w.QueueNativeEvent(name, ref)) {
				w.GetRuntime().Unref(ref);
				return false;
			}
			return true;
		}

		void QueueTriggerEvents(LuaWorld& w, PhysicsEngine& physics) {
			for (const TriggerEvent& event : physics.Triggers().TakeEvents()) {
				EntityHandle trigger;
				if (!HandleFor(&w, event.trigger, trigger)) continue;
				EntityHandle other;
				const bool hasOther = HandleFor(&w, event.other, other);
				QueueWithPayload(w, event.enter ? "trigger_enter" : "trigger_exit", &trigger,
					hasOther ? &other : nullptr, "trigger", "other");
			}
		}

		void QueueContactEvents(LuaWorld& w, PhysicsEngine& physics) {
			for (const PhysicsContactEvent& event : physics.TakeContactEvents()) {
				const std::shared_ptr<SceneNode> nodeA = event.bodyA ? event.bodyA->getAttachedNode() : nullptr;
				const std::shared_ptr<SceneNode> nodeB = event.bodyB ? event.bodyB->getAttachedNode() : nullptr;
				EntityHandle a, b;
				const bool hasA = nodeA && HandleFor(&w, nodeA->GetEntityID(), a);
				const bool hasB = nodeB && HandleFor(&w, nodeB->GetEntityID(), b);
				if (!hasA && !hasB) continue;   // no scripted entity on either side
				QueueWithPayload(w, event.type == PhysicsContactEvent::Type::BEGIN ? "contact_begin" : "contact_end",
					hasA ? &a : nullptr, hasB ? &b : nullptr, "a", "b");
			}
		}

	}

	// nox.physics: world tuning for scripts. Values are the runtime ones (SimulationConfig keys, see docs/lua_api.md).
	// Script changes are runtime only and are never written into a saved scene.
	namespace {

		PhysicsEngine* PhysicsOrError(lua_State* L) {
			PhysicsEngine* physics = nox::reflect::Services().physics;
			if (!physics) luaL_error(L, "physics is not available");
			return physics;
		}

		// Lua value to JSON. False for values that have no physics setting form (functions, userdata, mixed tables).
		bool LuaToJson(lua_State* L, int idx, nlohmann::json& out, int depth) {
			idx = lua_absindex(L, idx);
			if (depth > 4) return false;
			switch (lua_type(L, idx)) {
			case LUA_TBOOLEAN:
				out = lua_toboolean(L, idx) != 0;
				return true;
			case LUA_TNUMBER:
				if (lua_isinteger(L, idx)) out = static_cast<int64_t>(lua_tointeger(L, idx));
				else out = lua_tonumber(L, idx);
				return true;
			case LUA_TSTRING:
				out = std::string(lua_tostring(L, idx));
				return true;
			case LUA_TTABLE: {
				// {x=, y=, z=} is a vector: a three-number array, like config.json gravity.
				lua_getfield(L, idx, "x");
				lua_getfield(L, idx, "y");
				lua_getfield(L, idx, "z");
				const bool vector = lua_type(L, -3) == LUA_TNUMBER && lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER;
				const double x = vector ? lua_tonumber(L, -3) : 0.0, y = vector ? lua_tonumber(L, -2) : 0.0, z = vector ? lua_tonumber(L, -1) : 0.0;
				lua_pop(L, 3);
				if (vector) {
					out = nlohmann::json::array({ x, y, z });
					return true;
				}
				if (lua_rawlen(L, idx) > 0) {
					out = nlohmann::json::array();
					const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
					for (lua_Integer i = 1; i <= n; ++i) {
						lua_rawgeti(L, idx, i);
						nlohmann::json item;
						const bool ok = LuaToJson(L, -1, item, depth + 1);
						lua_pop(L, 1);
						if (!ok) return false;
						out.push_back(item);
					}
					return true;
				}
				out = nlohmann::json::object();
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					if (lua_type(L, -2) != LUA_TSTRING) {
						lua_pop(L, 2);
						return false;
					}
					nlohmann::json item;
					const std::string key = lua_tostring(L, -2);
					const bool ok = LuaToJson(L, -1, item, depth + 1);
					lua_pop(L, 1);
					if (!ok) {
						lua_pop(L, 2);   // value and the key that lua_next needs
						return false;
					}
					out[key] = item;
				}
				return true;
			}
			default:
				return false;
			}
		}

		// JSON to Lua (settings snapshots).
		void PushJson(lua_State* L, const nlohmann::json& j) {
			switch (j.type()) {
			case nlohmann::json::value_t::boolean:
				lua_pushboolean(L, j.get<bool>() ? 1 : 0);
				break;
			case nlohmann::json::value_t::number_integer:
			case nlohmann::json::value_t::number_unsigned:
				lua_pushinteger(L, static_cast<lua_Integer>(j.get<int64_t>()));
				break;
			case nlohmann::json::value_t::number_float:
				lua_pushnumber(L, j.get<double>());
				break;
			case nlohmann::json::value_t::string:
				lua_pushstring(L, j.get_ref<const std::string&>().c_str());
				break;
			case nlohmann::json::value_t::array: {
				lua_createtable(L, static_cast<int>(j.size()), 0);
				lua_Integer i = 1;
				for (auto it = j.begin(); it != j.end(); ++it, ++i) {
					PushJson(L, *it);
					lua_rawseti(L, -2, i);
				}
				break;
			}
			case nlohmann::json::value_t::object:
				lua_createtable(L, 0, static_cast<int>(j.size()));
				for (auto it = j.begin(); it != j.end(); ++it) {
					PushJson(L, it.value());
					lua_setfield(L, -2, it.key().c_str());
				}
				break;
			default:
				lua_pushnil(L);
				break;
			}
		}

		// nox.physics.settings() -> table of every setting (runtime values).
		int PhysicsSettings(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			PushJson(L, physics->GetConfig().ToJson());
			return 1;
		}

		// nox.physics.get(key) -> the runtime value of one setting.
		int PhysicsGet(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			const char* key = luaL_checkstring(L, 1);
			bool found = false;
			{
				const nlohmann::json all = physics->GetConfig().ToJson();
				if (all.contains(key)) {
					PushJson(L, all[key]);
					found = true;
				}
			}
			if (!found) return luaL_error(L, "unknown physics setting '%s'", key);
			return 1;
		}

		// Applies one runtime setting from Lua value idx. False with a message in error. The message goes to a
		// plain buffer because luaL_error longjmps: no C++ object with a destructor may be alive when it runs.
		bool SetRuntimeSetting(lua_State* L, int idx, PhysicsEngine* physics, const char* key, char* error, size_t errorSize) {
			nlohmann::json value;
			if (!LuaToJson(L, idx, value, 0)) {
				std::snprintf(error, errorSize, "unsupported value");
				return false;
			}
			if (!physics->GetConfig().ToJson().contains(key)) {
				std::snprintf(error, errorSize, "unknown setting");
				return false;
			}
			SimulationConfig config = physics->GetConfig();
			nlohmann::json patch = nlohmann::json::object();
			patch[key] = value;
			try {
				config.applyJson(patch);
			}
			catch (const std::exception& e) {
				std::snprintf(error, errorSize, "%s", e.what());
				return false;
			}
			physics->SetRuntimeConfig(config);
			return true;
		}

		// nox.physics.set(key, value): changes one runtime setting; the editor's authored values are not touched.
		int PhysicsSet(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			const char* key = luaL_checkstring(L, 1);
			luaL_checkany(L, 2);
			char error[160] = "";
			if (!SetRuntimeSetting(L, 2, physics, key, error, sizeof(error))) return luaL_error(L, "physics setting '%s': %s", key, error);
			return 0;
		}

		// nox.physics.reset(): runtime values back to the editor's authored values (drops script changes).
		int PhysicsReset(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			physics->SetRuntimeConfig(physics->GetAuthoredConfig());
			return 0;
		}

		bool LayerArg(lua_State* L, int idx, int& layer) {
			const lua_Integer v = luaL_checkinteger(L, idx);
			if (v < 0 || v >= SimulationConfig::kLayerCount) return false;
			layer = static_cast<int>(v);
			return true;
		}

		// nox.physics.set_collision(a, b, enabled): the world layer matrix entry for layers a and b (symmetric).
		int PhysicsSetCollision(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			int a = 0, b = 0;
			if (!LayerArg(L, 1, a)) return luaL_argerror(L, 1, "layer must be 0..31");
			if (!LayerArg(L, 2, b)) return luaL_argerror(L, 2, "layer must be 0..31");
			luaL_checkany(L, 3);
			const bool enabled = lua_toboolean(L, 3) != 0;
			SimulationConfig config = physics->GetConfig();
			for (int pair = 0; pair < 2; ++pair) {
				const int row = pair == 0 ? a : b;
				const int col = pair == 0 ? b : a;
				if (enabled) config.layerMatrix[row] |= (1u << col);
				else config.layerMatrix[row] &= ~(1u << col);
			}
			physics->SetRuntimeConfig(config);
			return 0;
		}

		// nox.physics.collides(a, b) -> boolean: whether layers a and b may collide under the layer matrix.
		int PhysicsCollides(lua_State* L) {
			PhysicsEngine* physics = PhysicsOrError(L);
			int a = 0, b = 0;
			if (!LayerArg(L, 1, a)) return luaL_argerror(L, 1, "layer must be 0..31");
			if (!LayerArg(L, 2, b)) return luaL_argerror(L, 2, "layer must be 0..31");
			const SimulationConfig& config = physics->GetConfig();
			const bool allowed = (config.layerMatrix[a] & (1u << b)) != 0 && (config.layerMatrix[b] & (1u << a)) != 0;
			lua_pushboolean(L, allowed ? 1 : 0);
			return 1;
		}

	}

	void InstallPhysicsSettingsApi(lua_State* L) {
		lua_getglobal(L, "nox");
		lua_newtable(L);
		lua_pushcfunction(L, PhysicsSettings);
		lua_setfield(L, -2, "settings");
		lua_pushcfunction(L, PhysicsGet);
		lua_setfield(L, -2, "get");
		lua_pushcfunction(L, PhysicsSet);
		lua_setfield(L, -2, "set");
		lua_pushcfunction(L, PhysicsReset);
		lua_setfield(L, -2, "reset");
		lua_pushcfunction(L, PhysicsSetCollision);
		lua_setfield(L, -2, "set_collision");
		lua_pushcfunction(L, PhysicsCollides);
		lua_setfield(L, -2, "collides");
		lua_setfield(L, -2, "physics");
		lua_pop(L, 1);
	}

	void InstallPhysicsApi(lua_State* L, int ctxIdx, LuaWorld* w) {
		ctxIdx = lua_absindex(L, ctxIdx);
		SetFn(L, w, ctxIdx, "physics_raycast", CtxPhysicsRaycast);
		SetFn(L, w, ctxIdx, "overlap_sphere", CtxOverlapSphere);
		SetFn(L, w, ctxIdx, "overlap_box", CtxOverlapBox);
		SetFn(L, w, ctxIdx, "physics_sweep_sphere", CtxPhysicsSweepSphere);
		SetFn(L, w, ctxIdx, "physics_sweep_capsule", CtxPhysicsSweepCapsule);
		SetFn(L, w, ctxIdx, "physics_sweep_box", CtxPhysicsSweepBox);
	}

	void PumpPhysicsEvents(LuaWorld& world) {
		PhysicsEngine* physics = nox::reflect::Services().physics;
		if (!physics) return;
		QueueTriggerEvents(world, *physics);
		QueueContactEvents(world, *physics);
	}
}
