// Lua world: project schemas, safe ECS access, deferred commands, faults, events, fixed substeps.
#include "TestFramework.h"

#include "PhysicsEngine.h"
#include "SceneGraph.h"
#include "TransformSystem.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

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

	// Schemas used by the tests. Declared by Lua, never by C++.
	const char* kPrelude =
		"nox.schema('Health', {current = 'integer', maximum = 'integer'}, {current = 100, maximum = 100})\n"
		"nox.schema('Quest', {state = 'string', progress = 'number', done = 'boolean'}, {state = 'open', progress = 0, done = false})\n"
		"nox.schema('Inventory', {gold = 'integer'}, {gold = 0})\n";

	struct Harness {
		MemorySource source;
		SceneGraph scene;
		NoxLua::LuaWorld world;

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
		}

		EntityID AddNode(const std::string& name, const glm::vec3& position) {
			const EntityID id = scene.GetComponentManager()->CreateEntity(name);
			scene.GetTransformSystem()->SetPosition(id, position);
			return id;
		}

		bool Load(const std::string& body, const nlohmann::json& authored = nlohmann::json::object()) {
			source.files[kEntry] = std::string(kPrelude) + body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, authored.value("entities", nlohmann::json::object()));
		}

		// Frame update; the world must be playing.
		void Frame(float dt = 1.0f / 60.0f) { world.Update(dt, true); }

		long long GlobalInt(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const long long value = lua_isinteger(L, -1) ? static_cast<long long>(lua_tointeger(L, -1)) : -1;
			lua_pop(L, 1);
			return value;
		}

		std::string GlobalStr(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const char* s = lua_tostring(L, -1);
			std::string value = s ? s : "";
			lua_pop(L, 1);
			return value;
		}

		bool HasDiagnostic(const std::string& needle) const {
			for (const auto& d : world.Diagnostics()) {
				if (d.message.find(needle) != std::string::npos) return true;
			}
			return false;
		}
	};
}

NOX_TEST(LuaWorld, SchemasLoadAndCopyWithoutNativeEdits)
{
	Harness h;
	h.AddNode("hero", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'init', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local hero = ctx:find('hero')\n"
		"  ctx:add(hero, 'Health', {current = 40})\n"
		"  ctx:add(hero, 'Quest')\n"
		"  ctx:set_field(hero, 'Quest', 'progress', 0.5)\n"
		"  local copy = ctx:get(hero, 'Health')\n"
		"  copy.current = 999\n"   // a copy: must not write back
		"  if ctx:get_field(hero, 'Health', 'current') ~= 40 then error('reads must be copies') end\n"
		"end}\n"));
	NOX_REQUIRE(h.world.IsLoaded());
	NOX_CHECK_EQ(h.world.Schemas().size(), size_t(3));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());

	const EntityHandle hero = h.world.FindByName("hero");
	NOX_REQUIRE(hero.id != INVALID_ENTITY);
	NoxLua::Value value;
	NOX_REQUIRE(h.world.GetDeclaredField(hero, "Health", "current", value));
	NOX_CHECK(std::holds_alternative<int64_t>(value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(40));
	NOX_REQUIRE(h.world.GetDeclaredField(hero, "Quest", "state", value));
	NOX_CHECK_EQ(std::get<std::string>(value), std::string("open"));
	NOX_REQUIRE(h.world.GetDeclaredField(hero, "Quest", "progress", value));
	NOX_CHECK_EQ(std::get<double>(value), 0.5);
}

NOX_TEST(LuaWorld, SchemaTypeErrorsAreRejectedWithoutWrites)
{
	Harness h;
	h.AddNode("hero", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'types', update = function(ctx, dt)\n"
		"  local hero = ctx:find('hero')\n"
		"  ctx:add(hero, 'Health')\n"
		"  local ok, err = pcall(ctx.set_field, ctx, hero, 'Health', 'current', 'lots')\n"
		"  assert(not ok and string.find(err, 'expected integer'))\n"
		"  ok, err = pcall(ctx.set_field, ctx, hero, 'Health', 'current', 1.5)\n"
		"  assert(not ok and string.find(err, 'expected integer'))\n"
		"  ok, err = pcall(ctx.set_field, ctx, hero, 'Health', 'bogus', 1)\n"
		"  assert(not ok and string.find(err, \"has no field 'bogus'\"))\n"
		"  ok, err = pcall(ctx.add, ctx, hero, 'Inventory', {gold = 'x'})\n"
		"  assert(not ok and string.find(err, 'expected integer'))\n"
		"  ok, err = pcall(ctx.add, ctx, hero, 'Quest', {state = 1})\n"
		"  assert(not ok and string.find(err, 'expected string'))\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.FaultedNames().empty());
	const EntityHandle hero = h.world.FindByName("hero");
	NoxLua::Value value;
	NOX_REQUIRE(h.world.GetDeclaredField(hero, "Health", "current", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(100));
	NOX_CHECK(!h.world.HasDeclared(hero, "Inventory"));
}

NOX_TEST(LuaWorld, StaleHandleAfterScriptDestroy)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"local token\n"
		"local saved\n"
		"nox.system{name = 'life', update = function(ctx, dt)\n"
		"  local f = ctx:frame()\n"
		"  if f == 1 then token = ctx:spawn('temp')\n"
		"  elseif f == 2 then\n"
		"    saved = ctx:resolve_spawn(token)\n"
		"    assert(saved ~= nil and ctx:is_valid(saved))\n"
		"    ctx:add(saved, 'Inventory', {gold = 3})\n"
		"    ctx:destroy(saved)\n"
		"  elseif f == 3 then\n"
		"    assert(not ctx:is_valid(saved))\n"
		"    assert(ctx:find('temp') == nil)\n"
		"    assert(ctx:resolve_spawn(token) == nil)\n"
		"    local ok, err = pcall(ctx.get_field, ctx, saved, 'Inventory', 'gold')\n"
		"    assert(not ok and string.find(err, 'stale or invalid entity handle'))\n"
		"    ctx:move(saved, 1, 2, 3)\n"   // queued; validated again when applied
		"  end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(1));
	h.Frame();
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(0));
	h.Frame();
	NOX_CHECK(h.world.FaultedNames().empty());
	NOX_CHECK(h.HasDiagnostic("target handle is stale"));
	h.world.EndPlay();
}

NOX_TEST(LuaWorld, QuerySnapshotIsStableAndSpawnsAreDeferred)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	h.AddNode("e2", glm::vec3(1.0f));
	h.AddNode("e3", glm::vec3(2.0f));
	nlohmann::json authored = {
		{ "entities", { { "e1", { { "Health", { { "current", 1 } } } } },
		                { "e2", { { "Health", { { "current", 2 } } } } },
		                { "e3", { { "Health", { { "current", 3 } } } } } } } };
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'mutate', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local list = ctx:query('Health')\n"
		"  assert(#list == 3)\n"
		"  for i, h in ipairs(list) do\n"
		"    ctx:spawn('n' .. i)\n"
		"    ctx:add(ctx:find('e' .. i), 'Quest')\n"   // immediate declared write; list is a snapshot
		"    assert(ctx:find('n' .. i) == nil)\n"       // spawn is deferred to the end of the callback
		"  end\n"
		"  assert(#list == 3)\n"
		"  assert(#ctx:query('Health') == 3)\n"
		"  assert(#ctx:query('Quest') == 3 + 0)\n"
		"end}\n",
		authored));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.FaultedNames().empty());
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(3));
	NOX_CHECK(h.world.Diagnostics().empty());
}

NOX_TEST(LuaWorld, FailingSystemDiscardsItsCommands)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'bad', order = 1, update = function(ctx, dt)\n"
		"  ctx:spawn('ghost')\n"
		"  ctx:teleport(ctx:find('e1'), 5, 0, 0)\n"
		"  error('boom')\n"
		"end}\n"
		"nox.system{name = 'good', order = 2, update = function(ctx, dt)\n"
		"  GOOD = (GOOD or 0) + 1\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(0));
	NOX_CHECK(h.world.PendingCommandCount() == 0);
	const EntityHandle e1 = h.world.FindByName("e1");
	const glm::vec3 p = h.scene.GetTransformSystem()->GetWorldPosition(e1.id);
	NOX_CHECK(p.x == 0.0f);
	NOX_CHECK(!h.world.ConsumeTeleport());
	NOX_CHECK(h.HasDiagnostic("boom"));
	const auto faults = h.world.FaultedNames();
	NOX_REQUIRE(faults.size() == 1);
	NOX_CHECK_EQ(faults[0], std::string("system:bad"));

	// The faulted system is skipped; the healthy one keeps running.
	h.Frame();
	NOX_CHECK_EQ(h.GlobalInt("GOOD"), 2);
	h.world.RetryFaulted();
	NOX_CHECK(h.world.FaultedNames().empty());
}

NOX_TEST(LuaWorld, SystemOrderIsDeclaredNotIterated)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"LOG = ''\n"
		"nox.system{name = 's5a', order = 5, update = function(ctx) LOG = LOG .. 's5a ' end}\n"
		"nox.system{name = 's1', order = 1, update = function(ctx) LOG = LOG .. 's1 ' end}\n"
		"nox.system{name = 's5b', order = 5, update = function(ctx) LOG = LOG .. 's5b ' end}\n"
		"nox.system{name = 's3', order = 3, update = function(ctx) LOG = LOG .. 's3 ' end}\n"));
	const auto names = h.world.SystemNames();
	NOX_REQUIRE(names.size() == 4);
	NOX_CHECK_EQ(names[0], std::string("s1"));
	NOX_CHECK_EQ(names[1], std::string("s3"));
	NOX_CHECK_EQ(names[2], std::string("s5a"));
	NOX_CHECK_EQ(names[3], std::string("s5b"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.GlobalStr("LOG"), std::string("s1 s3 s5a s5b "));
}

NOX_TEST(LuaWorld, EventsAreDeliveredInEmissionAndRegistrationOrder)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"LOG = ''\n"
		"nox.on('hit', function(ctx, p) LOG = LOG .. 'h1:' .. p .. ' ' end)\n"
		"nox.on('hit', function(ctx, p) LOG = LOG .. 'h2:' .. p .. ' ' end)\n"
		"nox.system{name = 'emit', update = function(ctx)\n"
		"  if ctx:frame() == 1 then ctx:emit('hit', 'a') ctx:emit('hit', 'b') end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.GlobalStr("LOG"), std::string("h1:a h2:a h1:b h2:b "));
	h.Frame();
	NOX_CHECK_EQ(h.GlobalStr("LOG"), std::string("h1:a h2:a h1:b h2:b "));
}

NOX_TEST(LuaWorld, TeleportIsFlaggedMoveIsNot)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'mover', update = function(ctx, dt)\n"
		"  local e = ctx:find('e1')\n"
		"  if ctx:frame() == 1 then ctx:move(e, 2, 0, 0) end\n"
		"  if ctx:frame() == 2 then ctx:teleport(e, 9, 0, 0) end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(!h.world.ConsumeTeleport());
	const EntityHandle e1 = h.world.FindByName("e1");
	NOX_CHECK(h.scene.GetTransformSystem()->GetWorldPosition(e1.id).x == 2.0f);
	h.Frame();
	NOX_CHECK(h.world.ConsumeTeleport());
	NOX_CHECK(!h.world.ConsumeTeleport());
	NOX_CHECK(h.scene.GetTransformSystem()->GetWorldPosition(e1.id).x == 9.0f);
}

NOX_TEST(LuaWorld, PlayStopRestoresAuthoredStateAndSpawns)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	nlohmann::json authored = { { "entities", { { "e1", { { "Inventory", { { "gold", 5 } } } } } } } };
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'play', update = function(ctx, dt)\n"
		"  local e = ctx:find('e1')\n"
		"  if ctx:frame() == 1 then\n"
		"    ctx:teleport(e, 7, 0, 0)\n"
		"    ctx:spawn('temp')\n"
		"    ctx:set_field(e, 'Inventory', 'gold', 99)\n"
		"  end\n"
		"end}\n",
		authored));
	const EntityHandle e1 = h.world.FindByName("e1");
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(1));
	NOX_CHECK(h.scene.GetTransformSystem()->GetWorldPosition(e1.id).x == 7.0f);
	NoxLua::Value gold;
	NOX_REQUIRE(h.world.GetDeclaredField(e1, "Inventory", "gold", gold));
	NOX_CHECK_EQ(std::get<int64_t>(gold), int64_t(99));

	h.world.EndPlay();
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(0));
	NOX_CHECK(h.scene.GetTransformSystem()->GetWorldPosition(e1.id).x == 0.0f);
	NOX_REQUIRE(h.world.GetDeclaredField(e1, "Inventory", "gold", gold));
	NOX_CHECK_EQ(std::get<int64_t>(gold), int64_t(5));
	NOX_CHECK(h.world.FindByName("temp").id == INVALID_ENTITY);
}

NOX_TEST(LuaWorld, AuthoredDataAndMissingComponents)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	h.AddNode("e2", glm::vec3(0.0f));
	nlohmann::json authored = { { "entities", { { "e1", { { "Health", { { "current", 25 } } } } }, { "missing", { { "Health", {} } } } } } };
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'probe', update = function(ctx)\n"
		"  local e2 = ctx:find('e2')\n"
		"  assert(ctx:get(e2, 'Health') == nil)\n"
		"  assert(ctx:get_field(e2, 'Health', 'current') == nil)\n"
		"  assert(ctx:has(e2, 'Health') == false)\n"
		"  assert(ctx:has_component(e2, 'Transform') == true)\n"
		"  assert(ctx:has_component(e2, 'Renderable') == false)\n"
		"  local ok, err = pcall(ctx.set_field, ctx, e2, 'Health', 'current', 1)\n"
		"  assert(not ok and string.find(err, \"has no 'Health' component\"))\n"
		"  ok, err = pcall(ctx.has_component, ctx, e2, 'Nope')\n"
		"  assert(not ok and string.find(err, 'unknown native component'))\n"
		"  assert(ctx:get_field(ctx:find('e1'), 'Health', 'current') == 25)\n"
		"end}\n",
		authored));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.FaultedNames().empty());
	NOX_CHECK(h.HasDiagnostic("no scene node named 'missing'"));
}

NOX_TEST(LuaWorld, NonStringErrorFaultsSystem)
{
	Harness h;
	NOX_REQUIRE(h.Load("nox.system{name = 'weird', update = function(ctx) error({code = 1}) end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.HasDiagnostic("error object is a table value"));
	NOX_CHECK_EQ(h.world.FaultedNames().size(), size_t(1));
}

NOX_TEST(LuaWorld, InfiniteLoopInSystemIsBudgetedAndFaulted)
{
	Harness h;
	NOX_REQUIRE(h.Load("nox.system{name = 'spin', update = function(ctx) while true do end end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.HasDiagnostic("instruction budget exceeded"));
	NOX_CHECK_EQ(h.world.FaultedNames().size(), size_t(1));
	h.Frame();   // faulted: not run again
	NOX_CHECK_EQ(h.world.Diagnostics().size(), size_t(1));
}

NOX_TEST(LuaWorld, LoadErrorIsDiagnosedWithFileAndLine)
{
	Harness h;
	NOX_CHECK(!h.Load("local x = 1\nerror('bad entry')\n"));
	NOX_CHECK(!h.world.IsLoaded());
	NOX_REQUIRE(!h.world.Diagnostics().empty());
	const auto& d = h.world.Diagnostics().front();
	NOX_CHECK_EQ(d.source, std::string("load"));
	NOX_CHECK_EQ(d.file, std::string(kEntry));
	NOX_CHECK_EQ(d.line, 5);   // the prelude adds three lines
	NOX_CHECK(d.traceback.find("stack traceback") != std::string::npos);
}

NOX_TEST(LuaWorld, BehaviorsRunPerAttachedEntity)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	h.AddNode("e2", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load(
		"nox.behavior{name = 'spin', update = function(ctx, e, dt) SPIN = (SPIN or 0) + 1 end}\n"
		"nox.system{name = 'attach', update = function(ctx)\n"
		"  if ctx:frame() == 1 then ctx:attach(ctx:find('e1'), 'spin') end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK_EQ(h.GlobalInt("SPIN"), 1);
	h.Frame();
	NOX_CHECK_EQ(h.GlobalInt("SPIN"), 2);
}

// Physics substeps: the Lua fixed systems run exactly at PhysicsEngine step boundaries.
NOX_TEST(LuaWorld, FixedSystemsRunAtPhysicsSubsteps)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'phys', fixed = true, update = function(ctx, dt)\n"
		"  FIXED = (FIXED or 0) + 1\n"
		"  LAST_INDEX = ctx:substep()\n"
		"  assert(dt > 0)\n"
		"end}\n"
		"nox.system{name = 'frame', update = function(ctx) FRAME = (FRAME or 0) + 1 end}\n"));
	SimulationConfig config;
	config.fixedDeltaTime = 1.0f / 60.0f;
	config.maxSubSteps = 8;
	PhysicsEngine physics(config);
	std::vector<int> indices;
	physics.SetSubstepCallback([&h, &indices](float dt, int index) {
		indices.push_back(index);
		h.world.FixedUpdate(dt, index);
	});

	h.world.BeginPlay();
	// Zero substeps: no callbacks and no fixed-system work.
	physics.Update(0.001f);
	NOX_CHECK(indices.empty());
	NOX_CHECK_EQ(h.world.FixedStepCount(), uint64_t(0));
	// Frame updates never run fixed systems.
	h.Frame();
	NOX_CHECK_EQ(h.GlobalInt("FRAME"), 1);
	NOX_CHECK_EQ(h.GlobalInt("FIXED"), -1);

	// Multiple substeps: callbacks carry contiguous indices from 0.
	physics.Update(0.05f);
	const size_t steps = indices.size();
	NOX_CHECK(steps >= 2 && steps <= 3);
	for (size_t i = 0; i < steps; ++i) NOX_CHECK_EQ(indices[i], int(i));
	NOX_CHECK_EQ(h.GlobalInt("FIXED"), static_cast<long long>(steps));
	NOX_CHECK_EQ(h.GlobalInt("LAST_INDEX"), static_cast<long long>(steps - 1));

	// SingleStep is one substep with index 0.
	indices.clear();
	physics.SingleStep();
	NOX_REQUIRE(indices.size() == 1);
	NOX_CHECK_EQ(indices[0], 0);
	NOX_CHECK_EQ(h.GlobalInt("FIXED"), static_cast<long long>(steps) + 1);

	// After EndPlay the fixed systems no longer run from the same callback.
	h.world.EndPlay();
	indices.clear();
	h.world.FixedUpdate(0.016f, 0);
	NOX_CHECK_EQ(h.GlobalInt("FIXED"), static_cast<long long>(steps) + 1);
}

NOX_TEST(LuaWorld, StandaloneWorldCreatesNoNativeCommandsWithoutPlay)
{
	Harness h;
	h.AddNode("e1", glm::vec3(0.0f));
	NOX_REQUIRE(h.Load("nox.system{name = 'x', update = function(ctx) ctx:spawn('a') end}\n"));
	h.Frame();   // not playing: nothing runs
	NOX_CHECK_EQ(h.world.SpawnedCount(), size_t(0));
	NOX_CHECK(h.world.Diagnostics().empty());
}
