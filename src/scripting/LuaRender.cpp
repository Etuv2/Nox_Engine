#include "LuaRender.h"

#include "../Camera.h"
#include "../ComponentTypes.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../render/ActiveCamera.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace NoxLua {

	namespace {

		void SetFn(lua_State* L, LuaWorld* w, int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, w);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, tableIdx, name);
		}

		LuaWorld* WorldOf(lua_State* L) {
			return static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		// ctx:screen_to_ray(nx, ny [, aspect]) -> ox, oy, oz, dx, dy, dz for the active camera node, or nil when
		// no node has Camera.current set. nx, ny are 0..1 over the view with y down; aspect defaults to 16:9.
		// Reads only; the ray starts at the camera and its direction is normalised.
		int CtxScreenToRay(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const double nx = luaL_checknumber(L, 2);
			const double ny = luaL_checknumber(L, 3);
			const float aspect = static_cast<float>(luaL_optnumber(L, 4, 16.0 / 9.0));
			if (!(aspect > 0.0f)) return luaL_argerror(L, 4, "aspect must be positive");
			SceneGraph* graph = w->Scene();
			if (!graph || !graph->GetRoot()) {
				lua_pushnil(L);
				return 1;
			}
			SceneNode* node = nox::render::FindActiveCameraNode(*graph->GetRoot());
			const CameraComponent* lens = node ? node->GetCameraComponent() : nullptr;
			if (!lens) {
				lua_pushnil(L);
				return 1;
			}
			const glm::mat4 world = node->GetWorldPosition4x4();
			const glm::mat4 view = glm::inverse(world);
			const glm::mat4 proj = glm::perspective(glm::radians(lens->fov), aspect, lens->nearPlane, lens->farPlane);
			const glm::mat4 inverseViewProj = glm::inverse(proj * view);
			const glm::vec4 ndc(static_cast<float>(nx * 2.0 - 1.0), static_cast<float>(1.0 - ny * 2.0), 1.0f, 1.0f);
			glm::vec4 farPoint = inverseViewProj * ndc;
			farPoint /= farPoint.w;
			const glm::vec3 origin = glm::vec3(world[3]);
			const glm::vec3 dir = glm::normalize(glm::vec3(farPoint) - origin);
			lua_pushnumber(L, origin.x);
			lua_pushnumber(L, origin.y);
			lua_pushnumber(L, origin.z);
			lua_pushnumber(L, dir.x);
			lua_pushnumber(L, dir.y);
			lua_pushnumber(L, dir.z);
			return 6;
		}
	}

	void InstallRenderApi(lua_State* L, int ctxIdx, LuaWorld* w) {
		ctxIdx = lua_absindex(L, ctxIdx);
		SetFn(L, w, ctxIdx, "screen_to_ray", CtxScreenToRay);
	}
}
