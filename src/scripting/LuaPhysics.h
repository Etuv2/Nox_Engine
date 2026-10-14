#pragma once
// Lua access to the physics world: ctx:physics_raycast and the overlap queries, and delivery of the physics
// events (trigger_enter, trigger_exit, contact_begin, contact_end) as nox.on handlers. Contract:
// docs/lua_api.md "Physics", docs/physics.md. Physics comes from the engine service (reflect::Services().physics);
// without it the queries return nil or an empty list and no events are delivered.

struct lua_State;

namespace NoxLua {

	class LuaWorld;

	// Adds the physics query functions to the ctx table at ctxIdx.
	void InstallPhysicsApi(lua_State* L, int ctxIdx, LuaWorld* world);

	// Adds nox.physics (settings, get, set, reset, set_collision, collides): runtime world tuning. Needs the nox global.
	void InstallPhysicsSettingsApi(lua_State* L);

	// Moves the physics events collected since the last call into the world's event queue (delivered by the
	// same Update). Called by LuaWorld::Update before its events are dispatched.
	void PumpPhysicsEvents(LuaWorld& world);
}
