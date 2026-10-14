#pragma once
// Lua scene instancing: ctx:instantiate(path, opts) creates a prefab instance (a full authored-like node with its
// registry components, not an ECS-only entity) and returns its handle. ctx:destroy(h) on a scene node removes it
// through the host (see LuaWorld::ApplyCommands). Contract: docs/lua_api.md, "Scene instances".

struct lua_State;

namespace NoxLua {

	class LuaWorld;

	// Adds ctx:instantiate to the ctx table at ctxIdx. Registered next to InstallComponentApi.
	void InstallSceneApi(lua_State* L, int ctxIdx, LuaWorld* world);
}
