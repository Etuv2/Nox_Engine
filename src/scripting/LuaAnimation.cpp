#include "LuaAnimation.h"

#include "../AnimationSystem.h"
#include "../ComponentManager.h"
#include "../SceneGraph.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <cstring>
#include <vector>

namespace NoxLua {

	namespace {

		constexpr const char* kHandleMeta = "nox.entity";   // the handle metatable LuaWorld installs

		void PushHandle(lua_State* L, const EntityHandle& h) {
			void* mem = lua_newuserdatauv(L, sizeof(EntityHandle), 0);
			std::memcpy(mem, &h, sizeof(EntityHandle));
			luaL_setmetatable(L, kHandleMeta);
		}
	}

	void PumpAnimationEvents(LuaWorld& world) {
		SceneGraph* scene = world.Scene();
		ComponentManager* manager = world.Manager();
		if (!scene || !manager) return;
		AnimationSystem* system = scene->GetAnimationSystem();
		if (!system) return;

		const std::vector<AnimationEvent> events = system->TakeAnimationEvents();
		if (events.empty()) return;

		lua_State* L = world.GetRuntime().State();
		for (const AnimationEvent& e : events) {
			if (e.entity == INVALID_ENTITY) continue;
			const EntityHandle handle = manager->MakeHandle(e.entity);
			if (!manager->IsHandleValid(handle)) continue;

			lua_createtable(L, 0, 4);
			PushHandle(L, handle);
			lua_setfield(L, -2, "entity");
			lua_pushstring(L, e.name.c_str());
			lua_setfield(L, -2, "name");
			lua_pushstring(L, e.clip.c_str());
			lua_setfield(L, -2, "clip");
			lua_pushnumber(L, static_cast<lua_Number>(e.phase));
			lua_setfield(L, -2, "phase");
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			if (!world.QueueNativeEvent("anim.event", ref)) {
				world.GetRuntime().Unref(ref);
			}
		}
	}
}
