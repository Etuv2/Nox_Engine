// Lua scene instancing (ctx:instantiate, ctx:destroy on scene nodes) and the Play/Stop restore that goes with it.
// Headless: a SceneGraph, a LuaWorld, prefab files in a private loose AssetIO sandbox, no GPU.
#include "TestFramework.h"

#include "PhysicsEngine.h"
#include "PlayWorldSnapshot.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "assets/AssetIO.h"
#include "reflect/ReflectServices.h"
#include "reflect/Reflection.h"
#include "scene/Prefab.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace fs = std::filesystem;

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

	// Private loose AssetIO tree with one prefab; the previous AssetIO configuration is restored on exit.
	struct PrefabSandbox {
		fs::path root;
		bool hadConfig = false;
		AssetIO::Config previous;
		bool ok = false;

		PrefabSandbox() {
			if (AssetIO::IsInitialized()) {
				hadConfig = true;
				previous = AssetIO::GetConfig();
				AssetIO::Shutdown();
			}
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() / ("nox_lua_scene_" + std::to_string(stamp));
			fs::create_directories(root / "prefabs");
			std::ofstream out(root / "prefabs" / "crate.prefab.json", std::ios::binary);
			out << R"({"type":"node","name":"Crate","children":[{"type":"node","name":"Lid"}]})";
			out.close();
			AssetIO::Config config;
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = root;
			config.cacheDir = root / "cache";
			config.userDir = root / "user";
			config.projectName = "lua-scene-test";
			std::string error;
			ok = AssetIO::Initialize(config, &error);
			nox::reflect::RegisterBuiltinComponents();
		}

		~PrefabSandbox() {
			AssetIO::Shutdown();
			if (hadConfig) AssetIO::Initialize(previous);
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	const char* kPrelude =
		"function try(f) local ok, e = pcall(f) if ok then return '' end return tostring(e) end\n";
	const char* kEntry = "projects/t/scripts/main.lua";

	// Scene with one authored node ("probe"), with the services wired as Core wires them.
	struct Harness {
		PrefabSandbox sandbox;
		MemorySource source;
		SceneGraph scene;
		PhysicsEngine physics;
		SceneLoader loader;
		NoxLua::LuaWorld world;
		PlayWorldSnapshot snapshot;
		std::shared_ptr<SceneNode> probe;

		Harness()
			: loader(nullptr, nullptr, 1280, 720),
			  world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			probe = loader.InstantiateNode(scene, nlohmann::json{ {"type", "node"}, {"name", "probe"}, {"position", {0, 1, 0}} },
				scene.GetRoot(), "scenes/_lua_scene_test.json");
			nox::reflect::EngineServices& services = nox::reflect::Services();
			services.instantiatePrefab = [this](const std::string& path, const std::shared_ptr<SceneNode>& parent,
				const nlohmann::json& placement, std::string& error) {
				return nox::prefab::InstantiatePrefab(loader, scene, path, parent ? parent : scene.GetRoot(), placement, &error);
			};
			services.destroyNode = [this](const std::shared_ptr<SceneNode>& node) {
				return PlayWorldSnapshot::RemoveSubtree(scene, node, &physics);
			};
		}

		~Harness() {
			nox::reflect::EngineServices& services = nox::reflect::Services();
			services.instantiatePrefab = nullptr;
			services.destroyNode = nullptr;
		}

		bool Load(const std::string& body) {
			source.files[kEntry] = std::string(kPrelude) + body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			const bool loaded = world.Load(project, scene, nlohmann::json::object());
			if (loaded) world.BeginPlay();   // systems run only while playing (Core::RequestPlay does this)
			return loaded;
		}

		void Frame() { world.Update(1.0f / 60.0f, true); }

		std::string GlobalStr(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const char* text = lua_tostring(L, -1);
			std::string value = text ? text : "";
			lua_pop(L, 1);
			return value;
		}

		std::shared_ptr<SceneNode> ChildNamed(const std::string& name) const {
			for (const auto& child : scene.GetRoot()->children) {
				if (child && child->GetName() == name) return child;
			}
			return nullptr;
		}
	};
}

NOX_TEST(LuaScene, InstantiateCreatesAuthoredLikeNodesAndDestroyRemovesSceneNodes)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.probe != nullptr);
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'spawner', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local crate = ctx:instantiate('prefabs/crate.prefab.json', {name = 'Crate A', position = {4, 0, 0}})\n"
		"  made = tostring(crate ~= nil)\n"
		"  missing = try(function() ctx:instantiate('prefabs/missing.prefab.json') end)\n"
		"  badpath = try(function() ctx:instantiate('../x.prefab.json') end)\n"
		"  ctx:destroy(ctx:find('probe'))\n"
		"end}\n"));

	h.Frame();
	auto crate = h.ChildNamed("Crate A");
	NOX_REQUIRE(crate != nullptr);
	NOX_CHECK(h.GlobalStr("made") == "true");
	NOX_CHECK(crate->GetPrefabPath() == "prefabs/crate.prefab.json");
	NOX_CHECK(crate->GetPosition() == glm::vec3(4.0f, 0.0f, 0.0f));
	NOX_REQUIRE(crate->children.size() == 1);
	NOX_CHECK(crate->GetEntityID() != INVALID_ENTITY);
	// Invalid requests are Lua errors and create nothing.
	NOX_CHECK(!h.GlobalStr("missing").empty());
	NOX_CHECK(!h.GlobalStr("badpath").empty());
	NOX_CHECK(h.scene.GetRoot()->children.size() == 1);   // only "Crate A"; "probe" was destroyed
	NOX_CHECK(h.ChildNamed("probe") == nullptr);
}

NOX_TEST(LuaScene, StopRemovesInstancesAndRecreatesDestroyedAuthoredNodes)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.probe != nullptr);
	h.snapshot.Capture(h.scene, h.loader);   // authored state, before play
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'spawner', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  ctx:instantiate('prefabs/crate.prefab.json', {name = 'Crate B'})\n"
		"  ctx:destroy(ctx:find('probe'))\n"
		"end}\n"));
	h.Frame();
	NOX_CHECK(h.ChildNamed("Crate B") != nullptr);
	NOX_CHECK(h.ChildNamed("probe") == nullptr);

	const PlayWorldSnapshot::Report report = h.snapshot.Restore(h.scene, &h.physics, h.loader, "scenes/_lua_scene_test.json");
	NOX_CHECK(report.removedNodes == 2);      // Crate B and its Lid
	NOX_CHECK(report.recreatedNodes == 1);    // probe
	NOX_CHECK(h.ChildNamed("Crate B") == nullptr);
	NOX_CHECK(h.ChildNamed("probe") != nullptr);
	NOX_CHECK(h.ChildNamed("probe")->GetPosition() == glm::vec3(0.0f, 1.0f, 0.0f));
}
