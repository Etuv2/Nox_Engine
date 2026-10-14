#include "LuaPhysics.h"

#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"
#include "../reflect/ReflectServices.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
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

	void InstallPhysicsApi(lua_State* L, int ctxIdx, LuaWorld* w) {
		ctxIdx = lua_absindex(L, ctxIdx);
		SetFn(L, w, ctxIdx, "physics_raycast", CtxPhysicsRaycast);
		SetFn(L, w, ctxIdx, "overlap_sphere", CtxOverlapSphere);
		SetFn(L, w, ctxIdx, "overlap_box", CtxOverlapBox);
	}

	void PumpPhysicsEvents(LuaWorld& world) {
		PhysicsEngine* physics = nox::reflect::Services().physics;
		if (!physics) return;
		QueueTriggerEvents(world, *physics);
		QueueContactEvents(world, *physics);
	}
}
