// Transactional reload (prompt section 4): declared-state preservation, failure retention, migrations,
// candidate budgets/OOM, staging side effects, edits during staging, watcher, leak checks. Memory files only.
#include "TestFramework.h"

#include "EditorExtensions.h"
#include "GameModule.h"
#include "SceneGraph.h"
#include "TransformSystem.h"
#include "scripting/LuaGameModule.h"
#include "scripting/LuaReload.h"
#include "scripting/LuaEditorPanel.h"

#include "json.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace {
	const char* kMain = "projects/t/scripts/main.lua";

	class MemoryFiles : public NoxLua::ScriptFiles {
	public:
		struct File {
			std::string text;
			int64_t version = 1;
		};
		std::map<std::string, File> files;
		// Runs after a successful read (used to change files while a reload stages).
		std::function<void(MemoryFiles&, const std::string&)> onRead;

		void Set(const std::string& path, const std::string& text) {
			File& f = files[path];
			f.text = text;
			++f.version;
		}

		bool Read(const std::string& path, std::string& text, NoxLua::FileStamp& stamp, std::string& error) override {
			const auto it = files.find(path);
			if (it == files.end()) {
				error = "no such file: " + path;
				return false;
			}
			text = it->second.text;
			stamp.size = text.size();
			stamp.mtime = it->second.version;
			if (onRead) onRead(*this, path);
			return true;
		}

		bool Stat(const std::string& path, NoxLua::FileStamp& stamp) override {
			const auto it = files.find(path);
			if (it == files.end()) return false;
			stamp.size = it->second.text.size();
			stamp.mtime = it->second.version;
			return true;
		}
	};

	// A project module over memory files and a scene with a "Player" node at the origin.
	struct Fixture {
		MemoryFiles* files = nullptr;
		SceneGraph scene;
		std::unique_ptr<NoxLua::LuaGameModule> module;
		int baselineVms = 0;

		Fixture(const std::string& body, const nlohmann::json& authored = nlohmann::json::object()) {
			baselineVms = NoxLua::Runtime::LiveCount();
			auto memory = std::make_unique<MemoryFiles>();
			files = memory.get();
			files->files[kMain] = MemoryFiles::File{ body, 1 };
			NoxLua::ProjectDescriptor descriptor;
			descriptor.descriptorPath = "projects/t/project.json";
			descriptor.content.name = "t";
			descriptor.content.entry = kMain;
			descriptor.content.scriptRoots = { "projects/t/scripts" };
			module = std::make_unique<NoxLua::LuaGameModule>(descriptor, std::move(memory));
			module->SetWatchEnabled(false);
			const EntityID id = scene.GetComponentManager()->CreateEntity("Player");
			scene.GetTransformSystem()->SetPosition(id, glm::vec3(0.0f));
			Configure(authored);
		}

		void Configure(const nlohmann::json& authored) {
			nlohmann::json root = { { "scripting", { { "entities", authored } } } };
			module->Configure(nlohmann::json::object(), root, scene);
		}

		void Edit(const std::string& body) { files->Set(kMain, body); }

		void Frames(int count) {
			for (int i = 0; i < count; ++i) module->Update(1.0f / 60.0f, true, 1.0f);
		}

		NoxLua::LuaWorld* World() { return module->MutableWorld(); }

		// Requests a reload and runs one frame boundary (the candidate is applied or rejected).
		void Attempt() {
			module->RequestReload();
			module->Update(1.0f / 60.0f, true, 1.0f);
		}

		// True when the attempt was applied.
		bool Reload() {
			const int before = module->ReloadCount();
			Attempt();
			return module->ReloadCount() == before + 1;
		}

		bool Rejected(const std::string& needle) const {
			for (const auto& d : module->ReloadLog()) {
				if (d.severity == NoxLua::Diagnostic::Severity::Error && d.message.find(needle) != std::string::npos) return true;
			}
			return false;
		}

		long long Counter(const char* schema, const char* field) {
			NoxLua::Value value;
			if (!World()->GetDeclaredField(World()->FindByName("Player"), schema, field, value)) return -1;
			if (std::holds_alternative<int64_t>(value)) return static_cast<long long>(std::get<int64_t>(value));
			return -2;
		}

		~Fixture() {
			module.reset();
		}
	};

	const char* kCounterV1 =
		"nox.schema('Counter', {n = 'integer'}, {n = 0})\n"
		"nox.system{name = 'tick', update = function(ctx)\n"
		"  local p = ctx:find('Player')\n"
		"  ctx:set_field(p, 'Counter', 'n', ctx:get_field(p, 'Counter', 'n') + 1)\n"
		"end}\n";

	nlohmann::json CounterAuthored(long long n) {
		return { { "Player", { { "Counter", { { "n", n } } } } } };
	}
}

NOX_TEST(LuaReload, SuccessfulReloadKeepsDeclaredStateAndRunsNewCode)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	NOX_REQUIRE(f.module->IsAvailable());
	f.World()->BeginPlay();
	f.Frames(10);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 10);

	f.Edit(std::string(kCounterV1).replace(std::string(kCounterV1).find("+ 1"), 3, "+ 5"));
	NOX_REQUIRE(f.Reload());
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 15);   // reload frame: state kept (10), new code ran once (+5)
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 20);
	f.Frames(2);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 30);
	NOX_CHECK(f.World()->Diagnostics().empty());
	NOX_CHECK(f.World()->FaultedNames().empty());
	f.module->EndPlay();
}

NOX_TEST(LuaReload, RepeatedReloadsMidUpdateLeakNothing)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	std::string v1 = kCounterV1;
	size_t memoryAfterFirst = 0;
	const int vmsWithOneWorld = NoxLua::Runtime::LiveCount();
	for (int i = 1; i <= 20; ++i) {
		f.Edit(v1 + "-- revision " + std::to_string(i) + "\n");
		f.Frames(2);
		NOX_REQUIRE(f.Reload());
		f.Frames(2);
		NOX_CHECK_EQ(NoxLua::Runtime::LiveCount(), vmsWithOneWorld);   // old VM closed on every swap
		if (i == 1) memoryAfterFirst = f.World()->GetRuntime().MemoryUsed();
	}
	const size_t memoryAfter = f.World()->GetRuntime().MemoryUsed();
	std::printf("    [reload] VM bytes after 1st reload %zu, after 20th %zu; live VMs %d\n",
		memoryAfterFirst, memoryAfter, NoxLua::Runtime::LiveCount());
	NOX_CHECK(memoryAfter <= memoryAfterFirst + 64 * 1024);
	NOX_CHECK(f.Counter("Counter", "n") >= 40);   // 2 frames before and after each reload, 20 times
	NOX_CHECK_EQ(f.module->ReloadCount(), 20);
	f.module->EndPlay();
	f.module.reset();
	NOX_CHECK_EQ(NoxLua::Runtime::LiveCount(), f.baselineVms);
}

NOX_TEST(LuaReload, FailedReloadKeepsCodeStateAndWorld)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(5);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 5);
	const NoxLua::LuaWorld* before = f.World();

	f.Edit("local = = broken\n");
	f.Attempt();   // processed at the frame boundary; the result is a rejection
	NOX_CHECK_EQ(f.module->RejectedReloadCount(), 1);
	NOX_CHECK(f.Rejected("reload rejected"));
	NOX_CHECK(f.World() == before);   // same world object: nothing was swapped
	NOX_CHECK(f.module->IsAvailable());
	f.Frames(3);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 9);   // old code and state keep running (5 + attempt frame + 3)
	NOX_CHECK_EQ(NoxLua::Runtime::LiveCount(), f.baselineVms + 1);   // candidate VM closed
	f.module->EndPlay();
}

NOX_TEST(LuaReload, CandidateErrorsAreRejectedWithReason)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(2);

	f.Edit(std::string(kCounterV1) + "error('top level failure in the new code')\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("top level failure"));

	// A schema redeclared with a different type and no migration: stored data would be wrong.
	f.Edit("nox.schema('Counter', {n = 'string'}, {n = 'x'})\nnox.system{name = 'tick', update = function(ctx) end}\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("has no nox.migrate"));

	// A reference to a missing module.
	f.Edit("require('does.not.exist')\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("not found"));
	NOX_CHECK_EQ(f.module->RejectedReloadCount(), 3);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 5);   // 2 + one old-code frame per rejected attempt
	f.module->EndPlay();
}

NOX_TEST(LuaReload, SchemaVersionChangeNeedsAMigration)
{
	const char* health = "nox.schema('Health', {current = 'integer'}, {current = 100})\n";
	const char* healthV2 =
		"nox.schema('Health', {current = 'integer', shield = 'integer'}, {current = 100, shield = 0}, 2)\n";
	const char* migrate =
		"nox.migrate('Health', function(fields, from)\n"
		"  assert(from == 1)\n"
		"  return { current = fields.current, shield = 5 }\n"
		"end)\n";
	Fixture f(std::string(health) + "nox.system{name = 'idle', update = function(ctx) end}\n",
		{ { "Player", { { "Health", { { "current", 7 } } } } } });
	f.World()->BeginPlay();
	f.Frames(1);

	// New version, no migration: rejected; the old record survives.
	f.Edit(std::string(healthV2) + "nox.system{name = 'idle', update = function(ctx) end}\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("has no nox.migrate"));
	NoxLua::Value value;
	NOX_REQUIRE(f.World()->GetDeclaredField(f.World()->FindByName("Player"), "Health", "current", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(7));

	// With a migration the record is converted and the new field gets its migrated value.
	f.Edit(std::string(healthV2) + std::string(migrate) + "nox.system{name = 'idle', update = function(ctx) end}\n");
	NOX_CHECK(f.Reload());
	NOX_REQUIRE(f.World()->GetDeclaredField(f.World()->FindByName("Player"), "Health", "shield", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(5));
	NOX_REQUIRE(f.World()->GetDeclaredField(f.World()->FindByName("Player"), "Health", "current", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(7));
	f.module->EndPlay();
}

NOX_TEST(LuaReload, MigrationErrorsAndBadResultsAreRejected)
{
	const char* health = "nox.schema('Health', {current = 'integer'}, {current = 100})\n"
		"nox.system{name = 'idle', update = function(ctx) end}\n";
	Fixture f(health, { { "Player", { { "Health", { { "current", 7 } } } } } });
	f.World()->BeginPlay();
	const char* v2 = "nox.schema('Health', {current = 'integer'}, {current = 100}, 2)\n";

	f.Edit(std::string(v2) + "nox.migrate('Health', function(fields) error('migration exploded') end)\n"
		"nox.system{name = 'idle', update = function(ctx) end}\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("migration exploded"));

	f.Edit(std::string(v2) + "nox.migrate('Health', function(fields) return 42 end)\n"
		"nox.system{name = 'idle', update = function(ctx) end}\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("must return a table"));

	f.Edit(std::string(v2) + "nox.migrate('Health', function(fields) return { current = 'lots' } end)\n"
		"nox.system{name = 'idle', update = function(ctx) end}\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("field 'current'"));

	NOX_CHECK_EQ(f.module->RejectedReloadCount(), 3);
	NoxLua::Value value;
	NOX_REQUIRE(f.World()->GetDeclaredField(f.World()->FindByName("Player"), "Health", "current", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(7));
	f.module->EndPlay();
}

NOX_TEST(LuaReload, CandidateInstructionBudgetIsEnforced)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(1);
	f.Edit(std::string(kCounterV1) + "while true do end\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("instruction budget"));
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 3);
	f.module->EndPlay();
}

NOX_TEST(LuaReload, CandidateMemoryLimitIsCleanOom)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	NoxLua::RuntimeLimits small;
	small.memoryBytes = 256 * 1024;
	f.module->SetRuntimeLimits(small);
	f.World()->BeginPlay();
	f.Frames(1);
	f.Edit(std::string(kCounterV1) + "local t = {} for i = 1, 200000 do t[i] = tostring(i) .. 'x' end\n");
	f.Attempt();
	NOX_CHECK(f.Rejected("out of memory"));
	NOX_CHECK(f.module->IsAvailable());
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 3);
	f.module->EndPlay();
}

NOX_TEST(LuaReload, StagingCannotReachHostEffects)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(1);
	// No ctx and no world operations exist while a project loads; print output is discarded and reported.
	f.Edit(std::string(kCounterV1) +
		"assert(ctx == nil and rawget(_G, 'ctx') == nil)\n"
		"assert(nox.spawn == nil and nox.move == nil and nox.destroy == nil)\n"
		"print('chatter during staging')\n");
	NOX_CHECK(f.Reload());
	NOX_CHECK(f.module->IsAvailable());
	NOX_CHECK_EQ(f.module->ReloadCount(), 1);
	bool noted = false;
	for (const auto& d : f.World()->Diagnostics()) {
		noted = noted || d.message.find("discarded") != std::string::npos;
	}
	NOX_CHECK(noted);
	f.module->EndPlay();
}

NOX_TEST(LuaReload, EditsDuringStagingAreRetriedConsistently)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(1);
	const std::string v2 = std::string(kCounterV1).replace(std::string(kCounterV1).find("+ 1"), 3, "+ 3");
	f.Edit(v2);
	// The first staged read is followed by another edit: the first candidate must be discarded.
	int reads = 0;
	f.files->onRead = [&reads, v2](MemoryFiles& files, const std::string& path) {
		if (path != kMain) return;
		if (++reads == 1) files.Set(kMain, v2 + "-- edited while staging\n");
	};
	NOX_CHECK(f.Reload());
	NOX_CHECK(f.module->IsAvailable());
	NOX_CHECK(reads >= 2);
	bool retried = false;
	for (const auto& d : f.module->ReloadLog()) {
		retried = retried || d.message.find("2 staging attempt") != std::string::npos;
	}
	NOX_CHECK(retried);
	f.files->onRead = nullptr;
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 7);   // +1 before, +3 in the reload frame, +3 after
	f.module->EndPlay();
}

NOX_TEST(LuaReload, PlayStateSurvivesReloadAndStopRestoresAuthoredWorld)
{
	const char* body =
		"nox.schema('Counter', {n = 'integer'}, {n = 0})\n"
		"nox.system{name = 'play', update = function(ctx)\n"
		"  local p = ctx:find('Player')\n"
		"  if ctx:frame() == 1 then\n"
		"    ctx:teleport(p, 4, 0, 0)\n"
		"    ctx:spawn('temp')\n"
		"  end\n"
		"  ctx:set_field(p, 'Counter', 'n', ctx:get_field(p, 'Counter', 'n') + 1)\n"
		"end}\n";
	Fixture f(body, CounterAuthored(0));
	const EntityHandle player = f.World()->FindByName("Player");
	f.World()->BeginPlay();
	f.Frames(3);
	NOX_CHECK_EQ(f.World()->SpawnedCount(), size_t(1));
	NOX_CHECK(f.scene.GetTransformSystem()->GetWorldPosition(player.id).x == 4.0f);

	// Reload while playing: spawned entity, moved transform and declared state carry over.
	std::string edited = body;
	edited.replace(edited.find("+ 1"), 3, "+ 2");
	f.Edit(edited);
	NOX_REQUIRE(f.Reload());
	NOX_CHECK_EQ(f.World()->SpawnedCount(), size_t(1));
	NOX_CHECK(f.World()->IsLoaded());
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 3 + 2 + 2);
	NOX_CHECK(f.World()->FindByName("temp").id != INVALID_ENTITY);

	// Stop: script entities removed, moved transform restored, authored declared state back.
	f.module->EndPlay();
	f.Configure(CounterAuthored(0));
	NOX_CHECK(f.World()->FindByName("temp").id == INVALID_ENTITY);
	NOX_CHECK(f.scene.GetTransformSystem()->GetWorldPosition(player.id).x == 0.0f);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 0);
}

NOX_TEST(LuaReload, ReloadKeepsPauseAndFrameCounter)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.World()->BeginPlay();
	f.Frames(4);
	f.module->SetPaused(true);
	NOX_REQUIRE(f.Reload());
	NOX_CHECK(f.module->IsPaused());
	const uint64_t frame = f.World()->Frame();
	NOX_CHECK_EQ(frame, uint64_t(4));
	f.Frames(10);
	NOX_CHECK_EQ(f.World()->Frame(), frame);   // paused: no frame progress across the reload
	f.module->SetPaused(false);
	f.Frames(1);
	NOX_CHECK_EQ(f.World()->Frame(), frame + 1);
	f.module->EndPlay();
}

NOX_TEST(LuaReload, StoppedReloadRebuildsFromAuthoredState)
{
	Fixture f(kCounterV1, CounterAuthored(9));
	NOX_REQUIRE(f.Reload());   // not playing: the reload runs at the next Update
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 9);
	NOX_CHECK(f.module->IsAvailable());
}

NOX_TEST(LuaReload, LooseWatcherReloadsAfterADebouncedChange)
{
	Fixture f(kCounterV1, CounterAuthored(0));
	f.module->SetWatchEnabled(true);
	f.World()->BeginPlay();
	f.Frames(1);
	f.Edit(std::string(kCounterV1).replace(std::string(kCounterV1).find("+ 1"), 3, "+ 7"));
	// Poll cadence 0.25 s, debounce 0.4 s: the change is applied within about a second of play.
	for (int i = 0; i < 120 && f.module->ReloadCount() == 0; ++i) f.Frames(1);
	NOX_CHECK_EQ(f.module->ReloadCount(), 1);
	const long long before = f.Counter("Counter", "n");
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), before + 7);
	f.module->EndPlay();
}

NOX_TEST(LuaReload, EditorPanelIsRegistered)
{
	NoxLua::RegisterLuaEditorPanel();
	NoxLua::RegisterLuaEditorPanel();   // idempotent
	int count = 0;
	for (const EditorPanel& panel : EditorExtensions::Panels()) {
		if (panel.id == "lua.project") ++count;
	}
	NOX_CHECK_EQ(count, 1);
}

// Regression: a project that registers nox.on_configure must still load, configure, play, reload and restore.
NOX_TEST(LuaReload, ConfigureHookSurvivesPlayReloadAndStop)
{
	const char* body =
		"nox.schema('Counter', {n = 'integer'}, {n = 0})\n"
		"nox.on_configure(function(game, scene) configured = (configured or 0) + 1 end)\n"
		"nox.system{name = 'play', update = function(ctx)\n"
		"  local p = ctx:find('Player')\n"
		"  ctx:set_field(p, 'Counter', 'n', ctx:get_field(p, 'Counter', 'n') + 1)\n"
		"end}\n";
	Fixture f(body, CounterAuthored(0));
	NOX_REQUIRE(f.World()->IsLoaded());
	f.World()->BeginPlay();
	f.Frames(2);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 2);
	std::string edited = body;
	edited.replace(edited.find("+ 1"), 3, "+ 2");
	f.Edit(edited);
	NOX_REQUIRE(f.Reload());
	NOX_CHECK(f.World()->IsLoaded());
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 2 + 2);
	f.module->EndPlay();
	f.Configure(CounterAuthored(0));
	NOX_CHECK(f.World()->IsLoaded());
	f.World()->BeginPlay();
	f.Frames(1);
	NOX_CHECK_EQ(f.Counter("Counter", "n"), 1);
}
