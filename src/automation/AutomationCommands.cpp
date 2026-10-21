// Engine commands of the automation server. Every handler runs on the main thread (Dispatcher::Run from
// BeginFrame), so it may use Core and the scene freely. Reference: docs/automation.md.

#include "AutomationServer.h"

#include "Core.h"
#include "SceneFrameClock.h"
#include "ImGuiInterface.h"
#include "ModularRenderer.h"
#include "RenderContext.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "SceneRenderSettings.h"
#include "Camera.h"
#include "ImGui/ConsoleLog.h"
#include "assets/AssetIO.h"
#include "reflect/Reflection.h"
#include "render/RenderSettingsTable.h"
#include "scripting/LuaGameModule.h"
#include "scripting/LuaRuntime.h"
#include "scripting/LuaWorld.h"

#include <SDL/SDL.h>
#include <lua.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace automation {
namespace {

using nox::reflect::ComponentDesc;
using nox::reflect::PropertyDesc;
using nox::reflect::PropType;
using nox::reflect::PropValue;
using nox::reflect::Registry;
using nox::render::RenderSetting;

// ---- Parameter helpers ---------------------------------------------------------------------------

[[noreturn]] void Invalid(const std::string& message)
{
	throw RpcError{ rpc::kInvalidParams, message, nullptr };
}

[[noreturn]] void EngineError(const std::string& message)
{
	throw RpcError{ rpc::kEngineError, message, nullptr };
}

Core& RequireCore(Invocation& inv)
{
	if (!inv.core) EngineError("engine core is not available");
	return *inv.core;
}

bool Has(const json& params, const char* key)
{
	return params.contains(key) && !params[key].is_null();
}

int64_t ParamInt(const json& params, const char* key, int64_t fallback, int64_t lo, int64_t hi)
{
	if (!Has(params, key)) return fallback;
	const json& v = params[key];
	if (!v.is_number_integer()) Invalid(std::string("'") + key + "' must be an integer");
	const int64_t value = v.get<int64_t>();
	if (value < lo || value > hi) {
		Invalid(std::string("'") + key + "' must be between " + std::to_string(lo) + " and " + std::to_string(hi));
	}
	return value;
}

int64_t RequireInt(const json& params, const char* key)
{
	if (!Has(params, key)) Invalid(std::string("missing integer parameter '") + key + "'");
	return ParamInt(params, key, 0, INT64_MIN, INT64_MAX);
}

std::string RequireString(const json& params, const char* key)
{
	if (!Has(params, key) || !params[key].is_string()) Invalid(std::string("missing string parameter '") + key + "'");
	return params[key].get<std::string>();
}

std::string OptString(const json& params, const char* key, const std::string& fallback)
{
	if (!Has(params, key)) return fallback;
	if (!params[key].is_string()) Invalid(std::string("'") + key + "' must be a string");
	return params[key].get<std::string>();
}

bool OptBool(const json& params, const char* key, bool fallback)
{
	if (!Has(params, key)) return fallback;
	if (!params[key].is_boolean()) Invalid(std::string("'") + key + "' must be a boolean");
	return params[key].get<bool>();
}

double OptNumber(const json& params, const char* key, double fallback)
{
	if (!Has(params, key)) return fallback;
	if (!params[key].is_number()) Invalid(std::string("'") + key + "' must be a number");
	return params[key].get<double>();
}

glm::vec3 Vec3Param(const json& params, const char* key)
{
	if (!Has(params, key) || !params[key].is_array() || params[key].size() != 3) {
		Invalid(std::string("'") + key + "' must be an array of 3 numbers");
	}
	glm::vec3 v(0.0f);
	for (int i = 0; i < 3; ++i) {
		if (!params[key][i].is_number()) Invalid(std::string("'") + key + "' must be an array of 3 numbers");
		v[i] = static_cast<float>(params[key][i].get<double>());
	}
	return v;
}

json Vec3Json(const glm::vec3& v)
{
	return json::array({ v.x, v.y, v.z });
}

// ---- Node identity -------------------------------------------------------------------------------
// Node ids are session-scoped integers handed out on first sight. They stay valid while the node lives.

struct NodeIds {
	std::unordered_map<const SceneNode*, int64_t> byPointer;
	std::map<int64_t, std::weak_ptr<SceneNode>> byId;
	int64_t next = 1;
};

NodeIds& Ids()
{
	static NodeIds ids;
	return ids;
}

int64_t IdOf(const std::shared_ptr<SceneNode>& node)
{
	NodeIds& ids = Ids();
	const auto it = ids.byPointer.find(node.get());
	if (it != ids.byPointer.end()) {
		const auto live = ids.byId.find(it->second);
		if (live != ids.byId.end() && live->second.lock() == node) return it->second;
	}
	const int64_t id = ids.next++;
	ids.byPointer[node.get()] = id;
	ids.byId[id] = node;
	return id;
}

std::shared_ptr<SceneNode> NodeFor(int64_t id)
{
	const auto it = Ids().byId.find(id);
	if (it == Ids().byId.end()) return nullptr;
	return it->second.lock();
}

// The node named by params[key]; null when the key is absent or null. Unknown ids are an error.
std::shared_ptr<SceneNode> OptNode(const json& params, const char* key)
{
	if (!Has(params, key)) return nullptr;
	const int64_t id = RequireInt(params, key);
	std::shared_ptr<SceneNode> node = NodeFor(id);
	if (!node) Invalid("no node with id " + std::to_string(id) + " (it may have been deleted)");
	return node;
}

std::shared_ptr<SceneNode> RequireNode(const json& params, const char* key = "id")
{
	if (!Has(params, key)) Invalid(std::string("missing parameter '") + key + "'");
	return OptNode(params, key);
}

json LoadStateData(Core& core)
{
	return json{ { "loading", core.IsSceneLoading() || core.IsSceneSwapPending() },
		{ "load_progress", core.GetSceneLoadProgress() }, { "load_step", core.GetSceneLoadStep() } };
}

std::shared_ptr<SceneGraph> RequireGraph(Core& core)
{
	// While a load runs the live scene may be the one being replaced: say so instead of answering from it.
	if (core.IsSceneLoading() || core.IsSceneSwapPending()) {
		throw RpcError{ rpc::kSceneLoading, "scene is loading (see engine.info load_progress / load_step)", LoadStateData(core) };
	}
	std::shared_ptr<SceneGraph> graph = core.GetSceneGraph();
	if (!graph) EngineError("no scene is loaded");
	return graph;
}

std::shared_ptr<SceneNode> RootOf(Core& core)
{
	std::shared_ptr<SceneGraph> graph = RequireGraph(core);
	std::shared_ptr<SceneNode> root = graph->GetRoot();
	if (!root) EngineError("the scene has no root node");
	return root;
}

const char* TypeName(SceneNode::NODE_TYPE type)
{
	switch (type) {
	case SceneNode::NODE: return "node";
	case SceneNode::AUDIO: return "audio";
	case SceneNode::MODEL: return "model";
	case SceneNode::LIGHT: return "light";
	case SceneNode::CAMERA: return "camera";
	case SceneNode::GUI: return "gui";
	case SceneNode::LPV_VOLUME: return "lpv_volume";
	case SceneNode::SKELETAL: return "skeletal";
	case SceneNode::FOLDER: return "folder";
	}
	return "unknown";
}

json ComponentNames(const SceneNode& node)
{
	json names = json::array();
	for (const ComponentDesc* desc : Registry::Get().ComponentsOf(node)) names.push_back(desc->name);
	return names;
}

json NodeBrief(const std::shared_ptr<SceneNode>& node)
{
	json j = json::object();
	j["id"] = IdOf(node);
	j["name"] = node->GetName();
	j["type"] = TypeName(node->GetNodeType());
	j["folder"] = node->IsFolder();
	j["components"] = ComponentNames(*node);
	j["child_count"] = node->children.size();
	return j;
}

json NodeTree(const std::shared_ptr<SceneNode>& node, int64_t depth)
{
	json j = NodeBrief(node);
	json children = json::array();
	if (depth != 0) {
		for (const auto& child : node->children) {
			if (child) children.push_back(NodeTree(child, depth - 1));
		}
	}
	j["children"] = std::move(children);
	return j;
}

void Walk(const std::shared_ptr<SceneNode>& node, const std::function<void(const std::shared_ptr<SceneNode>&)>& fn)
{
	fn(node);
	for (const auto& child : node->children) {
		if (child) Walk(child, fn);
	}
}

// ---- Render settings -----------------------------------------------------------------------------
// The RenderSettingTable keys, plus a few RenderContext switches the table does not carry (surfel GI, debug
// views). quality_tier is handled separately (it applies a tier from the scene file).

RenderSetting ContextInt(const char* key, const char* group, const char* tooltip, int RenderContext::*member,
	double lo, double hi)
{
	RenderSetting s;
	s.key = key;
	s.label = std::string(group) + " / " + key;
	s.group = group;
	s.tooltip = tooltip;
	s.type = PropType::Int;
	s.minValue = lo;
	s.maxValue = hi;
	s.get = [member](const RenderContext& c) { return PropValue(static_cast<int64_t>(c.*member)); };
	s.set = [member](RenderContext& c, const PropValue& v) {
		c.*member = static_cast<int>(std::get<int64_t>(v));
		return true;
	};
	return s;
}

RenderSetting ContextBool(const char* key, const char* group, const char* tooltip, bool RenderContext::*member)
{
	RenderSetting s;
	s.key = key;
	s.label = std::string(group) + " / " + key;
	s.group = group;
	s.tooltip = tooltip;
	s.type = PropType::Bool;
	s.get = [member](const RenderContext& c) { return PropValue(static_cast<bool>(c.*member)); };
	s.set = [member](RenderContext& c, const PropValue& v) {
		c.*member = std::get<bool>(v);
		return true;
	};
	return s;
}

const std::vector<RenderSetting>& ExtraRenderSettings()
{
	static const std::vector<RenderSetting> extras = [] {
		std::vector<RenderSetting> list;
		list.push_back(ContextBool("surfel_gi_enabled", "Surfel GI", "Surfel global illumination on/off.", &RenderContext::enableSurfelGI));
		list.push_back(ContextInt("surfel_gi_quality_tier", "Surfel GI", "0 low, 1 medium, 2 high, 3 ultra.",
			&RenderContext::surfelGIQualityTier, 0, 3));
		list.push_back(ContextInt("surfel_gi_debug_view", "Surfel GI",
			"Surfel debug view (SurfelGIDebugView: 0 off, 1 spheres, 2 normals, 3 age, 4 variance, 5 coverage, 6 grid cells, 7 occupancy ...).",
			&RenderContext::surfelGIDebugView, 0, 64));
		list.push_back(ContextInt("lighting_composite_debug_mode", "Debug",
			"0 full, 1 direct, 2 IBL, 3 SSGI, 4 surfel, 5 LPV.", &RenderContext::lightingCompositeDebugMode, 0, 5));
		list.push_back(ContextInt("post_debug_view", "Debug", "Final-output inspection of an intermediate image (PostProcessPass).",
			&RenderContext::postDebugView, 0, 64));
		list.push_back(ContextInt("ssr_debug_view", "Debug", "1 resolve, 2 temporal, 3 composite, 4 blur level.",
			&RenderContext::ssrDebugView, 0, 4));
		return list;
	}();
	return extras;
}

const RenderSetting* FindSetting(const std::string& key)
{
	if (const RenderSetting* table = nox::render::FindRenderSetting(key)) return table;
	for (const RenderSetting& extra : ExtraRenderSettings()) {
		if (extra.key == key) return &extra;
	}
	return nullptr;
}

PropertyDesc DescriptorFor(const RenderSetting& s)
{
	PropertyDesc d;
	d.name = s.key;
	d.type = s.type;
	d.enumNames = s.enumNames;
	d.minValue = s.minValue;
	d.maxValue = s.maxValue;
	d.flags = s.flags;
	return d;
}

json SettingJson(const RenderSetting& s, const RenderContext& ctx)
{
	return nox::reflect::ValueToJson(DescriptorFor(s), s.get(ctx));
}

// Converts and range-checks one JSON value for a render setting. Throws RpcError on mismatch.
PropValue SettingValue(const RenderSetting& s, const json& value)
{
	PropValue out;
	if (!nox::reflect::ValueFromJson(DescriptorFor(s), value, out)) {
		Invalid("value for '" + s.key + "' does not match its type");
	}
	if (s.maxValue > s.minValue) {
		double number = 0.0;
		if (const double* d = std::get_if<double>(&out)) number = *d;
		else if (const int64_t* i = std::get_if<int64_t>(&out)) number = static_cast<double>(*i);
		else return out;
		if (number < s.minValue || number > s.maxValue) {
			Invalid("value for '" + s.key + "' is outside [" + std::to_string(s.minValue) + ", " + std::to_string(s.maxValue) + "]");
		}
	}
	return out;
}

std::string g_qualityTierOverride;   // last tier applied through render.settings.set

std::string CurrentQualityTier()
{
	return g_qualityTierOverride.empty() ? SceneRenderSettings::ActiveTier() : g_qualityTierOverride;
}

std::shared_ptr<ModularRenderer> RequireRenderer(Core& core)
{
	std::shared_ptr<ModularRenderer> renderer = core.GetRenderer();
	if (!renderer) EngineError("renderer is not available");
	return renderer;
}

// ---- Lua -----------------------------------------------------------------------------------------

NoxLua::Runtime& AutomationVm()
{
	// A sandboxed VM owned by the automation server. Project worlds keep their own VM (LuaWorld), which this
	// command does not reach; see docs/automation.md.
	static NoxLua::Runtime vm(NoxLua::RuntimeLimits{}, nullptr);
	return vm;
}

// The VM of the loaded Lua project world, or null with the reason in `reason`.
NoxLua::Runtime* ProjectVm(Core& core, std::string& reason)
{
	auto* lua = dynamic_cast<NoxLua::LuaGameModule*>(core.GetGameModule());
	if (!lua) {
		reason = "no Lua project is loaded for this scene (pass \"isolated\": true to use the separate automation VM)";
		return nullptr;
	}
	NoxLua::LuaWorld* world = lua->MutableWorld();
	if (!world || !lua->IsAvailable()) {
		reason = "the Lua project did not load (see log.tail and the editor diagnostics)";
		return nullptr;
	}
	if (world->Loading()) {
		reason = "the Lua project is still loading; retry after engine.info reports loading=false";
		return nullptr;
	}
	return &world->GetRuntime();
}

// The first line of a Lua error message; the rest (the traceback) goes to the error's data.
std::string FirstLine(const std::string& message)
{
	const size_t nl = message.find('\n');
	return nl == std::string::npos ? message : message.substr(0, nl);
}

json TracebackData(const NoxLua::RunError& error)
{
	json data = json::object();
	const size_t nl = error.message.find('\n');
	if (nl != std::string::npos) data["traceback"] = error.message.substr(nl + 1);
	if (error.budgetExceeded) data["budget_exceeded"] = true;
	if (error.memoryExceeded) data["memory_exceeded"] = true;
	return data.empty() ? json() : data;
}

json LuaValueToJson(lua_State* L, int index)
{
	switch (lua_type(L, index)) {
	case LUA_TNIL:
		return nullptr;
	case LUA_TBOOLEAN:
		return lua_toboolean(L, index) != 0;
	case LUA_TNUMBER:
		if (lua_isinteger(L, index)) return static_cast<int64_t>(lua_tointeger(L, index));
		return lua_tonumber(L, index);
	case LUA_TSTRING: {
		size_t length = 0;
		const char* text = lua_tolstring(L, index, &length);
		return std::string(text, length);
	}
	default:
		return std::string("<") + lua_typename(L, lua_type(L, index)) + ">";
	}
}

// ---- Commands ------------------------------------------------------------------------------------

json EngineInfo(Invocation& inv)
{
	Core& core = RequireCore(inv);
	json info = json::object();
	info["version"] = "1.0.0";
	info["protocol"] = "2.0";
	info["automation_port"] = Port();
	info["mode"] = core.GetAppMode() == Core::AppMode::Editor ? "editor" : "game";
	info["scene"] = core.GetCurrentScenePath();
	info["scene_name"] = core.GetSceneName();
	info["loading"] = core.IsSceneLoading() || core.IsSceneSwapPending();
	info["load_progress"] = core.GetSceneLoadProgress();
	info["load_step"] = core.GetSceneLoadStep();
	if (!core.GetSceneLoadError().empty()) info["load_error"] = core.GetSceneLoadError();
	info["frame"] = Control().frameIndex;
	info["scene_frame"] = nox::SceneFrameClock::scene;  // live scene frames of the current scene (docs/scenes.md)
	info["fps"] = core.GetFPS();
	info["tier"] = CurrentQualityTier();
	info["paused"] = Control().paused;
	info["playing"] = core.IsPlaying();
	return info;
}

void RegisterEngine(Dispatcher& d)
{
	d.Register("engine.info", [](Invocation& inv) { return EngineInfo(inv); });

	d.Register("engine.quit", [](Invocation& inv) {
		RequireCore(inv).QuitApplication();
		json r = json::object();
		r["quitting"] = true;
		return r;
	});

	d.Register("engine.pause", [](Invocation& inv) {
		RequireCore(inv);
		Control().paused = true;
		json r = json::object();
		r["paused"] = true;
		r["frame"] = Control().frameIndex;
		return r;
	});

	d.Register("engine.resume", [](Invocation& inv) {
		RequireCore(inv);
		Control().paused = false;
		Control().stepRemaining = 0;   // cancels a step in progress; its caller gets the frame it reached
		json r = json::object();
		r["paused"] = false;
		r["frame"] = Control().frameIndex;
		return r;
	});

	// Pauses, then simulates exactly `frames` frames and answers after the last one has rendered.
	d.Register("engine.step", [](Invocation& inv) -> json {
		RequireCore(inv);
		const int64_t frames = ParamInt(inv.params, "frames", 1, 1, 100000);
		FrameControl& control = Control();
		control.paused = true;
		control.stepRemaining += static_cast<int>(frames);
		control.stepWaiters.push_back(inv.call);
		inv.Defer();
		return json();
	});
}

void RegisterScene(Dispatcher& d)
{
	d.Register("scene.list", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		json list = json::array();
		for (const std::string& path : core.GetSceneList()) list.push_back(path);
		return list;
	});

	d.Register("scene.load", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const std::string path = RequireString(inv.params, "path");
		if (!AssetIO::ReadText(path)) Invalid("scene not found: " + path);
		core.RequestSceneSwap(path);   // the normal scene swap, applied at the start of the next update
		json r = json::object();
		r["path"] = path;
		r["requested"] = true;
		return r;
	});

	d.Register("scene.save", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		json r = json::object();
		if (!Has(inv.params, "path")) {
			core.SaveCurrentScene();   // writes the authored source file; failures are reported in log.tail
			r["path"] = core.GetCurrentScenePath();
			r["requested"] = true;
			return r;
		}
		const std::string path = RequireString(inv.params, "path");
		if (path.size() < 5 || path.substr(path.size() - 5) != ".json") Invalid("scene.save path must end in .json");
		std::shared_ptr<SceneGraph> graph = RequireGraph(core);
		SceneLoader* loader = core.GetSceneLoader();
		if (!loader) EngineError("scene loader is not available");
		if (!loader->SaveScene(graph, path)) EngineError("could not write " + path);
		r["path"] = path;
		r["saved"] = true;
		return r;
	});

	d.Register("scene.tree", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const int64_t depth = ParamInt(inv.params, "depth", -1, -1, 1000);
		return NodeTree(RootOf(core), depth);
	});
}

void RegisterNodes(Dispatcher& d)
{
	d.Register("node.get", [](Invocation& inv) {
		const std::shared_ptr<SceneNode> node = RequireNode(inv.params);
		json r = NodeBrief(node);
		json components = json::object();
		for (const ComponentDesc* desc : Registry::Get().ComponentsOf(*node)) {
			json props = json::object();
			for (const PropertyDesc& prop : desc->properties) {
				if (!prop.AppliesTo(*node)) continue;
				PropValue value;
				if (!nox::reflect::GetProperty(*node, desc->name, prop.name, value)) continue;
				props[prop.name] = nox::reflect::ValueToJson(prop, value);
			}
			components[desc->name] = std::move(props);
		}
		r["properties"] = std::move(components);
		return r;
	});

	d.Register("node.set", [](Invocation& inv) {
		const std::shared_ptr<SceneNode> node = RequireNode(inv.params);
		const std::string component = RequireString(inv.params, "component");
		const std::string property = RequireString(inv.params, "property");
		if (!inv.params.contains("value")) Invalid("missing parameter 'value'");
		const ComponentDesc* desc = Registry::Get().Find(component);
		if (!desc) Invalid("unknown component '" + component + "'");
		const PropertyDesc* prop = desc->FindProperty(property);
		if (!prop) Invalid("component '" + component + "' has no property '" + property + "'");
		PropValue value;
		if (!nox::reflect::ValueFromJson(*prop, inv.params["value"], value)) {
			Invalid("value for '" + component + "." + property + "' does not match its type");
		}
		std::string error;
		if (!nox::reflect::SetProperty(*node, component, property, value, &error)) Invalid(error);
		PropValue readBack;
		json r = json::object();
		r["id"] = IdOf(node);
		r["component"] = component;
		r["property"] = property;
		if (nox::reflect::GetProperty(*node, component, property, readBack)) {
			r["value"] = nox::reflect::ValueToJson(*prop, readBack);
		}
		return r;
	});

	d.Register("node.create", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		std::shared_ptr<SceneNode> parent = OptNode(inv.params, "parent");
		json nodeJson = Has(inv.params, "node") ? inv.params["node"] : json::object();
		if (!nodeJson.is_object()) Invalid("'node' must be an object in scene-file node format");
		if (!nodeJson.contains("name")) nodeJson["name"] = "Node";
		std::shared_ptr<SceneNode> created = core.InstantiateNodeFromJson(nodeJson, parent);
		if (!created) EngineError("node was not created (play mode, or the node JSON was rejected)");
		return NodeBrief(created);
	});

	d.Register("node.delete", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const std::shared_ptr<SceneNode> node = RequireNode(inv.params);
		const int64_t id = IdOf(node);
		if (!core.DeleteNode(node)) EngineError("node " + std::to_string(id) + " could not be deleted (root, or play mode)");
		json r = json::object();
		r["deleted"] = id;
		return r;
	});

	d.Register("node.reparent", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const std::shared_ptr<SceneNode> node = RequireNode(inv.params);
		const std::shared_ptr<SceneNode> parent = OptNode(inv.params, "parent");   // null = scene root
		const int64_t index = ParamInt(inv.params, "index", -1, -1, 1000000);
		const bool keepWorld = OptBool(inv.params, "keep_world", true);
		if (!core.ReparentNode(node, parent, static_cast<int>(index), keepWorld)) {
			EngineError("node could not be reparented (cycle, root, or play mode)");
		}
		json r = json::object();
		r["id"] = IdOf(node);
		r["parent"] = parent ? json(IdOf(parent)) : json(nullptr);
		return r;
	});

	d.Register("node.find", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const bool hasName = Has(inv.params, "name");
		const bool hasType = Has(inv.params, "type");
		if (!hasName && !hasType) Invalid("node.find needs 'name' or 'type'");
		const std::string name = OptString(inv.params, "name", "");
		const std::string type = OptString(inv.params, "type", "");
		json matches = json::array();
		Walk(RootOf(core), [&](const std::shared_ptr<SceneNode>& node) {
			if (matches.size() >= 1000) return;
			if (hasName && node->GetName() != name) return;
			if (hasType && type != TypeName(node->GetNodeType())) return;
			matches.push_back(NodeBrief(node));
		});
		return matches;
	});
}

void RegisterRender(Dispatcher& d)
{
	d.Register("render.screenshot", [](Invocation& inv) -> json {
		Core& core = RequireCore(inv);
		RequireGraph(core);
		const std::string path = OptString(inv.params, "path",
			"captures/automation/screenshot_" + std::to_string(Control().frameIndex) + ".png");
		if (path.empty()) Invalid("path must not be empty");
		Control().screenshots.push_back({ path, inv.call });
		inv.Defer();   // answered in EndFrame, after Render and before Present
		return json();
	});

	d.Register("render.settings.get", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		const RenderContext& ctx = RequireRenderer(core)->GetContext();
		std::vector<std::string> keys;
		if (Has(inv.params, "keys")) {
			if (!inv.params["keys"].is_array()) Invalid("'keys' must be an array of names");
			for (const json& k : inv.params["keys"]) {
				if (!k.is_string()) Invalid("'keys' must be an array of names");
				keys.push_back(k.get<std::string>());
			}
		}
		json r = json::object();
		if (keys.empty()) {
			for (const RenderSetting& s : nox::render::RenderSettingTable()) r[s.key] = SettingJson(s, ctx);
			for (const RenderSetting& s : ExtraRenderSettings()) r[s.key] = SettingJson(s, ctx);
			r["quality_tier"] = CurrentQualityTier();
			return r;
		}
		for (const std::string& key : keys) {
			if (key == "quality_tier") {
				r[key] = CurrentQualityTier();
				continue;
			}
			const RenderSetting* s = FindSetting(key);
			if (!s) Invalid("unknown render setting '" + key + "'");
			r[key] = SettingJson(*s, ctx);
		}
		return r;
	});

	// Accepts {"key": "...", "value": ...} or {"settings": {key: value, ...}}. Every value is validated before
	// any is applied, so a bad key changes nothing.
	d.Register("render.settings.set", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		std::shared_ptr<ModularRenderer> renderer = RequireRenderer(core);
		RenderContext& ctx = renderer->GetContext();

		std::vector<std::pair<std::string, json>> requested;
		if (Has(inv.params, "settings")) {
			if (!inv.params["settings"].is_object()) Invalid("'settings' must be an object");
			for (auto it = inv.params["settings"].begin(); it != inv.params["settings"].end(); ++it) {
				requested.emplace_back(it.key(), it.value());
			}
		}
		else {
			const std::string key = RequireString(inv.params, "key");
			if (!inv.params.contains("value")) Invalid("missing parameter 'value'");
			requested.emplace_back(key, inv.params["value"]);
		}
		if (requested.empty()) Invalid("nothing to set");

		struct Pending { const RenderSetting* setting; PropValue value; };
		std::vector<Pending> pending;
		std::string tierName;
		for (const auto& entry : requested) {
			if (entry.first == "quality_tier") {
				if (!entry.second.is_string()) Invalid("quality_tier must be a tier name");
				tierName = entry.second.get<std::string>();
				continue;
			}
			const RenderSetting* s = FindSetting(entry.first);
			if (!s) Invalid("unknown render setting '" + entry.first + "'");
			if (!s->set) Invalid("render setting '" + entry.first + "' is read-only");
			pending.push_back({ s, SettingValue(*s, entry.second) });
		}

		json applied = json::object();
		for (const Pending& p : pending) {
			if (!p.setting->set(ctx, p.value)) Invalid("render setting '" + p.setting->key + "' rejected the value");
			applied[p.setting->key] = SettingJson(*p.setting, ctx);
		}
		if (!tierName.empty()) {
			// A quality tier is the "quality_tiers" object of the current scene file: its overrides are applied on top.
			const std::optional<std::string> text = AssetIO::ReadText(core.GetCurrentScenePath());
			const json root = text ? json::parse(*text, nullptr, false) : json();
			const json tiers = root.is_object() ? root.value("quality_tiers", json::object()) : json::object();
			if (!tiers.is_object() || !tiers.contains(tierName) || !tiers[tierName].is_object()) {
				Invalid("the current scene has no quality tier '" + tierName + "'");
			}
			SceneRenderSettings::Apply(ctx, tiers[tierName], false);
			g_qualityTierOverride = tierName;
			applied["quality_tier"] = tierName;
		}
		json r = json::object();
		r["applied"] = std::move(applied);
		return r;
	});

	d.Register("render.pass_timings", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		std::shared_ptr<ModularRenderer> renderer = RequireRenderer(core);
		json passes = json::array();
		double cpuTotal = 0.0;
		double gpuTotal = 0.0;
		for (const auto& m : renderer->GetLastPassMetrics()) {
			json p = json::object();
			p["name"] = m.name;
			p["cpu_ms"] = m.cpuTimeMs;
			p["gpu_ms"] = m.gpuTimeMs;
			p["cpu_wait_ms"] = m.cpuWaitSyncMs;
			p["draw_calls"] = m.drawCalls;
			p["dispatches"] = m.dispatchCount;
			passes.push_back(std::move(p));
			cpuTotal += m.cpuTimeMs;
			gpuTotal += m.gpuTimeMs;
		}
		json r = json::object();
		r["frame"] = Control().frameIndex;
		r["cpu_total_ms"] = cpuTotal;
		r["gpu_total_ms"] = gpuTotal;
		r["passes"] = std::move(passes);
		return r;
	});
}

void RegisterTools(Dispatcher& d)
{
	d.Register("log.tail", [](Invocation& inv) {
		const int64_t lines = ParamInt(inv.params, "lines", 200, 1, 20000);
		size_t total = 0;
		const size_t count = ConsoleLog::Get().Count();
		const size_t from = count > static_cast<size_t>(lines) ? count - static_cast<size_t>(lines) : 0;
		const std::vector<ConsoleLog::Line> entries = ConsoleLog::Get().Since(from, total);
		static const char* kLevels[] = { "info", "warning", "error" };
		json list = json::array();
		for (const ConsoleLog::Line& line : entries) {
			json e = json::object();
			e["text"] = line.text;
			e["level"] = kLevels[std::clamp(line.level, 0, 2)];
			e["time"] = line.time;
			list.push_back(std::move(e));
		}
		json r = json::object();
		r["total"] = total;
		r["lines"] = std::move(list);
		return r;
	});

	// Runs a chunk in the project world's VM (the VM the game scripts run in, with the nox API), or with
	// "isolated": true in the separate automation VM. Errors carry the first line as message and the traceback
	// in data.traceback.
	d.Register("lua.exec", [](Invocation& inv) -> json {
		Core& core = RequireCore(inv);
		const std::string code = RequireString(inv.params, "code");
		const std::string chunk = OptString(inv.params, "chunk", "=automation");
		const bool isolated = OptBool(inv.params, "isolated", false);

		NoxLua::Runtime* vm = nullptr;
		if (isolated) {
			vm = &AutomationVm();
			if (!vm->Ok()) EngineError("isolated Lua VM did not start: " + vm->InitError());
		}
		else {
			std::string reason;
			vm = ProjectVm(core, reason);
			if (!vm) EngineError(reason);
		}

		int ref = 0;
		std::string error;
		if (!vm->Compile(code, chunk, ref, error)) throw RpcError{ rpc::kEngineError, error, nullptr };
		lua_State* L = vm->State();
		const int base = vm->StackTop();
		NoxLua::RunError runError;
		const bool ok = vm->CallRef(ref, [](lua_State*) { return 0; }, 1, &runError);
		vm->Unref(ref);
		if (!ok) {
			lua_settop(L, base);
			throw RpcError{ rpc::kEngineError, FirstLine(runError.message), TracebackData(runError) };
		}
		json result = LuaValueToJson(L, -1);
		lua_settop(L, base);
		return result;
	});

	d.Register("camera.get", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		std::shared_ptr<Camera> camera = core.GetCamera();
		if (!camera) EngineError("no camera");
		const glm::vec3 front = glm::normalize(camera->GetCameraFrontVector());
		json r = json::object();
		r["position"] = Vec3Json(camera->GetCameraPosition());
		r["front"] = Vec3Json(front);
		r["up"] = Vec3Json(camera->GetCameraUpVector());
		r["rotation"] = json::array({ glm::degrees(std::atan2(front.z, front.x)), glm::degrees(std::asin(std::clamp(front.y, -1.0f, 1.0f))) });
		r["fov"] = camera->GetCameraFov();
		r["near"] = camera->GetCameraNearPlane();
		r["far"] = camera->GetCameraFarPlane();
		r["aspect"] = camera->GetAspect();
		return r;
	});

	// rotation = [yaw, pitch] in degrees (yaw 0 looks down +X); lookAt is an alternative to rotation.
	d.Register("camera.set", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		std::shared_ptr<Camera> camera = core.GetCamera();
		if (!camera) EngineError("no camera");
		const bool hasRotation = Has(inv.params, "rotation");
		const bool hasLookAt = Has(inv.params, "lookAt");
		if (hasRotation && hasLookAt) Invalid("give either 'rotation' or 'lookAt', not both");

		glm::vec3 position = Has(inv.params, "position") ? Vec3Param(inv.params, "position") : camera->GetCameraPosition();
		float yaw = 0.0f;
		float pitch = 0.0f;
		if (hasRotation) {
			if (!inv.params["rotation"].is_array() || inv.params["rotation"].size() != 2) Invalid("'rotation' must be [yaw, pitch] in degrees");
			yaw = static_cast<float>(inv.params["rotation"][0].get<double>());
			pitch = static_cast<float>(inv.params["rotation"][1].get<double>());
		}
		else if (hasLookAt) {
			glm::vec3 dir = Vec3Param(inv.params, "lookAt") - position;
			if (glm::dot(dir, dir) < 1e-10f) Invalid("'lookAt' equals the camera position");
			dir = glm::normalize(dir);
			yaw = glm::degrees(std::atan2(dir.z, dir.x));
			pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
		}
		else {
			const glm::vec3 front = glm::normalize(camera->GetCameraFrontVector());
			yaw = glm::degrees(std::atan2(front.z, front.x));
			pitch = glm::degrees(std::asin(std::clamp(front.y, -1.0f, 1.0f)));
		}
		camera->SetPose(position, glm::vec3(0.0f, 1.0f, 0.0f), yaw, pitch);

		if (Has(inv.params, "fov") || Has(inv.params, "near") || Has(inv.params, "far")) {
			const float fov = static_cast<float>(OptNumber(inv.params, "fov", camera->GetCameraFov()));
			const float nearPlane = static_cast<float>(OptNumber(inv.params, "near", camera->GetCameraNearPlane()));
			const float farPlane = static_cast<float>(OptNumber(inv.params, "far", camera->GetCameraFarPlane()));
			if (fov <= 1.0f || fov >= 179.0f) Invalid("'fov' must be between 1 and 179 degrees");
			if (nearPlane <= 0.0f || farPlane <= nearPlane) Invalid("need 0 < near < far");
			camera->SetPerspective(fov, camera->GetAspect(), nearPlane, farPlane);
		}
		json r = json::object();
		r["position"] = Vec3Json(camera->GetCameraPosition());
		r["fov"] = camera->GetCameraFov();
		return r;
	});

	// Injects SDL key or mouse events; they travel the normal event path (MainWindow -> Core -> Input layer)
	// from the next frame on.
	d.Register("input.inject", [](Invocation& inv) {
		RequireCore(inv);
		const bool hasKey = Has(inv.params, "key");
		const bool hasMouse = Has(inv.params, "mouse");
		if (Has(inv.params, "action")) Invalid("action injection is not supported; inject 'key' or 'mouse' events");
		if (hasKey == hasMouse) Invalid("give exactly one of 'key' or 'mouse'");
		const std::string state = RequireString(inv.params, "state");
		if (state != "down" && state != "up") Invalid("'state' must be \"down\" or \"up\"");
		const bool down = state == "down";

		SDL_Window* focus = SDL_GetKeyboardFocus();
		if (!focus) focus = SDL_GetMouseFocus();
		const Uint32 windowId = focus ? SDL_GetWindowID(focus) : 1;

		SDL_Event event{};
		json r = json::object();
		if (hasKey) {
			const std::string name = RequireString(inv.params, "key");
			const SDL_Keycode sym = SDL_GetKeyFromName(name.c_str());
			if (sym == SDLK_UNKNOWN) Invalid("unknown key name '" + name + "' (SDL key names, e.g. \"W\", \"Space\", \"Left\")");
			event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
			event.key.timestamp = SDL_GetTicks();
			event.key.windowID = windowId;
			event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
			event.key.repeat = 0;
			event.key.keysym.sym = sym;
			event.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
			event.key.keysym.mod = 0;
			r["key"] = name;
		}
		else {
			const std::string button = RequireString(inv.params, "mouse");
			Uint8 which = 0;
			if (button == "left") which = SDL_BUTTON_LEFT;
			else if (button == "right") which = SDL_BUTTON_RIGHT;
			else if (button == "middle") which = SDL_BUTTON_MIDDLE;
			else Invalid("'mouse' must be \"left\", \"right\" or \"middle\"");
			int mx = 0, my = 0;
			SDL_GetMouseState(&mx, &my);
			event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			event.button.timestamp = SDL_GetTicks();
			event.button.windowID = windowId;
			event.button.which = 0;
			event.button.button = which;
			event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
			event.button.clicks = 1;
			event.button.x = static_cast<Sint32>(OptNumber(inv.params, "x", mx));
			event.button.y = static_cast<Sint32>(OptNumber(inv.params, "y", my));
			r["mouse"] = button;
		}
		if (SDL_PushEvent(&event) < 0) EngineError("SDL_PushEvent failed");
		r["state"] = state;
		r["applies_at"] = "next frame";
		return r;
	});

	d.Register("editor.select", [](Invocation& inv) {
		Core& core = RequireCore(inv);
		if (core.GetAppMode() != Core::AppMode::Editor) EngineError("editor.select needs editor mode (--editor)");
		ImGuiInterface* ui = core.GetImGuiInterface();
		if (!ui) EngineError("editor UI is not available");
		const std::shared_ptr<SceneNode> node = OptNode(inv.params, "id");   // null clears the selection
		ui->SetSelectedNode(node);
		json r = json::object();
		r["selected"] = node ? json(IdOf(node)) : json(nullptr);
		return r;
	});
}

} // namespace

void RegisterEngineCommands(Dispatcher& dispatcher)
{
	RegisterEngine(dispatcher);
	RegisterScene(dispatcher);
	RegisterNodes(dispatcher);
	RegisterRender(dispatcher);
	RegisterTools(dispatcher);
}

} // namespace automation
