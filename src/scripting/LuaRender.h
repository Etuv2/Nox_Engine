#pragma once
// Lua render helpers that the component proxies do not cover: ctx:screen_to_ray for the scene's current
// camera (the node with Camera.current, src/render/ActiveCamera.h). Camera parameters, world environment
// and the rest of the render settings are plain components (ctx:component(h, "WorldEnvironment") etc.).
// Contract: docs/lua_api.md, "Rendering".

struct lua_State;

namespace NoxLua {

	class LuaWorld;

	// Adds the render functions to the ctx table at ctxIdx. Called next to InstallComponentApi.
	void InstallRenderApi(lua_State* L, int ctxIdx, LuaWorld* world);
}
