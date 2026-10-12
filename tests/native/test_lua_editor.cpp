// Lua editor binding: viewport handles (drag, release, undo through a sink, weak lifetime), panel registration from
// nox.editor_panel, and a headless ImGui frame that draws a project panel with every ed widget.
#include "TestFramework.h"

#include "EditorExtensions.h"
#include "SceneGraph.h"
#include "scripting/LuaEditor.h"
#include "scripting/LuaEditorPanel.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <IMGUI/imgui.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

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

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
		}

		bool Load(const std::string& body, const nlohmann::json& authored = nlohmann::json::object()) {
			source.files[kEntry] = body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, authored.value("entities", nlohmann::json::object()));
		}
	};

	// An ImGui context with a built font atlas and one frame open (no backend: nothing is rendered).
	struct ImGuiFrame {
		ImGuiFrame() {
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;   // headless: never write imgui.ini into the working directory
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.Fonts->AddFontDefault();
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			ImGui::NewFrame();
		}
		~ImGuiFrame() {
			ImGui::EndFrame();
			ImGui::DestroyContext();
		}
	};
}

NOX_TEST(LuaEditor, HandleDragRecordsOneUndoStep) {
	auto handles = std::make_shared<NoxLua::EditorHandles>();
	std::vector<std::string> labels;
	std::function<void()> undo, redo;
	handles->SetUndoSink([&](const std::string& label, std::function<void()> u, std::function<void()> r) {
		labels.push_back(label);
		undo = u;
		redo = r;
	});
	NOX_CHECK(handles->Set("checkpoint", glm::vec3(1.0f, 0.5f, 2.0f)));

	handles->Drag("checkpoint", glm::vec2(5.0f, 6.0f));
	handles->Drag("checkpoint", glm::vec2(7.0f, 8.0f));   // the drag continues; the start is kept
	NOX_CHECK(handles->Dragging("checkpoint"));
	glm::vec3 p;
	NOX_REQUIRE(handles->Get("checkpoint", p));
	NOX_CHECK(p == glm::vec3(7.0f, 0.5f, 8.0f));           // height is kept while dragging on the ground plane

	handles->EndDrag("checkpoint");
	NOX_CHECK(!handles->Dragging("checkpoint"));
	NOX_REQUIRE(labels.size() == 1u);
	NOX_CHECK(labels[0] == "Move point");

	undo();
	NOX_REQUIRE(handles->Get("checkpoint", p));
	NOX_CHECK(p == glm::vec3(1.0f, 0.5f, 2.0f));
	redo();
	NOX_REQUIRE(handles->Get("checkpoint", p));
	NOX_CHECK(p == glm::vec3(7.0f, 0.5f, 8.0f));

	// A drag that does not move the point records nothing.
	handles->Drag("checkpoint", glm::vec2(7.0f, 8.0f));
	handles->EndDrag("checkpoint");
	NOX_CHECK(labels.size() == 1u);
}

NOX_TEST(LuaEditor, UndoIsSafeAfterTheHandlesAreGone) {
	std::function<void()> undo;
	{
		auto handles = std::make_shared<NoxLua::EditorHandles>();
		handles->SetUndoSink([&](const std::string&, std::function<void()> u, std::function<void()>) { undo = u; });
		handles->Set("p", glm::vec3(0.0f));
		handles->Drag("p", glm::vec2(3.0f, 3.0f));
		handles->EndDrag("p");
	}
	NOX_REQUIRE(static_cast<bool>(undo));
	undo();   // the owner is gone: nothing happens, nothing crashes
}

NOX_TEST(LuaEditor, HandleCapacityIsBounded) {
	NoxLua::EditorHandles handles;
	for (size_t i = 0; i < NoxLua::editor_limits::kMaxHandles; ++i) {
		NOX_REQUIRE(handles.Set("h" + std::to_string(i), glm::vec3(0.0f)));
	}
	NOX_CHECK(!handles.Set("one_more", glm::vec3(0.0f)));
	NOX_CHECK(handles.Set("h0", glm::vec3(1.0f)));   // existing points can still move
}

NOX_TEST(LuaEditor, PanelsAreDeclaredByTheProject) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'tune', title = 'Tuning', draw = function(ctx, ed) ed.text('hi') end }\n"
		"nox.editor_panel{ id = 'map', draw = function(ctx, ed) ed.separator() end }\n"
		"nox.editor_panel{ id = 'tune', title = 'Tuning 2', draw = function(ctx, ed) ed.text('again') end }\n"));
	const auto& panels = h.world.EditorPanels();
	NOX_REQUIRE(panels.size() == 2u);
	NOX_CHECK(panels[0].id == "tune");
	NOX_CHECK(panels[0].title == "Tuning 2");   // the last declaration of an id wins
	NOX_CHECK(panels[1].title == "map");        // the title defaults to the id
	NOX_CHECK(!h.world.HasEditorViewport());
}

NOX_TEST(LuaEditor, HandlesAreRefusedOutsideTheViewport) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'p', draw = function(ctx, ed) ed.handle('a', 1, 2, 3) end }\n"));
	// ed.handle is a viewport tool: in a panel it is an error of the panel (the panel faults), and no point is created.
	NoxLua::EditorHandles handles;
	NOX_CHECK(!h.world.DrawEditorPanel("p", nullptr, &handles));
	NOX_CHECK(!handles.Has("a"));
	NOX_REQUIRE(h.world.Diagnostics().size() == 1u);
	NOX_CHECK(h.world.Diagnostics()[0].message.find("only valid inside nox.editor_viewport") != std::string::npos);
	// A faulted panel is skipped until reload.
	NOX_CHECK(!h.world.DrawEditorPanel("p", nullptr, &handles));
	NOX_CHECK(h.world.Diagnostics().size() == 1u);
}

NOX_TEST(LuaEditor, ViewportHandleReturnsDragState) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_viewport(function(ctx, ed)\n"
		"  dragging, x, y, z = ed.handle('cp', 1, 0.5, 2, { radius = 9, color = 0xFF00FF00 })\n"
		"end)\n"));
	NoxLua::EditorHandles handles;
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	NOX_CHECK(handles.Has("cp"));
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "x");
	NOX_CHECK(lua_tonumber(L, -1) == 1.0);
	lua_pop(L, 1);

	// A drag from the overlay moves the stored point; the next frame reports the new position.
	handles.Drag("cp", glm::vec2(4.0f, 5.0f));
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	lua_getglobal(L, "x");
	NOX_CHECK(lua_tonumber(L, -1) == 4.0);
	lua_pop(L, 1);
	lua_getglobal(L, "z");
	NOX_CHECK(lua_tonumber(L, -1) == 5.0);
	lua_pop(L, 1);
}

NOX_TEST(LuaEditor, PanelWidgetsDrawInAnImGuiFrame) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'tune', title = 'Tuning', draw = function(ctx, ed)\n"
		"  ed.text('Engine')\n"
		"  ed.separator()\n"
		"  local pressed = ed.button('Apply')\n"
		"  local on = ed.checkbox('Enabled', true)\n"
		"  local power = ed.input_float('Power', 1.5)\n"
		"  local gear = ed.input_int('Gear', 3)\n"
		"  local label = ed.input_text('Name', 'sport')\n"
		"  local grip = ed.slider('Grip', 0.5, 0, 1)\n"
		"  local choice = ed.combo('Mode', 2, { 'eco', 'sport', 'track' })\n"
		"  ed.same_line()\n"
		"  ed.tree('Advanced', function(e)\n"
		"    e.text('nested')\n"
		"    e.tree('Deeper', function(f) f.text('deeper') end)\n"
		"  end)\n"
		"  ok = (pressed == false) and (on == true) and (power == 1.5) and (gear == 3) and (label == 'sport')\n"
		"    and (grip == 0.5) and (choice == 2)\n"
		"end }\n"));
	NoxLua::EditorHandles handles;
	{
		ImGuiFrame frame;
		ImGui::Begin("panel host");
		NOX_CHECK(h.world.DrawEditorPanel("tune", nullptr, &handles));
		ImGui::End();
	}
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "ok");
	NOX_CHECK(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	NOX_CHECK(h.world.Diagnostics().empty());
}

NOX_TEST(LuaEditor, DeclaredDataIsListedForTheInspector) {
	Harness h;
	h.scene.GetComponentManager()->CreateEntity("Player");
	const nlohmann::json authored = nlohmann::json::parse(
		"{ \"entities\": { \"Player\": { \"Health\": { \"current\": 3, \"maximum\": 3 } } } }");
	NOX_REQUIRE(h.Load("nox.schema('Health', { current = 'integer', maximum = 'integer' }, { current = 3, maximum = 3 })\n", authored));
	const auto handles = h.world.DeclaredHandles("Health");
	NOX_REQUIRE(handles.size() == 1u);
	NoxLua::Value value;
	NOX_REQUIRE(h.world.GetDeclaredField(handles[0], "Health", "current", value));
	NOX_CHECK(std::get<int64_t>(value) == 3);
	std::string error;
	NOX_CHECK(h.world.SetDeclaredField(handles[0], "Health", "current", NoxLua::Value(int64_t(1)), error));
	NOX_REQUIRE(h.world.GetDeclaredField(handles[0], "Health", "current", value));
	NOX_CHECK(std::get<int64_t>(value) == 1);
	NOX_CHECK(!h.world.SetDeclaredField(handles[0], "Health", "current", NoxLua::Value(std::string("x")), error));
}

namespace {
	// Records what the editor bindings ask of the host. Only the members the tests use do anything.
	class FakeHost : public EditorHost {
	public:
		struct Undo {
			std::string label;
			std::function<void()> undo, redo;
		};
		bool playing = false;
		int playRequests = 0;
		int stopRequests = 0;
		std::vector<Undo> undos;
		std::vector<ViewportHandle> viewportHandles;

		bool IsPlaying() const override { return playing; }
		void RequestPlay() override { ++playRequests; playing = true; }
		void RequestStop() override { ++stopRequests; playing = false; }
		void MarkDirty() override {}
		void PushUndo(const std::string& label, std::function<void()> undo, std::function<void()> redo) override {
			undos.push_back(Undo{ label, std::move(undo), std::move(redo) });
		}
		void ApplyGameJson(const nlohmann::json&) override {}
		nlohmann::json GetAuthoredGameJson() const override { return nlohmann::json::object(); }
		void CommitGameEdit(const std::string&) override {}
		void ApplyRuntimeChangesToAuthored() override {}
		IGameModule* Game() override { return nullptr; }
		SceneGraph* GetSceneGraph() override { return nullptr; }
		ModularRenderer* GetRenderer() override { return nullptr; }
		bool ProjectToViewport(const glm::vec3& world, ImVec2& out) const override {
			if (world.z < -1000.0f) return false;   // behind the camera in this fake
			out = ImVec2(world.x * 10.0f, world.z * 10.0f);
			return true;
		}
		void AddViewportHandle(const ViewportHandle& handle) override { viewportHandles.push_back(handle); }
		void ShowPanel(const std::string&) override {}
	};

	long long GlobalInt(lua_State* L, const char* name) {
		lua_getglobal(L, name);
		const long long value = static_cast<long long>(lua_tointeger(L, -1));
		lua_pop(L, 1);
		return value;
	}
}

NOX_TEST(LuaEditor, UndoStepsRunInTheirWorldAndDieWithIt) {
	MemorySource source;
	source.files[kEntry] =
		"x = 5\n"
		"nox.editor_panel{ id = 'p', draw = function(ctx, ed)\n"
		"  ed.undo('Set x', function() x = 0 end, function() x = 2 end)\n"
		"end }\n";
	NoxLua::ProjectContent project;
	project.name = "t";
	project.entry = kEntry;
	project.scriptRoots = { "projects/t/scripts" };
	SceneGraph scene;
	auto world = std::make_unique<NoxLua::LuaWorld>(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source);
	NOX_REQUIRE(world->Load(project, scene, nlohmann::json::object()));

	FakeHost host;
	NoxLua::EditorHandles handles;
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(world->DrawEditorPanel("p", &host, &handles));
		ImGui::End();
	}
	NOX_REQUIRE(host.undos.size() == 1u);
	NOX_CHECK(host.undos[0].label == "Set x");
	lua_State* L = world->GetRuntime().State();
	host.undos[0].redo();
	NOX_CHECK(GlobalInt(L, "x") == 2);
	host.undos[0].undo();
	NOX_CHECK(GlobalInt(L, "x") == 0);
	NOX_CHECK(world->Diagnostics().empty());

	// A failing step is a diagnostic of its world and does not fault the world.
	MemorySource bad;
	bad.files[kEntry] =
		"nox.editor_panel{ id = 'p', draw = function(ctx, ed)\n"
		"  ed.undo('Break', function() error('boom') end, function() end)\n"
		"end }\n";
	NoxLua::LuaWorld broken(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &bad);
	NOX_REQUIRE(broken.Load(project, scene, nlohmann::json::object()));
	FakeHost brokenHost;
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(broken.DrawEditorPanel("p", &brokenHost, &handles));
		ImGui::End();
	}
	brokenHost.undos[0].undo();
	NOX_REQUIRE(broken.Diagnostics().size() == 1u);
	NOX_CHECK(broken.Diagnostics()[0].source.find("editor undo") != std::string::npos);
	NOX_CHECK(broken.Diagnostics()[0].message.find("boom") != std::string::npos);

	// The world goes away (reload, Reconfigure, scene load): its steps are dropped and do nothing, safely.
	world.reset();
	host.undos[0].redo();
	host.undos[0].undo();
}

NOX_TEST(LuaEditor, PlayStateAndRequestsGoThroughTheHost) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'play', draw = function(ctx, ed)\n"
		"  seen = ed.playing()\n"
		"  ed.request_play()\n"
		"  ed.request_play()\n"
		"end }\n"
		"nox.editor_panel{ id = 'stop', draw = function(ctx, ed)\n"
		"  ed.request_stop()\n"
		"  ed.request_stop()\n"
		"end }\n"));
	FakeHost host;
	NoxLua::EditorHandles handles;
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(h.world.DrawEditorPanel("play", &host, &handles));
		ImGui::End();
	}
	NOX_CHECK(host.playRequests == 1);   // the second request sees the play already started
	NOX_CHECK(host.playing);
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "seen");
	NOX_CHECK(lua_toboolean(L, -1) == 0);   // not playing when the panel started
	lua_pop(L, 1);
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(h.world.DrawEditorPanel("stop", &host, &handles));
		ImGui::End();
	}
	NOX_CHECK(host.stopRequests == 1);
	NOX_CHECK(!host.playing);
	NOX_CHECK(h.world.Diagnostics().empty());
}

NOX_TEST(LuaEditor, HandleLabelAndClickedFlag) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_viewport(function(ctx, ed)\n"
		"  local dragging, x, y, z, clicked = ed.handle('cp', 1, 0.5, 2, { label = 'Move checkpoint' })\n"
		"  if clicked then clicks = (clicks or 0) + 1 end\n"
		"end)\n"));
	NoxLua::EditorHandles handles;
	std::vector<std::string> labels;
	handles.SetUndoSink([&](const std::string& label, std::function<void()>, std::function<void()>) { labels.push_back(label); });
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "clicks");
	NOX_CHECK(lua_isnil(L, -1));   // no click yet
	lua_pop(L, 1);

	// A press reported by the overlay is seen once, on the next frame.
	handles.MarkClicked("cp");
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	NOX_CHECK(GlobalInt(L, "clicks") == 1);

	// The drag that is released records its undo step under the handle's label.
	handles.Drag("cp", glm::vec2(4.0f, 5.0f));
	NOX_REQUIRE(h.world.DrawEditorViewport(nullptr, &handles));
	NOX_REQUIRE(labels.size() == 1u);
	NOX_CHECK(labels[0] == "Move checkpoint");
}

NOX_TEST(LuaEditor, WidgetsAndViewportLinesDrawInAFrame) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.editor_panel{ id = 'w', draw = function(ctx, ed)\n"
		"  local sel = ed.selectable('Row A', true)\n"
		"  local v = ed.drag_float('Speed', 2.5, 0.1, 0, 10)\n"
		"  ed.disabled(true, function(e) e.checkbox('Locked', false) end)\n"
		"  ed.columns(2, function(e)\n"
		"    e.text('Name'); e.next_column(); e.text('Value')\n"
		"  end)\n"
		"  ed.button('Hover')\n"
		"  ed.tooltip('explains the button')\n"
		"  ok = (type(sel) == 'boolean') and (v == 2.5)\n"
		"end }\n"
		"nox.editor_viewport(function(ctx, ed)\n"
		"  ed.line3({ 0, 0, 0 }, { x = 5, y = 0, z = 5 }, 0xFFFF0000, 2)\n"
		"  ed.text3(1, 0, 1, 'CP 1', 0xFFFFFFFF)\n"
		"  vok = true\n"
		"end)\n"
		"nox.editor_panel{ id = 'bad', draw = function(ctx, ed) ed.line3({ 0, 0, 0 }, { 1, 0, 1 }) end }\n"));
	NoxLua::EditorHandles handles;
	FakeHost host;
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(h.world.DrawEditorPanel("w", &host, &handles));
		NOX_CHECK(h.world.DrawEditorViewport(&host, &handles));
		ImGui::End();
	}
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "ok");
	NOX_CHECK(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	lua_getglobal(L, "vok");
	NOX_CHECK(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	NOX_CHECK(h.world.Diagnostics().empty());

	// Lines and labels are viewport tools: in a panel they fault the panel with the usual message.
	{
		ImGuiFrame frame;
		ImGui::Begin("host");
		NOX_CHECK(!h.world.DrawEditorPanel("bad", &host, &handles));
		ImGui::End();
	}
	NOX_REQUIRE(h.world.Diagnostics().size() == 1u);
	NOX_CHECK(h.world.Diagnostics()[0].message.find("ed.line3 is only valid inside nox.editor_viewport") != std::string::npos);
}

// A project's panels are listed only while that project is the active module (EditorPanel::available).
NOX_TEST(LuaEditor, ProjectPanelsAreAvailableOnlyForTheirProject) {
	Harness h;
	NOX_REQUIRE(h.Load("nox.editor_panel{ id = 'tune', title = 'Tuning', draw = function(ctx, ed) ed.text('x') end }\n"));
	NoxLua::RegisterLuaProjectPanels(h.world, "projects/t/project.json");
	const EditorPanel* panel = nullptr;
	for (const EditorPanel& p : EditorExtensions::Panels()) {
		if (p.id == "lua.panel.tune") panel = &p;
	}
	NOX_REQUIRE(panel != nullptr);
	NOX_REQUIRE(static_cast<bool>(panel->available));
	FakeHost host;   // no module active
	NOX_CHECK(!panel->available(host));
}
