// Lua access to the reflection registry: ctx:component proxies over Transform and Light, validated writes,
// methods, add/remove, stale handles, nox.components() introspection, parent/children, raycast argument checks
// and load_scene validation. Headless: a SceneGraph with LightNode and plain nodes, no GPU.
#include "TestFramework.h"

#include "reflect/Reflection.h"

#include "LightNode.h"
#include "PointLight.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "TransformSystem.h"
#include "reflect/ReflectServices.h"
#include "reflect/Reflection.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <map>
#include <set>
#include <string>

using nox::reflect::ComponentDesc;
using nox::reflect::MethodDesc;
using nox::reflect::PropertyDesc;
using nox::reflect::PropType;
using nox::reflect::PropValue;
using nox::reflect::Registry;

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

	// Test-only descriptors. "LuaTestProbe" is attached to nodes through a set (add/remove); "LuaTestFixed" has
	// no add/remove, so the script-side changes are refused. Registered once; names are unique to these tests.
	std::set<const SceneNode*> g_tagged;
	int g_level = 0;
	std::string g_label;
	int g_mode = 0;

	void RegisterProbes() {
		static bool done = false;
		if (done) return;
		done = true;

		ComponentDesc probe;
		probe.name = "LuaTestProbe";
		probe.category = "Test";
		probe.tooltip = "Test component for the Lua proxy tests";
		probe.order = 900;
		probe.has = [](const SceneNode& n) { return g_tagged.count(&n) > 0; };
		probe.add = [](SceneNode& n) { g_tagged.insert(&n); return true; };
		probe.remove = [](SceneNode& n) { g_tagged.erase(&n); return true; };
		{
			PropertyDesc level;
			level.name = "level";
			level.type = PropType::Int;
			level.minValue = 0.0;
			level.maxValue = 10.0;
			level.get = [](const SceneNode&) { return PropValue(static_cast<int64_t>(g_level)); };
			level.set = [](SceneNode&, const PropValue& v) { g_level = static_cast<int>(std::get<int64_t>(v)); return true; };
			probe.properties.push_back(level);
		}
		{
			PropertyDesc label;
			label.name = "label";
			label.type = PropType::String;
			label.get = [](const SceneNode&) { return PropValue(g_label); };
			label.set = [](SceneNode&, const PropValue& v) { g_label = std::get<std::string>(v); return true; };
			probe.properties.push_back(label);
		}
		{
			PropertyDesc mode;
			mode.name = "mode";
			mode.type = PropType::Enum;
			mode.enumNames = { "idle", "run" };
			mode.get = [](const SceneNode&) { return PropValue(static_cast<int64_t>(g_mode)); };
			mode.set = [](SceneNode&, const PropValue& v) { g_mode = static_cast<int>(std::get<int64_t>(v)); return true; };
			probe.properties.push_back(mode);
		}
		{
			PropertyDesc fixed;
			fixed.name = "fixed_id";
			fixed.type = PropType::Int;
			fixed.get = [](const SceneNode&) { return PropValue(int64_t(7)); };
			probe.properties.push_back(fixed);   // no setter: read-only
		}
		{
			MethodDesc play;
			play.name = "play";
			play.args = { PropType::String };
			play.returnsValue = true;
			play.invoke = [](SceneNode&, const std::vector<PropValue>& args, PropValue& result) {
				result = PropValue(std::string("played:") + std::get<std::string>(args[0]));
				return true;
			};
			probe.methods.push_back(play);

			MethodDesc fail;
			fail.name = "fail";
			fail.invoke = [](SceneNode&, const std::vector<PropValue>&, PropValue&) { return false; };
			probe.methods.push_back(fail);
		}
		Registry::Get().Register(probe);

		ComponentDesc fixed;
		fixed.name = "LuaTestFixed";
		fixed.category = "Test";
		fixed.has = [](const SceneNode& n) { return g_tagged.count(&n) > 0; };
		Registry::Get().Register(fixed);   // no add/remove
	}

	const char* kPrelude =
		"function try(f) local ok, e = pcall(f) if ok then return '' end return tostring(e) end\n";

	struct Harness {
		MemorySource source;
		SceneGraph scene;
		NoxLua::LuaWorld world;
		std::shared_ptr<SceneNode> lamp;
		std::shared_ptr<SceneNode> probe;

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			RegisterProbes();
			g_tagged.clear();
			g_level = 0;
			g_label.clear();
			g_mode = 0;

			// A point light node under the root, with an entity (headless: no model, no GPU).
			lamp = std::make_shared<LightNode>(std::make_shared<PointLight>());
			lamp->SetName("lamp");
			scene.GetRoot()->AddChild(lamp);
			lamp->SetECSContext(scene.GetComponentManager(), scene.GetTransformSystem());
			lamp->CreateECSEntity("lamp");

			probe = std::make_shared<SceneNode>(scene.GetComponentManager(), scene.GetTransformSystem());
			probe->SetName("probe");
			scene.GetRoot()->AddChild(probe);
			probe->SetECSContext(scene.GetComponentManager(), scene.GetTransformSystem());
			probe->CreateECSEntity("probe");
			g_tagged.insert(probe.get());
		}

		~Harness() {
			g_tagged.clear();
			nox::reflect::Services().requestSceneSwap = nullptr;
		}

		bool Load(const std::string& body) {
			source.files[kEntry] = std::string(kPrelude) + body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}

		void Frame(float dt = 1.0f / 60.0f) { world.Update(dt, true); }

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

NOX_TEST(LuaComponents, ProxyReadsAndWritesTransformAndLight)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'proxy', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local lamp = ctx:find('lamp')\n"
		"  local t = ctx:component(lamp, 'Transform')\n"
		"  t.position = {x = 1, y = 2, z = 3}\n"
		"  local p = t.position\n"
		"  if p.x ~= 1 or p.y ~= 2 or p.z ~= 3 then error('position round trip') end\n"
		"  t.scale = {4, 5, 6}\n"          // array form is accepted too
		"  if t.scale.y ~= 5 then error('scale array input') end\n"
		"  local l = ctx:component(lamp, 'Light')\n"
		"  if l.kind ~= 'point' then error('kind enum name: ' .. tostring(l.kind)) end\n"
		"  l.intensity = 2.5\n"
		"  if l.intensity ~= 2.5 then error('intensity') end\n"
		"  l.color = {r = 0.5, g = 0.25, b = 1}\n"
		"  if l.color.g ~= 0.25 then error('colour') end\n"
		"  l.enabled = false\n"
		"  if l.enabled ~= false then error('enabled') end\n"
		"  l.range = -5\n"                  // clamped to the descriptor minimum
		"  if l.range <= 0 then error('range clamp') end\n"
		"  if ctx:has_component(lamp, 'Light') ~= true then error('has_component') end\n"
		"  result = 'ok'\n"
		"end}\n"));
	NOX_REQUIRE(h.world.IsLoaded());
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK_EQ(h.GlobalStr("result"), std::string("ok"));

	// The writes reached the native subsystems, not just a Lua copy.
	NOX_CHECK(h.lamp->GetPosition() == glm::vec3(1.0f, 2.0f, 3.0f));
	auto light = std::dynamic_pointer_cast<LightNode>(h.lamp);
	NOX_REQUIRE(light != nullptr);
	NOX_CHECK(light->GetLight()->GetIntensity() == 2.5f);
	NOX_CHECK(!light->GetLight()->IsEnabled());
}

NOX_TEST(LuaComponents, MethodsAndProxyErrorsUseTheExistingStyle)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'probe', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local p = ctx:component(ctx:find('probe'), 'LuaTestProbe')\n"
		"  p.level = 7\n"
		"  p.label = 'hi'\n"
		"  p.mode = 'run'\n"
		"  if p.mode ~= 'run' then error('enum name round trip') end\n"
		"  if p:play('Walk') ~= 'played:Walk' then error('method result') end\n"
		"  errors = table.concat({\n"
		"    try(function() p.fixed_id = 3 end),\n"
		"    try(function() p.level = 'x' end),\n"
		"    try(function() p.level = 99 end),\n"
		"    try(function() return p.nope end),\n"
		"    try(function() p.mode = 'sprint' end),\n"
		"    try(function() p:play() end),\n"
		"    try(function() p:play(3) end),\n"
		"    try(function() p:fail() end),\n"
		"    try(function() p.unknown = 1 end),\n"
		"  }, '|')\n"
		"  if p.level ~= 10 then error('level clamps to the range') end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	const std::string errors = h.GlobalStr("errors");
	NOX_CHECK(errors.find("LuaTestProbe.fixed_id is read-only") != std::string::npos);
	NOX_CHECK(errors.find("LuaTestProbe.level: expected integer") != std::string::npos);
	NOX_CHECK(errors.find("LuaTestProbe has no property or method 'nope'") != std::string::npos);
	NOX_CHECK(errors.find("unknown value 'sprint' (expected one of: idle, run)") != std::string::npos);
	NOX_CHECK(errors.find("LuaTestProbe:play expects 1 argument(s)") != std::string::npos);
	NOX_CHECK(errors.find("bad argument #1 (expected string)") != std::string::npos);
	NOX_CHECK(errors.find("LuaTestProbe:fail failed") != std::string::npos);
	NOX_CHECK(errors.find("LuaTestProbe has no property 'unknown'") != std::string::npos);
	NOX_CHECK_EQ(g_level, 10);   // 99 was clamped to the descriptor maximum
}

NOX_TEST(LuaComponents, StaleHandleAndRemovedComponentRaise)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'keep', update = function(ctx, dt)\n"
		"  if ctx:frame() == 1 then\n"
		"    probe_handle = ctx:find('probe')\n"
		"    kept = ctx:component(probe_handle, 'LuaTestProbe')\n"
		"    removed = ctx:remove_component(probe_handle, 'LuaTestProbe')\n"
		"    if removed ~= true then error('remove') end\n"
		"    removed_read = try(function() return kept.level end)\n"
		"    removed_has = ctx:has_component(probe_handle, 'LuaTestProbe')\n"
		"    gone = ctx:component(probe_handle, 'LuaTestProbe')\n"
		"    if gone ~= nil then error('component after removal must be nil') end\n"
		"    readd = ctx:add_component(probe_handle, 'LuaTestProbe')\n"
		"    if readd ~= true then error('re-add') end\n"
		"    no_remove, reason = ctx:remove_component(ctx:find('probe'), 'Node')\n"
		"    no_remove_reason = tostring(reason)\n"
		"  end\n"
		"end}\n"
		"nox.system{name = 'stale', update = function(ctx, dt)\n"
		"  if ctx:frame() == 2 then\n"
		"    stale_read = try(function() return kept.level end)\n"
		"    stale_component = try(function() return ctx:component(probe_handle, 'LuaTestProbe') end)\n"
		"  end\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK(h.GlobalStr("removed_read").find("LuaTestProbe' is not present") != std::string::npos);
	NOX_CHECK_EQ(h.GlobalStr("no_remove_reason"), std::string("component 'Node' cannot be removed from this node"));

	// The entity itself is destroyed natively: the checked handle is stale for every proxy and call.
	h.scene.GetComponentManager()->DestroyEntity(h.probe->GetEntityID());
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK(h.GlobalStr("stale_read").find("stale or invalid entity handle") != std::string::npos);
	NOX_CHECK(h.GlobalStr("stale_component").find("stale or invalid entity handle") != std::string::npos);
}

NOX_TEST(LuaComponents, ComponentsListsEveryRegisteredDescriptor)
{
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'list', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local names, light, intensity = {}, nil, nil\n"
		"  for _, c in ipairs(nox.components()) do\n"
		"    names[c.name] = true\n"
		"    if c.name == 'Light' then light = c end\n"
		"  end\n"
		"  if not (names.Node and names.Transform and names.Light) then error('core names missing') end\n"
		"  if light.category ~= 'Rendering' then error('category') end\n"
		"  for _, p in ipairs(light.properties) do\n"
		"    if p.name == 'intensity' then intensity = p end\n"
		"    if p.name == 'kind' and not p.read_only then error('kind is read-only') end\n"
		"    if p.name == 'kind' and p.enum[2] ~= 'point' then error('enum names') end\n"
		"  end\n"
		"  if intensity.type ~= 'float' or intensity.min ~= 0 then error('intensity range') end\n"
		"  local lamp_components = {}\n"
		"  for _, n in ipairs(ctx:components(ctx:find('lamp'))) do lamp_components[n] = true end\n"
		"  if not (lamp_components.Node and lamp_components.Transform and lamp_components.Light) then error('ctx:components') end\n"
		"  if lamp_components.LuaTestProbe then error('probe is not on the lamp') end\n"
		"  if ctx:has_component(ctx:find('lamp'), 'Renderable') ~= false then error('native presence') end\n"
		"  if try(function() ctx:has_component(ctx:find('lamp'), 'Nope') end) == '' then error('unknown name must raise') end\n"
		"  result = 'ok'\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK_EQ(h.GlobalStr("result"), std::string("ok"));
}

NOX_TEST(LuaComponents, ParentChildrenRaycastAndSceneRequests)
{
	Harness h;
	std::string requested;
	nox::reflect::Services().requestSceneSwap = [&requested](const std::string& path) { requested = path; };
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'graph', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local lamp = ctx:find('lamp')\n"
		"  local parent = ctx:parent(lamp)\n"
		"  if parent == nil then error('lamp has a parent (root)') end\n"
		"  local found = false\n"
		"  for _, c in ipairs(ctx:children(parent)) do if ctx:same(c, lamp) then found = true end end\n"
		"  if not found then error('lamp is a child of its parent') end\n"
		"  if ctx:parent(parent) ~= nil then error('root has no parent') end\n"
		"  if ctx:raycast({0, 0, 0}, {0, 0, -1}, 10) ~= nil then error('no geometry in the headless scene') end\n"
		"  errors = table.concat({\n"
		"    try(function() ctx:raycast({0, 0, 0}, {0, 0, 0}, 10) end),\n"
		"    try(function() ctx:raycast({0, 0, 0}, {0, 0, -1}, 0) end),\n"
		"    try(function() ctx:load_scene('scenes/../secret.json') end),\n"
		"    try(function() ctx:load_scene('scenes/next.txt') end),\n"
		"  }, '|')\n"
		"  ctx:load_scene('scenes/next_level.json')\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	const std::string errors = h.GlobalStr("errors");
	NOX_CHECK(errors.find("direction must not be zero") != std::string::npos);
	NOX_CHECK(errors.find("max_dist must be in") != std::string::npos);
	NOX_CHECK(errors.find("parent or repeated separators") != std::string::npos);
	NOX_CHECK(errors.find("expected a logical path scenes/<name>.json") != std::string::npos);
	NOX_CHECK_EQ(requested, std::string("scenes/next_level.json"));
}

NOX_TEST(LuaComponents, BareEntitiesHaveNoComponents)
{
	Harness h;
	const EntityID bare = h.scene.GetComponentManager()->CreateEntity("bare");
	(void)bare;
	NOX_REQUIRE(h.Load(
		"nox.system{name = 'bare', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local b = ctx:find('bare')\n"
		"  if ctx:component(b, 'Transform') ~= nil then error('bare entity has no scene node') end\n"
		"  local names = ctx:components(b)\n"
		"  if #names ~= 1 or names[1] ~= 'Transform' then error('bare entity lists only its native pool') end\n"
		"  if ctx:has_component(b, 'Transform') ~= true then error('native Transform pool') end\n"
		"  if ctx:parent(b) ~= nil or #ctx:children(b) ~= 0 then error('no hierarchy') end\n"
		"  result = 'ok'\n"
		"end}\n"));
	h.world.BeginPlay();
	h.Frame();
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK_EQ(h.GlobalStr("result"), std::string("ok"));
}
