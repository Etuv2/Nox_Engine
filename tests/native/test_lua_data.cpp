// Project data bindings (docs/lua_api.md "Project data"): nox.json read policy and limits, the project
// descriptor and scene copies, ctx:env, authoring saves (editor contexts, loose mode, declared roots, reload
// staging), the configure/save hooks, inline and declared road queries, camera projection and overlay rect,
// ui.texture/icon policy, and ctx:request_exit. Headless: AssetIO over a temporary loose tree, no GPU.
#include "TestFramework.h"

#include "Camera.h"
#include "SceneGraph.h"
#include "assets/AssetIO.h"
#include "scripting/LuaProject.h"
#include "scripting/LuaUi.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace {
	namespace fs = std::filesystem;

	// Temporary loose tree mounted as the asset root for the test's lifetime.
	struct AssetFixture {
		fs::path root;
		bool ok = false;

		AssetFixture() {
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() / ("nox_lua_data_" + std::to_string(stamp));
			fs::create_directories(root);
			AssetIO::Config config;
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = root;
			config.cacheDir = root / "cache";
			config.userDir = root / "user";
			config.projectName = "lua-data-test";
			std::string error;
			ok = AssetIO::Initialize(config, &error);
		}

		~AssetFixture() {
			AssetIO::Shutdown();
			std::error_code ec;
			fs::remove_all(root, ec);
		}

		void Put(const std::string& relative, const std::string& text) const {
			const fs::path path = root / relative;
			fs::create_directories(path.parent_path());
			std::ofstream out(path, std::ios::binary);
			out << text;
		}

		std::string Get(const std::string& relative) const {
			std::ifstream in(root / relative, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		bool Exists(const std::string& relative) const {
			std::error_code ec;
			return fs::exists(root / relative, ec);
		}
	};

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
		NoxLua::ProjectContent project;

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			project.projectDir = "projects/t";
			project.descriptorPath = "projects/t/project.json";
			project.authoringRoots = { "config/hud.json", "data" };
			project.descriptor = nlohmann::json{
				{ "format", 1 }, { "name", "t" }, { "runtime", "lua" }, { "api_version", 1 },
				{ "entry", "scripts/main.lua" }, { "authoring_roots", { "config/hud.json", "data" } },
			};
		}

		bool Load(const std::string& body, const nlohmann::json& game = nullptr,
			const nlohmann::json& root = nlohmann::json::object()) {
			source.files[kEntry] = body;
			world.SetSceneDocuments(game, root);
			return world.Load(project, scene, nlohmann::json::object());
		}

		void Draw(float x = 0.0f, float y = 0.0f, float w = 800.0f, float h = 600.0f) {
			NoxLua::UiRecorder rec;
			world.SetOverlayRect(glm::vec2(x, y), glm::vec2(w, h));
			world.DrawFrame(1.0f / 60.0f, glm::vec2(w, h), glm::vec2(0.0f), rec);
		}

		bool DrawPanel(const std::string& id) {
			NoxLua::EditorHandles handles;
			return world.DrawEditorPanel(id, nullptr, &handles);
		}

		lua_State* L() { return world.GetRuntime().State(); }

		bool IsNil(const char* name) {
			lua_getglobal(L(), name);
			const bool nil = lua_isnil(L(), -1) != 0;
			lua_pop(L(), 1);
			return nil;
		}
		double Num(const char* name) {
			lua_getglobal(L(), name);
			const double v = lua_tonumber(L(), -1);
			lua_pop(L(), 1);
			return v;
		}
		bool Bool(const char* name) {
			lua_getglobal(L(), name);
			const bool v = lua_toboolean(L(), -1) != 0;
			lua_pop(L(), 1);
			return v;
		}
		std::string Str(const char* name) {
			lua_getglobal(L(), name);
			const char* s = lua_tostring(L(), -1);
			std::string out = s ? s : "";
			lua_pop(L(), 1);
			return out;
		}
	};

	bool Contains(const std::string& text, const char* needle) {
		return text.find(needle) != std::string::npos;
	}

	// A road network in the scene's inline form (the same schema as game.road_network).
	nlohmann::json InlineNetwork() {
		return nlohmann::json{
			{ "road_half_width", 5.0 },
			{ "sidewalk_width", 4.0 },
			{ "nodes", nlohmann::json::array({
				{ { "id", "a" }, { "pos", nlohmann::json::array({ 0.0, 0.0 }) } },
				{ { "id", "b" }, { "pos", nlohmann::json::array({ 100.0, 0.0 }) } },
				{ { "id", "c" }, { "pos", nlohmann::json::array({ 100.0, 100.0 }) } },
			}) },
			{ "edges", nlohmann::json::array({
				{ { "a", "a" }, { "b", "b" }, { "name", "Main" }, { "lanes", 2 } },
				nlohmann::json::array({ "b", "c" }),
			}) },
		};
	}
}

NOX_TEST(LuaData, JsonReadPolicyAndLimits) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	fx.Put("config/a.json", "{\"x\": 1}");
	fx.Put("config/bad.json", "{oops");
	fx.Put("config/big.json", std::string(1100u * 1024u, ' ') + "{}");
	fx.Put("scenes/s.json", "{\"n\": 7}");
	fx.Put("projects/t/data/roads.json", "{\"k\": [1, 2]}");
	fx.Put("projects/other/secret.json", "{\"s\": 1}");
	fx.Put("escape.json", "{\"e\": 1}");

	Harness h;
	NOX_REQUIRE(h.Load(
		"local a = nox.json('config/a.json')\n"
		"ax = a and a.x\n"
		"local b = nox.json('projects/t/data/roads.json')\n"
		"bk2 = b and b.k[2]\n"
		"local c, rc = nox.json('projects/other/secret.json')\n"
		"cnil = c == nil\n"
		"crs = rc\n"
		"local d = nox.json('../escape.json')\n"
		"dnil = d == nil\n"
		"local e = nox.json('/etc/passwd')\n"
		"enil = e == nil\n"
		"local _, rf = nox.json('config/missing.json')\n"
		"frs = rf\n"
		"local _, rg = nox.json('config/bad.json')\n"
		"grs = rg\n"
		"local _, rh = nox.json('config/big.json')\n"
		"hrs = rh\n"
		"local s = nox.json('scenes/s.json')\n"
		"sn = s and s.n\n"));
	NOX_CHECK(h.Num("ax") == 1);
	NOX_CHECK(h.Num("bk2") == 2);
	NOX_CHECK(h.Bool("cnil"));
	NOX_CHECK(Contains(h.Str("crs"), "not readable"));
	NOX_CHECK(h.Bool("dnil"));
	NOX_CHECK(h.Bool("enil"));
	NOX_CHECK(Contains(h.Str("frs"), "no such file"));
	NOX_CHECK(Contains(h.Str("grs"), "not valid JSON"));
	NOX_CHECK(Contains(h.Str("hrs"), "larger than 1 MiB"));
	NOX_CHECK(h.Num("sn") == 7);
}

NOX_TEST(LuaData, ProjectDescriptorAndSceneCopies) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	const nlohmann::json game = { { "hud", { { "size", 3 } } }, { "road_network", InlineNetwork() } };
	NOX_REQUIRE(h.Load(
		"local p = nox.project()\n"
		"pname = p.name\n"
		"pauth = p.authoring_roots[1]\n"
		"local g = nox.game()\n"
		"gsize = g.hud.size\n"
		"g.hud.size = 99\n"
		"gsize2 = nox.game().hud.size\n"
		"sname = nox.scene().name\n"
		"local rn = nox.roads.from_table(nox.game().road_network)\n"
		"rnedges = rn:edge_count()\n",
		game, nlohmann::json{ { "name", "scene" } }));
	NOX_CHECK(h.Str("pname") == "t");
	NOX_CHECK(h.Str("pauth") == "config/hud.json");
	NOX_CHECK(h.Num("gsize") == 3);
	NOX_CHECK(h.Num("gsize2") == 3);   // the copy was changed, the scene was not
	NOX_CHECK(h.Str("sname") == "scene");
	NOX_CHECK(h.Num("rnedges") == 2);

	Harness none;
	NOX_REQUIRE(none.Load("gnil = nox.game() == nil\nsnil = nox.scene() ~= nil\n"));
	NOX_CHECK(none.Bool("gnil"));
	NOX_CHECK(none.Bool("snil"));
}

NOX_TEST(LuaData, EnvIsRestrictedToNoxNames) {
#if defined(_WIN32)
	_putenv_s("NOX_TEST_ENV_BINDING", "on");
#endif
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  envv = ctx:env('NOX_TEST_ENV_BINDING')\n"
		"  envnil = ctx:env('NOX_TEST_UNSET_VARIABLE_XYZ') == nil\n"
		"  envpath, envpr = ctx:env('PATH')\n"
		"  ok1, err1 = pcall(function() return ctx:env('NOX_lower') end)\n"
		"  ok2, err2 = pcall(function() return ctx:env('NOX_" + std::string(70, 'A') + "') end)\n"
		"end)\n"));
	h.Draw();
#if defined(_WIN32)
	_putenv_s("NOX_TEST_ENV_BINDING", "");
#endif
	NOX_CHECK(h.Str("envv") == "on");
	NOX_CHECK(h.Bool("envnil"));
	NOX_CHECK(h.IsNil("envpath"));
	NOX_CHECK(Contains(h.Str("envpr"), "NOX_"));
	NOX_CHECK(!h.Bool("ok1"));
	NOX_CHECK(Contains(h.Str("err1"), "A-Z"));
	NOX_CHECK(!h.Bool("ok2"));
}

NOX_TEST(LuaData, AuthoringSavesOnlyFromEditorContexts) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	NOX_REQUIRE(h.Load(
		"local ok0, r0 = nox.save_authoring_json('config/hud.json', { a = 1 })\n"
		"loadok = ok0\n"
		"loadr = r0\n"
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  drawok, drawr = nox.save_authoring_json('config/hud.json', { z = 1 })\n"
		"end)\n"
		"nox.editor_panel{ id = 'hud', title = 'HUD', draw = function(ctx, ed)\n"
		"  p1, r1 = nox.save_authoring_json('config/hud.json', { a = 1 })\n"
		"  p2, r2 = nox.save_authoring_json('data/x.json', { b = { 1, 2 } })\n"
		"  p3, r3 = nox.save_authoring_json('config/other.json', {})\n"
		"  p4, r4 = nox.save_authoring_json('data/notes.txt', {})\n"
		"  p5, r5 = nox.save_authoring_json('projects/t/x.json', {})\n"
		"  p6, r6 = nox.save_authoring_json('config/hud.json', function() end)\n"
		"end }\n"));
	NOX_CHECK(!h.Bool("loadok"));
	NOX_CHECK(Contains(h.Str("loadr"), "only allowed from editor"));
	h.Draw();
	NOX_CHECK(!h.Bool("drawok"));
	NOX_CHECK(!fx.Exists("config/hud.json"));

	NOX_CHECK(h.DrawPanel("hud"));
	NOX_CHECK(h.Bool("p1"));
	NOX_CHECK(h.IsNil("r1"));
	NOX_CHECK(fx.Get("config/hud.json") == "{\n  \"a\": 1\n}\n");
	NOX_CHECK(h.Bool("p2"));
	NOX_CHECK(fx.Get("data/x.json") == "{\n  \"b\": [\n    1,\n    2\n  ]\n}\n");
	NOX_CHECK(!h.Bool("p3"));
	NOX_CHECK(Contains(h.Str("r3"), "authoring_roots"));
	NOX_CHECK(!h.Bool("p4"));
	NOX_CHECK(Contains(h.Str("r4"), ".json"));
	NOX_CHECK(!h.Bool("p5"));
	NOX_CHECK(!h.Bool("p6"));
	NOX_CHECK(Contains(h.Str("r6"), "cannot"));
	NOX_CHECK(!fx.Exists("config/hud.json.tmp"));
	NOX_CHECK(!fx.Exists("config/other.json"));
}

NOX_TEST(LuaData, AuthoringRefusedWhileStaging) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'hud', draw = function(ctx, ed)\n"
		"  p1, r1 = nox.save_authoring_json('config/hud.json', { a = 1 })\n"
		"end }\n"));
	h.world.SetStaging(true);
	NOX_CHECK(h.DrawPanel("hud"));
	NOX_CHECK(!h.Bool("p1"));
	NOX_CHECK(Contains(h.Str("r1"), "reload is staging"));
	NOX_CHECK(!fx.Exists("config/hud.json"));
	h.world.SetStaging(false);
	NOX_CHECK(h.DrawPanel("hud"));
	NOX_CHECK(h.Bool("p1"));
	NOX_CHECK(fx.Exists("config/hud.json"));
}

NOX_TEST(LuaData, ConfigureAndSaveGameHooks) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	const nlohmann::json game = { { "hud", { { "size", 3 } } } };
	NOX_REQUIRE(h.Load(
		"nox.on_configure(function(game, scene)\n"
		"  cfgsize = game and game.hud and game.hud.size or -1\n"
		"  cfgname = scene.name\n"
		"end)\n"
		"nox.on_save_game(function() return { saved = true, count = 3 } end)\n",
		game, nlohmann::json{ { "name", "scene" } }));
	std::string error;
	NOX_CHECK(h.world.RunConfigureHook(error));
	NOX_CHECK(h.Num("cfgsize") == 3);
	NOX_CHECK(h.Str("cfgname") == "scene");
	NOX_CHECK(h.world.HasSaveGameHook());
	nlohmann::json saved;
	NOX_CHECK(h.world.SaveGameJson(saved, error));
	NOX_CHECK(saved.value("saved", false));
	NOX_CHECK(saved.value("count", 0) == 3);

	Harness failing;
	NOX_REQUIRE(failing.Load("nox.on_save_game(function() error('boom') end)\n"));
	nlohmann::json unused;
	NOX_CHECK(!failing.world.SaveGameJson(unused, error));
	NOX_CHECK(Contains(error, "failed"));

	Harness none;
	NOX_REQUIRE(none.Load("x = 1\n"));
	NOX_CHECK(!none.world.HasSaveGameHook());
	NOX_CHECK(none.world.RunConfigureHook(error));
}

NOX_TEST(LuaData, InlineRoadsAndDeclaredQueries) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	fx.Put("projects/t/data/net.json", InlineNetwork().dump());

	Harness h;
	h.project.roads = { NoxLua::DeclaredFile{ "net", "projects/t/data/net.json" } };
	NOX_REQUIRE(h.Load(
		"local declared = nox.roads('net')\n"
		"dcount = declared:edge_count()\n"
		"local inline = nox.roads.from_table({ road_half_width = 5, sidewalk_width = 4,\n"
		"  nodes = { { id = 'a', pos = { 0, 0 } }, { id = 'b', pos = { 100, 0 } }, { id = 'c', pos = { 100, 100 } } },\n"
		"  edges = { { a = 'a', b = 'b', name = 'Main', lanes = 2 }, { 'b', 'c' } } })\n"
		"icount = inline:edge_count()\n"
		"local near = inline:edges_in_rect(-10, -10, 10, 10)\n"
		"nearn = #near\n"
		"nearfirst = near[1]\n"
		"local wide = inline:edges_in_rect(90, -10, 200, 200)\n"
		"widen = #wide\n"
		"local none = inline:edges_in_rect(500, 500, 600, 600)\n"
		"nonen = #none\n"
		"local poly = inline:edge_polyline(2, 2)\n"
		"polyn = #poly\n"
		"polyx = poly[1]\n"
		"polyz = poly[6]\n"
		"local hw, y, attrs = inline:edge_info(1)\n"
		"infohw = hw\n"
		"infoy = y\n"
		"infoname = attrs.name\n"
		"infolanes = attrs.lanes\n"
		"infofrom = attrs.from\n"
		"infoto = attrs.to\n"
		"infooneway = attrs.oneway\n"
		"ok1, err1 = pcall(function() return inline:edge_info(9) end)\n"
		"ok2, err2 = pcall(function() return nox.roads.from_table({ file = 'x.json' }) end)\n"
		"ok3, err3 = pcall(function() return nox.roads.from_table({}) end)\n"
		"ok4, err4 = pcall(function() return nox.roads('missing') end)\n"));
	NOX_CHECK(h.Num("dcount") == 2);
	NOX_CHECK(h.Num("icount") == 2);
	NOX_CHECK(h.Num("nearn") == 1);
	NOX_CHECK(h.Num("nearfirst") == 1);
	NOX_CHECK(h.Num("widen") == 2);
	NOX_CHECK(h.Num("nonen") == 0);
	NOX_CHECK(h.Num("polyn") == 6);
	NOX_CHECK(std::fabs(h.Num("polyx") - 100.0) < 1e-4);
	NOX_CHECK(std::fabs(h.Num("polyz") - 100.0) < 1e-4);
	NOX_CHECK(std::fabs(h.Num("infohw") - 5.0) < 1e-4);
	NOX_CHECK(std::fabs(h.Num("infoy")) < 1e-4);
	NOX_CHECK(h.Str("infoname") == "Main");
	NOX_CHECK(h.Num("infolanes") == 2);
	NOX_CHECK(h.Str("infofrom") == "a");
	NOX_CHECK(h.Str("infoto") == "b");
	NOX_CHECK(!h.Bool("infooneway"));
	NOX_CHECK(!h.Bool("ok1"));
	NOX_CHECK(Contains(h.Str("err1"), "out of range"));
	NOX_CHECK(!h.Bool("ok2"));
	NOX_CHECK(Contains(h.Str("err2"), "inline form"));
	NOX_CHECK(!h.Bool("ok3"));
	NOX_CHECK(!h.Bool("ok4"));
}

NOX_TEST(LuaData, CameraProjectionAndOverlayRect) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	Camera camera(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f, 60.0f, 0.1f, 1000.0f, 10.0f, 0.1f);
	h.world.SetCamera(&camera);
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  cx, cy, cd, cv = ctx:camera_project(0, 0, -10)\n"
		"  bv = select(4, ctx:camera_project(0, 0, 10))\n"
		"  sv = select(4, ctx:camera_project(1000, 0, -10))\n"
		"  ox, oy, ow, oh = ctx:overlay_rect()\n"
		"end)\n"));
	h.Draw(4.0f, 8.0f, 800.0f, 600.0f);
	NOX_CHECK(std::fabs(h.Num("cx") - 0.5) < 1e-3);
	NOX_CHECK(std::fabs(h.Num("cy") - 0.5) < 1e-3);
	NOX_CHECK(std::fabs(h.Num("cd") - 10.0) < 1e-3);
	NOX_CHECK(h.Bool("cv"));
	NOX_CHECK(!h.Bool("bv"));   // behind the camera
	NOX_CHECK(!h.Bool("sv"));   // far outside the view
	NOX_CHECK(h.Num("ox") == 4);
	NOX_CHECK(h.Num("oy") == 8);
	NOX_CHECK(h.Num("ow") == 800);
	NOX_CHECK(h.Num("oh") == 600);
}

NOX_TEST(LuaData, UiTextureAndIconPolicy) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	NoxLua::UiTextureCache cache;
	h.world.SetTextures(&cache);
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  tok, treason = ui.texture('textures/missing.png', 0, 0, 10, 10)\n"
		"  pok, preason = ui.texture('shaders/x.png', 0, 0, 10, 10)\n"
		"  iex = ui.icon_exists('nothing/here')\n"
		"end)\n"));
	h.Draw();
	NOX_CHECK(!h.Bool("tok"));
	NOX_CHECK(Contains(h.Str("treason"), "no such image"));
	NOX_CHECK(!h.Bool("pok"));
	NOX_CHECK(Contains(h.Str("preason"), "not readable"));
	NOX_CHECK(!h.Bool("iex"));   // no icon atlas in a headless run
	NOX_CHECK(cache.Count() == 0);
	NOX_CHECK(cache.Bytes() == 0);
}

NOX_TEST(LuaData, RequestExitFlag) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness quiet;
	NOX_REQUIRE(quiet.Load("nox.on_draw(function(ctx, ui, dt) end)\n"));
	quiet.Draw();
	NOX_CHECK(!quiet.world.ExitRequested());
	Harness exiting;
	NOX_REQUIRE(exiting.Load("nox.on_draw(function(ctx, ui, dt) ctx:request_exit() end)\n"));
	exiting.Draw();
	NOX_CHECK(exiting.world.ExitRequested());
}

NOX_TEST(LuaData, SceneCollisionFilesAndRawLog) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	std::vector<std::string> lines;
	h.world.GetRuntime().SetLogSink([&lines](const std::string& line) { lines.push_back(line); });
	const nlohmann::json game = { { "collision", { { "files", nlohmann::json::array({ "models/town_collision.glb" }) } } } };
	NOX_REQUIRE(h.Load(
		"nox.log_raw('[Race] start')\n"
		"local ok1, e1 = pcall(function() return nox.collision_file('models/other.glb') end)\n"
		"nox_ok1 = ok1\n"
		"nox_e1 = e1\n"
		"local ok2, e2 = pcall(function() return nox.collision_file('models/town_collision.glb') end)\n"
		"nox_ok2 = ok2\n"
		"nox_e2 = e2\n"
		"local ok3, e3 = pcall(function() return nox.collision_file('../models/x.glb') end)\n"
		"nox_ok3 = ok3\n",
		game));
	NOX_REQUIRE(!lines.empty());
	NOX_CHECK(lines.front() == "[Race] start");
	NOX_CHECK(!h.Bool("nox_ok1"));
	NOX_CHECK(Contains(h.Str("nox_e1"), "neither declared"));
	// The scene names this file, so it passes the policy; the file itself is absent here, so loading fails
	// with the "no triangle meshes" error instead of the policy error.
	NOX_CHECK(!h.Bool("nox_ok2"));
	NOX_CHECK(!Contains(h.Str("nox_e2"), "neither declared"));
	NOX_CHECK(!h.Bool("nox_ok3"));
}

NOX_TEST(LuaData, DescriptorAuthoringRoots) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	NoxLua::ProjectDescriptor out;
	std::string error;
	const nlohmann::json good = {
		{ "format", 1 }, { "runtime", "lua" }, { "api_version", 1 }, { "entry", "scripts/main.lua" },
		{ "authoring_roots", { "config/hud.json", "data/" } },
	};
	NOX_REQUIRE(NoxLua::LoadProjectDescriptor("projects/t/project.json", good, out, error));
	NOX_CHECK(out.content.authoringRoots.size() == 2);
	NOX_CHECK(out.content.authoringRoots[1] == "data");
	NOX_CHECK(out.content.projectDir == "projects/t");
	NOX_CHECK(out.content.descriptor.value("runtime", std::string()) == "lua");

	nlohmann::json generated = good;
	generated["authoring_roots"] = { "generated/cache" };
	NoxLua::ProjectDescriptor rejected;
	NOX_CHECK(!NoxLua::LoadProjectDescriptor("projects/t/project.json", generated, rejected, error));
	NOX_CHECK(Contains(error, "generated"));

	nlohmann::json escaping = good;
	escaping["authoring_roots"] = { "../outside" };
	NOX_CHECK(!NoxLua::LoadProjectDescriptor("projects/t/project.json", escaping, rejected, error));
}

// Empty arrays keep their shape: nox.game() -> on_save_game round trips, nox.array, and the scene game objects.
NOX_TEST(LuaData, EmptyArraysSaveAsArrays) {
	Harness h;
	const nlohmann::json game = nlohmann::json::parse(
		"{\"events\": [], \"places\": [], \"name\": \"x\", \"nested\": {\"list\": []}, \"plain\": {}}");
	NOX_REQUIRE(h.Load("nox.on_save_game(function() return nox.game() end)\n", game, nlohmann::json{ { "name", "scene" } }));
	std::string error;
	nlohmann::json saved;
	NOX_REQUIRE(h.world.SaveGameJson(saved, error));
	NOX_CHECK(saved.dump(2) == game.dump(2));
	NOX_CHECK(saved["events"].is_array());
	NOX_CHECK(saved["plain"].is_object());

	Harness marked;
	NOX_REQUIRE(marked.Load(
		"nox.on_save_game(function()\n"
		"  return { list = nox.array(), copy = nox.array({}), plain = {}, n = 1 }\n"
		"end)\n",
		nlohmann::json::object(), nlohmann::json{ { "name", "scene" } }));
	nlohmann::json out;
	NOX_REQUIRE(marked.world.SaveGameJson(out, error));
	NOX_CHECK(out["list"].is_array() && out["list"].empty());
	NOX_CHECK(out["copy"].is_array() && out["copy"].empty());
	NOX_CHECK(out["plain"].is_object());
	NOX_CHECK(out["n"] == 1);

	Harness mixed;
	NOX_REQUIRE(mixed.Load(
		"nox.on_save_game(function() return { t = nox.array({ key = 1 }) } end)\n",
		nlohmann::json::object(), nlohmann::json{ { "name", "scene" } }));
	NOX_CHECK(!mixed.world.SaveGameJson(out, error));
}

// The shipped game objects survive nox.game() -> on_save_game unchanged (byte-identical after dump(2)).
NOX_TEST(LuaData, SceneGameObjectsRoundTripUnchanged) {
	const char* scenes[] = { "scenes/nfs_sprint.json", "scenes/shuto_buildings.json" };
	for (const char* path : scenes) {
		std::ifstream in(path, std::ios::binary);
		NOX_REQUIRE(in.good());
		const nlohmann::json root = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), nullptr, false);
		NOX_REQUIRE(root.is_object());
		NOX_REQUIRE(root.contains("game"));
		const nlohmann::json game = root["game"];
		Harness h;
		NOX_REQUIRE(h.Load("nox.on_save_game(function() return nox.game() end)\n", game, root));
		std::string error;
		nlohmann::json saved;
		NOX_REQUIRE(h.world.SaveGameJson(saved, error));
		NOX_CHECK(saved.dump(2) == game.dump(2));
	}
}

// Inline road networks: identical tables share one network, nox.roads.release frees it, and the road cap counts live networks.
NOX_TEST(LuaData, InlineRoadNetworksShareAndRelease) {
	AssetFixture fx;
	NOX_REQUIRE(fx.ok);
	Harness h;
	NOX_REQUIRE(h.Load(
		"local function net(w)\n"
		"  return { road_half_width = w, nodes = { { id = 'a', pos = { 0, 0 } }, { id = 'b', pos = { 100, 0 } } },\n"
		"    edges = { { 'a', 'b' } } }\n"
		"end\n"
		"local r1 = nox.roads.from_table(net(5))\n"
		"local r2 = nox.roads.from_table(net(5))\n"
		"shared_ok = r1:edge_count() == r2:edge_count()\n"
		"nox.roads.release(r2)\n"
		"shared_gone = not pcall(function() return r1:edge_count() end)\n"
		"local r3 = nox.roads.from_table(net(5))\n"
		"rebuilt = r3:edge_count()\n"
		"for i = 1, 7 do nox.roads.from_table(net(10 + i)) end\n"
		"capped = not pcall(nox.roads.from_table, net(50))\n"
		"nox.roads.release(r3)\n"
		"freed = pcall(nox.roads.from_table, net(50))\n"
		"nox.roads.release(r3)\n"));   // releasing twice is a no-op
	NOX_CHECK(h.Bool("shared_ok"));
	NOX_CHECK(h.Bool("shared_gone"));
	NOX_CHECK(h.Num("rebuilt") == 1);
	NOX_CHECK(h.Bool("capped"));
	NOX_CHECK(h.Bool("freed"));
	NOX_CHECK(h.world.Diagnostics().empty());
}
