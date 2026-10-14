#pragma once
// Lua access to the reflection registry (src/reflect): component proxies (ctx:component), component add,
// remove and listing, nox.components() introspection, and the scene queries ctx:parent, ctx:children,
// ctx:raycast and ctx:load_scene. Generic over the registry: a descriptor registered by any subsystem is
// scriptable without changes here. Contract: docs/lua_api.md, "Components".

#include "../ComponentManager.h"

#include <string>
#include <vector>

struct lua_State;

namespace NoxLua {

	class LuaWorld;

	// Adds the component functions to the ctx table at ctxIdx and nox.components to the nox table.
	// Also registers the engine's built-in descriptors (idempotent).
	void InstallComponentApi(lua_State* L, int ctxIdx, LuaWorld* world);

	// Native presence-only components that have no descriptor yet (Renderable, Collider, ...), plus Transform
	// and Light, which are also in the native pools. `known` is false for names outside this list.
	bool NativeComponentPresent(ComponentManager* cm, EntityID id, const std::string& name, bool& known);
	const std::vector<std::string>& NativeComponentNames();
}
