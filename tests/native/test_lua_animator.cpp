// Lua access to the Animator state machine: graph set through the component proxy, set_float / set_trigger /
// current_state through ctx:component, and the failure of a method on an undeclared parameter. Headless: a
// SceneGraph with a rig node whose model has four clips (no GPU, no model file). The scene's animation stage is
// stepped from the harness, before each Lua update, the same order the engine uses.
#include "TestFramework.h"

#include "Animation.h"
#include "ComponentManager.h"
#include "ComponentTypes.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "scripting/LuaWorld.h"
#include "reflect/Reflection.h"
#include "reflect/ReflectServices.h"

#include "lua.h"

#include <map>
#include <memory>
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

	const char* kEntry = "projects/animator/scripts/main.lua";

	// The graph the script installs: idle -> walk -> run by speed, any state -> jump by trigger, jump has no way out.
	const char* kGraphScript = R"LUA(
local GRAPH = [[
{
  "params": { "speed": "float", "jump": "trigger" },
  "default": "idle",
  "states": [
    { "name": "idle", "clip": "idle" },
    { "name": "walk", "clip": "walk" },
    { "name": "run", "clip": "run" },
    { "name": "jump", "clip": "jump" }
  ],
  "transitions": [
    { "from": "idle", "to": "walk", "when": [ { "param": "speed", "op": ">", "value": 1 } ] },
    { "from": "walk", "to": "run", "when": [ { "param": "speed", "op": ">", "value": 5 } ] },
    { "from": "*", "to": "jump", "when": [ { "param": "jump" } ] }
  ]
}
]]
failures = ""
local function expect(ok, message)
  if not ok then failures = failures .. message .. ";" end
end
nox.on("begin_play", function(ctx)
  local anim = ctx:component(ctx:find("rig"), "Animator")
  anim.graph = GRAPH
end)
nox.system{
  name = "drive",
  update = function(ctx, dt)
    local anim = ctx:component(ctx:find("rig"), "Animator")
    local frame = ctx:frame()
    if frame == 1 then
      expect(anim:current_state() == "idle", "frame 1 current_state " .. anim:current_state())
    elseif frame == 2 then
      anim:set_float("speed", 2)
    elseif frame == 3 then
      expect(anim:current_state() == "walk", "frame 3 " .. anim:current_state())
      anim:set_float("speed", 6)
    elseif frame == 4 then
      expect(anim:current_state() == "run", "frame 4 " .. anim:current_state())
      anim:set_trigger("jump")
    elseif frame == 5 then
      expect(anim:current_state() == "jump", "frame 5 " .. anim:current_state())
    elseif frame == 6 or frame == 7 then
      -- The trigger was consumed by the jump transition; nothing else sends the rig back.
      expect(anim:current_state() == "jump", "trigger not consumed, frame " .. frame .. " " .. anim:current_state())
    elseif frame == 8 then
      local ok, err = pcall(function() anim:set_float("nope", 1) end)
      expect(not ok and string.find(tostring(err), "failed") ~= nil, "undeclared parameter should raise")
      ok, err = pcall(function() anim:set_bool("speed", true) end)
      expect(not ok, "bool write to a float parameter should raise")
      expect(anim:current_state() == "jump", "state changed by a failed write")
      result = "done"
    end
  end,
}
)LUA";

	struct Harness {
		MemorySource source;
		SceneGraph scene;
		NoxLua::LuaWorld world;
		std::shared_ptr<SceneNode> rig;
		nox::reflect::EngineServices savedServices = nox::reflect::Services();

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			nox::reflect::RegisterBuiltinComponents();
			// The Animator methods reach the animation system through the scene graph (Core sets this in the app).
			nox::reflect::Services().sceneGraph = [this] { return &scene; };

			auto model = std::make_shared<Scene>();
			model->nodes.resize(1);
			model->nodes[0].parent = -1;
			model->nodes[0].localTransform = glm::mat4(1.0f);
			for (const char* name : { "idle", "walk", "run", "jump" }) {
				Animation clip;
				clip.name = name;
				clip.isLooping = true;
				Animation::Channel channel;
				channel.type = Animation::ChannelType::TRANSLATION;
				channel.path = "translation";
				channel.targetNode = 0;
				channel.keyframes = { 0.0f, 1.0f };
				channel.values = { glm::vec4(0.0f), glm::vec4(0.0f) };
				clip.channels.push_back(channel);
				clip.UpdateDuration();
				model->animations.push_back(clip);
			}

			rig = std::make_shared<SceneNode>(scene.GetComponentManager(), scene.GetTransformSystem());
			rig->SetName("rig");
			scene.GetRoot()->AddChild(rig);
			rig->SetECSContext(scene.GetComponentManager(), scene.GetTransformSystem());
			rig->CreateECSEntity("rig");
			RenderableComponent renderable;
			renderable.model = model;
			scene.GetComponentManager()->AddRenderable(rig->GetEntityID(), renderable);
			scene.GetComponentManager()->AddAnimation(rig->GetEntityID());
		}

		~Harness() {
			nox::reflect::Services() = savedServices;
		}

		bool Load() {
			source.files[kEntry] = std::string(kGraphScript);
			NoxLua::ProjectContent project;
			project.name = "animator";
			project.entry = kEntry;
			project.scriptRoots = { "projects/animator/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}

		// One update: the animation stage first (as Core does), then Lua.
		void Frame(float dt = 1.0f / 60.0f) {
			scene.UpdateAnimations(dt);
			world.Update(dt, true);
		}

		std::string GlobalStr(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const char* s = lua_tostring(L, -1);
			std::string value = s ? s : "";
			lua_pop(L, 1);
			return value;
		}
	};
}

NOX_TEST(LuaAnimator, GraphDrivenFromLuaWithTriggersAndStates)
{
	Harness h;
	NOX_REQUIRE(h.Load());
	NOX_REQUIRE(h.world.IsLoaded());
	h.world.BeginPlay();
	for (int i = 0; i < 9; ++i) {
		h.Frame();
	}
	NOX_CHECK(h.world.Diagnostics().empty());
	NOX_CHECK_EQ(h.GlobalStr("failures"), std::string(""));
	NOX_CHECK_EQ(h.GlobalStr("result"), std::string("done"));
	NOX_CHECK_EQ(h.scene.GetAnimationSystem()->AnimatorState(h.rig->GetEntityID()), std::string("jump"));
}
