// Lua services: camera (look-at, fov, clip, cut), effects budget (VehicleFX emitters), declared nav files,
// save-data names and the bounded Lua <-> JSON conversion. Headless: no GPU, no window, no AssetIO mounts.
#include "TestFramework.h"

#include "Camera.h"
#include "SceneGraph.h"
#include "scripting/LuaRuntime.h"
#include "scripting/LuaServices.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <cmath>
#include <map>
#include <string>

namespace {
	class MemorySource : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const auto it = files.find(logicalPath);
			if (it == files.end()) {
				error = "no such file";
				return false;
			}
			text = it->second;
			return true;
		}
	};

	const char* kEntry = "projects/t/scripts/main.lua";

	struct Harness {
		MemorySource source;
		SceneGraph scene;
		NoxLua::LuaWorld world;
		Camera camera;

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source)
			, camera(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f, 60.0f, 16.0f / 9.0f, 0.1f, 1000.0f) {
			world.SetCamera(&camera);
		}

		bool Load(const std::string& body) {
			source.files[kEntry] = body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}

		void Play() {
			world.BeginPlay();
			world.Update(1.0f / 60.0f, true);
		}

		bool GlobalBool(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const bool value = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		}
	};

	// Runs `body` in a standalone VM (no world) and returns the first result converted to JSON.
	bool EvalToJson(const std::string& body, nlohmann::json& out, std::string& error) {
		NoxLua::Runtime runtime(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, nullptr);
		int ref = 0;
		if (!runtime.Compile("return " + body, "test.lua", ref, error)) return false;
		NoxLua::RunError runError;
		if (!runtime.CallRef(ref, [](lua_State*) { return 0; }, 1, &runError)) {
			error = runError.message;
			return false;
		}
		const bool ok = NoxLua::LuaValueToJson(runtime.State(), -1, out, error);
		lua_pop(runtime.State(), 1);
		return ok;
	}
}

NOX_TEST(LuaServices, ValidatesSaveNames) {
	std::string error;
	NOX_CHECK(NoxLua::ValidateUserFileName("best_times.json", error));
	NOX_CHECK(NoxLua::ValidateUserFileName("saves/slot-1.json", error));
	NOX_CHECK(NoxLua::ValidateUserFileName("a/b/c/d.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("save.txt", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("../escape.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("a/../b.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("/abs.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("C:/x.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("a//b.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("a/b/c/d/e.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName("bad name.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName(".hidden.json", error));
	NOX_CHECK(!NoxLua::ValidateUserFileName(std::string(120, 'a') + ".json", error));
}

NOX_TEST(LuaServices, RoundTripsValuesWithTypes) {
	nlohmann::json value;
	std::string error;
	NOX_REQUIRE(EvalToJson("{ n = 3, f = 2.5, s = 'x', ok = true, list = { 1, 2.5, 'three' }, inner = { deep = {} } }", value, error));
	NOX_CHECK(value["n"].is_number_integer());
	NOX_CHECK(value["n"].get<int64_t>() == 3);
	NOX_CHECK(value["f"].is_number_float());
	NOX_CHECK(value["s"] == "x");
	NOX_CHECK(value["ok"] == true);
	NOX_CHECK(value["list"].size() == 3u);
	NOX_CHECK(value["list"][2] == "three");
	NOX_CHECK(value["inner"]["deep"].is_object());

	// JSON -> Lua keeps integers as integers.
	NoxLua::Runtime runtime(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, nullptr);
	lua_State* L = runtime.State();
	NoxLua::PushJsonValue(L, value);
	lua_getfield(L, -1, "n");
	NOX_CHECK(lua_isinteger(L, -1) != 0);
	lua_pop(L, 1);
	lua_getfield(L, -1, "f");
	NOX_CHECK(lua_isinteger(L, -1) == 0);
	lua_pop(L, 1);
	lua_pop(L, 1);
}

NOX_TEST(LuaServices, RejectsValuesThatCannotBeSaved) {
	nlohmann::json value;
	std::string error;
	NOX_CHECK(!EvalToJson("{ f = function() end }", value, error));
	NOX_CHECK(error.find("function") != std::string::npos);
	NOX_CHECK(!EvalToJson("{ 1, 2, x = 3 }", value, error));           // mixed table
	NOX_CHECK(!EvalToJson("{ 1, nil, 3 }", value, error));             // hole
	NOX_CHECK(!EvalToJson("{ [0] = 1 }", value, error));               // non-positive index
	NOX_CHECK(!EvalToJson("0/0", value, error));                       // NaN
	NOX_CHECK(!EvalToJson("1/0", value, error));                       // infinity
	NOX_CHECK(EvalToJson("{ x = { y = { z = {} } } }", value, error));   // nested objects are fine

	std::string deep = "1";
	for (int i = 0; i < 20; ++i) deep = "{ " + deep + " }";
	NOX_CHECK(!EvalToJson(deep, value, error));
	NOX_CHECK(error.find("nested") != std::string::npos);

	std::string big = "{";
	for (int i = 0; i < 21000; ++i) big += "0,";
	big += "}";
	NOX_CHECK(!EvalToJson(big, value, error));
	NOX_CHECK(error.find("values") != std::string::npos);
}

NOX_TEST(LuaServices, CameraLookAtFovCutAndTeleportFlag) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'cam', update = function(ctx, dt)\n"
		"  ctx:camera_look_at(0, 2, 0, 10, 2, 0)\n"
		"  ctx:camera_set_fov(75)\n"
		"  ctx:camera_set_clip(0.5, 500)\n"
		"  ctx:camera_cut()\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	const glm::vec3 front = h.camera.GetCameraFrontVector();
	NOX_CHECK(std::fabs(front.x - 1.0f) < 1e-4f);
	NOX_CHECK(std::fabs(front.y) < 1e-4f);
	NOX_CHECK(std::fabs(h.camera.GetCameraFov() - 75.0f) < 1e-4f);
	NOX_CHECK(std::fabs(h.camera.GetCameraNearPlane() - 0.5f) < 1e-6f);
	NOX_CHECK(std::fabs(h.camera.GetCameraFarPlane() - 500.0f) < 1e-3f);
	NOX_CHECK(h.world.ConsumeTeleport());   // the cut is reported once
	NOX_CHECK(!h.world.ConsumeTeleport());
}

NOX_TEST(LuaServices, CameraLookAtPitchUp) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'cam', update = function(ctx, dt)\n"
		"  ctx:camera_look_at(0, 0, 0, 0, 10, 0.0001)\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	const glm::vec3 front = h.camera.GetCameraFrontVector();
	NOX_CHECK(front.y > 0.99f);   // pitch is clamped to 89 degrees by the camera
}

NOX_TEST(LuaServices, CameraRejectsBadArguments) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'cam', update = function(ctx, dt)\n"
		"  ok_same = pcall(ctx.camera_look_at, ctx, 1, 2, 3, 1, 2, 3)\n"
		"  ok_fov = pcall(ctx.camera_set_fov, ctx, 200)\n"
		"  ok_clip = pcall(ctx.camera_set_clip, ctx, 2, 1)\n"
		"  ok_nan = pcall(ctx.camera_look_at, ctx, 0/0, 0, 0, 1, 0, 0)\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	NOX_CHECK(!h.GlobalBool("ok_same"));
	NOX_CHECK(!h.GlobalBool("ok_fov"));
	NOX_CHECK(!h.GlobalBool("ok_clip"));
	NOX_CHECK(!h.GlobalBool("ok_nan"));
	NOX_CHECK(std::fabs(h.camera.GetCameraFov() - 60.0f) < 1e-4f);   // unchanged by the rejected calls
}

NOX_TEST(LuaServices, EffectEmitterBudgetPerFrame) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'fx', update = function(ctx, dt)\n"
		"  accepted, dropped = 0, 0\n"
		"  for i = 1, 600 do\n"
		"    if ctx:fx_smoke(0, 0, 0, 0, 0, 0, 0.5, 0.016) then accepted = accepted + 1 else dropped = dropped + 1 end\n"
		"  end\n"
		"  ctx:fx_spray(0, 0, 0, 1, 0, 0, 1.0, 0.016, 0)\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "accepted");
	NOX_CHECK(lua_tointeger(L, -1) == 512);
	lua_pop(L, 1);
	lua_getglobal(L, "dropped");
	NOX_CHECK(lua_tointeger(L, -1) == 88);
	lua_pop(L, 1);
	NOX_CHECK(h.world.Services().fxUsed);
	NOX_CHECK(h.world.Diagnostics().empty());
}

NOX_TEST(LuaServices, UndeclaredNavFilesAreRefused) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'nav', update = function(ctx, dt)\n"
		"  ok_roads, err_roads = pcall(nox.roads, 'data/secret.json')\n"
		"  ok_col, err_col = pcall(nox.collision, 'models/secret.glb')\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	NOX_CHECK(!h.GlobalBool("ok_roads"));
	NOX_CHECK(!h.GlobalBool("ok_col"));
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "err_roads");
	NOX_CHECK(std::string(lua_tostring(L, -1)).find("not declared in project.json (roads)") != std::string::npos);
	lua_pop(L, 1);
}

NOX_TEST(LuaServices, SaveNamesAreCheckedBeforeAnyIo) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'save', update = function(ctx, dt)\n"
		"  saved, why = ctx:save_user_json('../escape.json', { a = 1 })\n"
		"  loaded, why_load = ctx:load_user_json('/etc/passwd')\n"
		"  bad_value, why_value = ctx:save_user_json('ok.json', { f = function() end })\n"
		"  ran = true\n"
		"end }\n"));
	h.Play();
	NOX_REQUIRE(h.GlobalBool("ran"));
	NOX_CHECK(!h.GlobalBool("saved"));
	NOX_CHECK(!h.GlobalBool("bad_value"));
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "loaded");
	NOX_CHECK(lua_isnil(L, -1));
	lua_pop(L, 1);
	lua_getglobal(L, "why_value");
	NOX_CHECK(std::string(lua_tostring(L, -1)).find("function") != std::string::npos);
	lua_pop(L, 1);
}
