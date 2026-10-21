#pragma once
// Lua scene instancing: ctx:instantiate(path, opts) creates a prefab instance (a full authored-like node with its
// registry components, not an ECS-only entity) and returns its handle. ctx:destroy(h) on a scene node removes it
// through the host (see LuaWorld::ApplyCommands). Contract: docs/lua_api.md, "Scene instances".

#include <optional>
#include "json.hpp"

struct lua_State;

namespace NoxLua {

	class LuaWorld;

	// Adds ctx:instantiate to the ctx table at ctxIdx. Registered next to InstallComponentApi.
	void InstallSceneApi(lua_State* L, int ctxIdx, LuaWorld* world);

	// Scene payload handed from ctx:load_scene(path, payload) to the scene that loads next. The payload is plain data
	// (JSON form, copied). LuaGameModule takes it when it configures the next scene and gives it to that world
	// (LuaWorld::SetScenePayload): begin_play(ctx, payload) and ctx:scene_payload() read it. Stored in the scripting
	// layer only; Core sees just the scene request.
	void SetPendingScenePayload(std::optional<nlohmann::json> payload);
	std::optional<nlohmann::json> TakePendingScenePayload();
}
