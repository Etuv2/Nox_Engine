// Lua gameplay runtime (docs/lua_api.md, "Lifecycle, timers and coroutines" and "Scenes and saves"): behaviour hooks,
// timers, coroutines, runtime instances, scene payloads and save slots. Headless: a LuaWorld over a SceneGraph, prefab
// and user files in a private AssetIO sandbox, no GPU.
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
#include "scripting/LuaScene.h"
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

	// Private loose AssetIO tree (one prefab, a user directory for save slots); the previous configuration is restored.
	struct Sandbox {
		fs::path root;
		bool hadConfig = false;
		AssetIO::Config previous;
		bool ok = false;

		Sandbox() {
			if (AssetIO::IsInitialized()) {
				hadConfig = true;
				previous = AssetIO::GetConfig();
				AssetIO::Shutdown();
			}
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() / ("nox_lua_gameplay_" + std::to_string(stamp));
			fs::create_directories(root / "prefabs");
			fs::create_directories(root / "user");
			std::ofstream out(root / "prefabs" / "crate.prefab.json", std::ios::binary);
			out << R"({"type":"node","name":"Crate","children":[{"type":"node","name":"Lid"}]})";
			out.close();
			AssetIO::Config config;
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = root;
			config.cacheDir = root / "cache";
			config.userDir = root / "user";
			config.projectName = "lua-gameplay-test";
			std::string error;
			ok = AssetIO::Initialize(config, &error);
			nox::reflect::RegisterBuiltinComponents();
		}

		~Sandbox() {
			AssetIO::Shutdown();
			if (hadConfig) AssetIO::Initialize(previous);
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	const char* kPrelude = "function try(f) local ok, e = pcall(f) if ok then return '' end return tostring(e) end\n";
	const char* kEntry = "projects/t/scripts/main.lua";

	struct Harness {
		Sandbox sandbox;
		MemorySource source;
		SceneGraph scene;
		PhysicsEngine physics;
		SceneLoader loader;
		NoxLua::LuaWorld world;
		std::string requested;   // last ctx:load_scene path the host received

		Harness()
			: loader(nullptr, nullptr, 1280, 720),
			  world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			nox::reflect::EngineServices& services = nox::reflect::Services();
			services.instantiatePrefab = [this](const std::string& path, const std::shared_ptr<SceneNode>& parent,
				const nlohmann::json& placement, std::string& error) {
				return nox::prefab::InstantiatePrefab(loader, scene, path, parent ? parent : scene.GetRoot(), placement, &error);
			};
			services.destroyNode = [this](const std::shared_ptr<SceneNode>& node) {
				return PlayWorldSnapshot::RemoveSubtree(scene, node, &physics);
			};
			services.requestSceneSwap = [this](const std::string& path) { requested = path; };
		}

		~Harness() {
			nox::reflect::EngineServices& services = nox::reflect::Services();
			services.instantiatePrefab = nullptr;
			services.destroyNode = nullptr;
			services.requestSceneSwap = nullptr;
		}

		bool Load(const std::string& body) {
			source.files[kEntry] = std::string(kPrelude) + body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}

		void Play() { world.BeginPlay(); }
		void Frame() { world.Update(1.0f / 60.0f, true); }
		void Frames(int n) {
			for (int i = 0; i < n; ++i) Frame();
		}

		double GlobalNum(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const double value = lua_tonumber(L, -1);
			lua_pop(L, 1);
			return value;
		}

		std::string GlobalStr(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const char* text = lua_tostring(L, -1);
			std::string value = text ? text : "";
			lua_pop(L, 1);
			return value;
		}

		bool GlobalTruthy(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const bool value = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		}

		std::shared_ptr<SceneNode> ChildNamed(const std::string& name) const {
			for (const auto& child : scene.GetRoot()->children) {
				if (child && child->GetName() == name) return child;
			}
			return nullptr;
		}

		const NoxLua::Diagnostic* FindDiagnostic(const std::string& source) const {
			for (const NoxLua::Diagnostic& d : world.Diagnostics()) {
				if (d.source == source) return &d;
			}
			return nullptr;
		}
	};
}

NOX_TEST(LuaGameplay, BehaviourOnStartRunsOnceAndOnDestroyOnceForInstantiatedNode)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"starts = 0 ticks = 0 stops = 0\n"
		"nox.behavior{ name = 'spin',\n"
		"  on_start = function(ctx, e) starts = starts + 1 end,\n"
		"  update = function(ctx, e, dt) ticks = ticks + 1 end,\n"
		"  on_destroy = function(ctx, e) stops = stops + 1 end }\n"
		"nox.system{ name = 'spawner', update = function(ctx, dt)\n"
		"  if ctx:frame() == 1 then\n"
		"    crate = ctx:instantiate('prefabs/crate.prefab.json', {name = 'Crate A'})\n"
		"    ctx:attach(crate, 'spin')\n"
		"  elseif ctx:frame() == 6 then ctx:destroy(crate) end\n"
		"end }\n"));
	h.Play();
	h.Frames(1);
	NOX_CHECK(h.GlobalNum("starts") == 1);
	NOX_CHECK(h.GlobalNum("ticks") == 1);
	h.Frames(4);   // frames 2 to 5
	NOX_CHECK(h.GlobalNum("starts") == 1);
	NOX_CHECK(h.GlobalNum("ticks") == 5);
	h.Frames(1);   // frame 6: ctx:destroy is applied when the system returns, before behaviours run
	NOX_CHECK(h.GlobalNum("stops") == 1);
	NOX_CHECK(h.GlobalNum("ticks") == 5);
	NOX_CHECK(h.ChildNamed("Crate A") == nullptr);
	h.world.EndPlay();
	NOX_CHECK(h.GlobalNum("stops") == 1);   // not run again at Stop
}

NOX_TEST(LuaGameplay, AfterFiresAtFixedStep30AndNeverAfterStop)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"fired = -1 count = 0\n"
		"nox.on('begin_play', function(ctx, payload)\n"
		"  ctx:after(0.5, function(c) fired = c:frame() count = count + 1 end)\n"
		"  ctx:after(5.0, function(c) late = true end)\n"
		"end)\n"));
	h.Play();
	h.Frames(29);
	NOX_CHECK(h.GlobalNum("fired") == -1);
	h.Frame();   // step 30 at 60 Hz
	NOX_CHECK(h.GlobalNum("fired") == 30);
	h.Frames(60);
	NOX_CHECK(h.GlobalNum("count") == 1);   // one-shot
	h.world.EndPlay();
	h.Frames(100);   // not playing: the play clock does not advance and nothing is due
	NOX_CHECK(!h.GlobalTruthy("late"));
	NOX_CHECK(h.GlobalNum("count") == 1);
}

NOX_TEST(LuaGameplay, EveryRepeatsUntilCancelled)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"ticks = 0 handle = nil active_after = nil\n"
		"nox.on('begin_play', function(ctx) handle = ctx:every(0.25, function(c) ticks = ticks + 1 end) end)\n"
		"nox.system{ name = 'canceller', update = function(ctx)\n"
		"  if ctx:frame() == 61 then handle:cancel() active_after = handle:active() end\n"
		"end }\n"));
	h.Play();
	h.Frames(60);
	NOX_CHECK(h.GlobalNum("ticks") == 4);   // frames 15, 30, 45, 60
	h.Frames(60);
	NOX_CHECK(h.GlobalNum("ticks") == 4);   // cancelled at frame 61
	NOX_CHECK(!h.GlobalTruthy("active_after"));
}

NOX_TEST(LuaGameplay, CoroutineWaitResumesOnTheRightFrame)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"startFrame = 0 resumedFrame = 0\n"
		"nox.on('begin_play', function(ctx)\n"
		"  ctx:start_coroutine(function(c)\n"
		"    startFrame = c:frame()\n"
		"    c:wait(0.25)\n"
		"    resumedFrame = c:frame()\n"
		"  end)\n"
		"end)\n"));
	h.Play();
	h.Frames(20);
	NOX_CHECK(h.GlobalNum("startFrame") == 1);    // first segment runs at the start of the first frame
	NOX_CHECK(h.GlobalNum("resumedFrame") == 16); // 15 frames = 0.25 s at 60 Hz after the wait began
}

NOX_TEST(LuaGameplay, CoroutineWaitFramesAndWaitUntil)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"framesElapsed = -1 untilFrame = -1 gate = false\n"
		"nox.on('begin_play', function(ctx)\n"
		"  ctx:start_coroutine(function(c)\n"
		"    local s = c:frame()\n"
		"    c:wait_frames(3)\n"
		"    framesElapsed = c:frame() - s\n"
		"  end)\n"
		"  ctx:start_coroutine(function(c)\n"
		"    c:wait_until(function(c2) return gate end)\n"
		"    untilFrame = c:frame()\n"
		"  end)\n"
		"end)\n"
		"nox.system{ name = 'gate', update = function(ctx) if ctx:frame() == 5 then gate = true end end }\n"));
	h.Play();
	h.Frames(10);
	NOX_CHECK(h.GlobalNum("framesElapsed") == 3);
	NOX_CHECK(h.GlobalNum("untilFrame") == 6);   // gate is set by a system in frame 5; the predicate sees it in frame 6
}

NOX_TEST(LuaGameplay, EntityScopedTimerAndCoroutineCancelWhenTheEntityIsDestroyed)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"fired = false bodyDone = false\n"
		"nox.system{ name = 's', update = function(ctx)\n"
		"  local f = ctx:frame()\n"
		"  if f == 1 then token = ctx:spawn('ghost')\n"
		"  elseif f == 2 then\n"
		"    g = ctx:resolve_spawn(token)\n"
		"    ctx:after(0.5, function(c) fired = true end, g)\n"
		"    ctx:start_coroutine(function(c) c:wait(1.0) bodyDone = true end, g)\n"
		"  elseif f == 3 then ctx:destroy(g) end\n"
		"end }\n"));
	h.Play();
	h.Frames(90);
	NOX_CHECK(!h.GlobalTruthy("fired"));
	NOX_CHECK(!h.GlobalTruthy("bodyDone"));
}

NOX_TEST(LuaGameplay, OnDestroyRunsAtStopForSpawnedEntitiesAndWaitOutsideCoroutineIsAnError)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"started = 0 ended = 0 outside = ''\n"
		"nox.behavior{ name = 'ghostly',\n"
		"  on_start = function(ctx, e) started = started + 1 end,\n"
		"  on_destroy = function(ctx, e) ended = ended + 1 end }\n"
		"nox.system{ name = 's', update = function(ctx)\n"
		"  if ctx:frame() == 1 then token = ctx:spawn('g')\n"
		"  elseif ctx:frame() == 2 then ctx:attach(ctx:resolve_spawn(token), 'ghostly')\n"
		"  elseif ctx:frame() == 3 then outside = try(function() ctx:wait(1) end) end\n"
		"end }\n"));
	h.Play();
	h.Frames(3);
	NOX_CHECK(h.GlobalNum("started") == 1);
	NOX_CHECK(h.GlobalNum("ended") == 0);
	NOX_CHECK(!h.GlobalStr("outside").empty());
	h.world.EndPlay();
	NOX_CHECK(h.GlobalNum("ended") == 1);   // the spawned entity is still alive at Stop, so its on_destroy runs
	NOX_CHECK(h.world.SpawnedCount() == 0);
}

NOX_TEST(LuaGameplay, CoroutineErrorIsDiagnosedWithTraceback)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"nox.on('begin_play', function(ctx)\n"
		"  ctx:start_coroutine(function(c) c:wait_frames(1) error('boom') end)\n"
		"end)\n"));
	h.Play();
	h.Frames(3);
	const NoxLua::Diagnostic* d = h.FindDiagnostic("coroutine");
	NOX_REQUIRE(d != nullptr);
	NOX_CHECK(d->message.find("boom") != std::string::npos);
	NOX_CHECK(!d->traceback.empty());
	h.Frames(3);   // the failed coroutine does not run again
	NOX_CHECK(h.world.Diagnostics().size() == 1);
}

NOX_TEST(LuaGameplay, ScenePayloadReachesBeginPlayAndCopiesPlainData)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"got = 'none' count_type = 'none' copy = 'none'\n"
		"nox.on('begin_play', function(ctx, p)\n"
		"  got = p and p.name or 'nil'\n"
		"  count_type = math.type(p.count)\n"
		"  copy = ctx:scene_payload().name\n"
		"end)\n"));
	nlohmann::json payload = { { "name", "arena" }, { "count", 3 } };
	h.world.SetScenePayload(payload);
	h.Play();
	NOX_CHECK(h.GlobalStr("got") == "arena");
	NOX_CHECK(h.GlobalStr("count_type") == "integer");
	NOX_CHECK(h.GlobalStr("copy") == "arena");
}

NOX_TEST(LuaGameplay, LoadSceneHandsThePayloadToTheNextScene)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"ok = 'unset' bad = 'unset'\n"
		"nox.system{ name = 'go', update = function(ctx)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  ok = try(function() ctx:load_scene('scenes/next.json', {level = 2, tags = {'x', 'y'}, flag = true, ratio = 0.5}) end)\n"
		"  bad = try(function() ctx:load_scene('scenes/next.json', {f = print}) end)\n"
		"end }\n"));
	h.Play();
	h.Frame();
	NOX_CHECK(h.GlobalStr("ok").empty());
	NOX_CHECK(!h.GlobalStr("bad").empty());   // functions are not plain data: the request is refused
	NOX_CHECK(h.requested == "scenes/next.json");
	const std::optional<nlohmann::json> payload = NoxLua::TakePendingScenePayload();
	NOX_REQUIRE(payload.has_value());
	NOX_CHECK((*payload)["level"] == 2);
	NOX_CHECK((*payload)["tags"].size() == 2);
	NOX_CHECK((*payload)["flag"] == true);
	NOX_CHECK((*payload)["ratio"] == 0.5);
	NOX_CHECK(!NoxLua::TakePendingScenePayload().has_value());   // taken once
}

NOX_TEST(LuaGameplay, SaveSlotsStoreListDeleteAndRejectPathEscapes)
{
	Harness h;
	NOX_REQUIRE(h.sandbox.ok);
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'saves', update = function(ctx)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  saved1 = ctx:save_slot('slot_1', {gold = 5, name = 'a'})\n"
		"  saved2 = ctx:save_slot('slot_2', {gold = 9})\n"
		"  okEscape, whyEscape = ctx:save_slot('../evil', {})\n"
		"  okSep = ctx:save_slot('a/b', {})\n"
		"  okDot = ctx:save_slot('x.json', {})\n"
		"  loaded = ctx:load_slot('slot_1')\n"
		"  gold = loaded.gold\n"
		"  existsOne = ctx:save_exists('slot_1')\n"
		"  existsEscape = ctx:save_exists('../evil')\n"
		"  names = ctx:list_saves()\n"
		"  listCount = #names\n"
		"  firstName = names[1]\n"
		"  secondName = names[2]\n"
		"  removed = ctx:delete_save('slot_1')\n"
		"  gone = not ctx:save_exists('slot_1')\n"
		"  missing, missingWhy = ctx:load_slot('nope')\n"
		"end }\n"));
	h.Play();
	h.Frame();
	NOX_CHECK(h.GlobalTruthy("saved1"));
	NOX_CHECK(h.GlobalTruthy("saved2"));
	NOX_CHECK(!h.GlobalTruthy("okEscape"));
	NOX_CHECK(!h.GlobalStr("whyEscape").empty());
	NOX_CHECK(!h.GlobalTruthy("okSep"));
	NOX_CHECK(!h.GlobalTruthy("okDot"));
	NOX_CHECK(h.GlobalNum("gold") == 5);
	NOX_CHECK(h.GlobalTruthy("existsOne"));
	NOX_CHECK(!h.GlobalTruthy("existsEscape"));
	NOX_CHECK(h.GlobalNum("listCount") == 2);
	NOX_CHECK(h.GlobalStr("firstName") == "slot_1");
	NOX_CHECK(h.GlobalStr("secondName") == "slot_2");
	NOX_CHECK(h.GlobalTruthy("removed"));
	NOX_CHECK(h.GlobalTruthy("gone"));
	NOX_CHECK(!h.GlobalStr("missing").size());   // nil prints as an empty string through GlobalStr
	// Slots live in the user directory, under saves/.
	NOX_CHECK(fs::exists(h.sandbox.root / "user" / "saves" / "slot_2.json"));
	NOX_CHECK(!fs::exists(h.sandbox.root / "user" / "saves" / "slot_1.json"));
	NOX_CHECK(!fs::exists(h.sandbox.root / "user" / "evil.json"));
}
