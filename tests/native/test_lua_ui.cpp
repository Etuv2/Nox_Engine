// Lua ui drawing: recorder bounds, draw handlers (nox.on_draw), argument errors, the per-frame budget,
// text measurement with an ImGui font and playback onto an ImDrawList. Headless (no GPU, no window).
#include "TestFramework.h"

#include "SceneGraph.h"
#include "scripting/LuaUi.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <IMGUI/imgui.h>

#include <map>
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

		bool Load(const std::string& body) {
			source.files[kEntry] = body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}

		void Draw(NoxLua::UiRecorder& rec) {
			world.DrawFrame(1.0f / 60.0f, glm::vec2(1280.0f, 720.0f), glm::vec2(0.0f), rec);
		}

		bool GlobalBool(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const bool value = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		}

		size_t DiagnosticCount() const { return world.Diagnostics().size(); }
	};

	// Creates an ImGui context with the default font baked, so text measurement has glyph metrics.
	struct ImGuiScope {
		ImGuiScope() {
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;   // headless: never write imgui.ini into the working directory
			io.Fonts->AddFontDefault();
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		}
		~ImGuiScope() { ImGui::DestroyContext(); }
	};

	// IM_COL32 layout: r | g << 8 | b << 16 | a << 24.
	constexpr uint32_t Col(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
		return r | (g << 8) | (b << 16) | (a << 24);
	}
}

NOX_TEST(LuaUi, RecorderBoundsAndClipBalance) {
	NoxLua::UiRecorder rec;
	rec.Begin();
	NOX_CHECK(!rec.PopClip());
	for (size_t i = 0; i < NoxLua::ui_limits::kMaxClipDepth; ++i) NOX_REQUIRE(rec.PushClip(glm::vec2(0.0f), glm::vec2(1.0f)));
	NOX_CHECK(!rec.PushClip(glm::vec2(0.0f), glm::vec2(1.0f)));
	NOX_CHECK_EQ(rec.ClipDepth(), NoxLua::ui_limits::kMaxClipDepth);
	for (size_t i = 0; i < NoxLua::ui_limits::kMaxClipDepth; ++i) NOX_REQUIRE(rec.PopClip());
	NOX_CHECK_EQ(rec.ClipDepth(), 0u);

	rec.Begin();
	NoxLua::UiCommand c;
	for (size_t i = 0; i < NoxLua::ui_limits::kMaxCommands + 10; ++i) rec.Add(c);
	NOX_CHECK_EQ(rec.Commands().size(), NoxLua::ui_limits::kMaxCommands);
	NOX_CHECK_EQ(rec.Dropped(), 10u);

	rec.Begin();
	std::vector<glm::vec2> big(NoxLua::ui_limits::kMaxPoints + 1, glm::vec2(0.0f));
	NOX_CHECK(!rec.AddPoints(c, big));
	NOX_CHECK_EQ(rec.Points().size(), 0u);
	NOX_CHECK_EQ(rec.Dropped(), 1u);
	rec.Begin();
	NOX_CHECK_EQ(rec.Dropped(), 0u);
}

NOX_TEST(LuaUi, DrawHandlerRecordsCommands) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  ui.rect_filled(10, 20, 100, 30, 0xFF102030, 4)\n"
		"  ui.line(0, 0, 5, 5, {r=255, g=0, b=0, a=128}, 2)\n"
		"  ui.polyline({0,0, 10,0, 10,10}, 0xFFFFFFFF, 1, true)\n"
		"  ui.polygon_filled({{0,0},{10,0},{5,8}}, 0xFF00FF00)\n"
		"  ui.circle(50, 50, 12, 0xFFFFFFFF)\n"
		"  ui.arc(50, 50, 20, 0, 3.14159, 0xFFFFFFFF, 2)\n"
		"  ui.clip_push(0, 0, 64, 64)\n"
		"  ui.text('', 'hello', 5, 5, 14, 0xFFFFFFFF, 'center', 'middle')\n"
		"  ui.clip_pop()\n"
		"  ui.image('hud/speed', 1, 2, 16, 16)\n"
		"end)\n"));

	NoxLua::UiRecorder rec;
	h.Draw(rec);
	const auto& cmds = rec.Commands();
	NOX_REQUIRE(cmds.size() == 10u);
	NOX_CHECK(cmds[0].op == NoxLua::UiOp::RectFilled);
	NOX_CHECK(cmds[0].a == glm::vec2(10.0f, 20.0f));
	NOX_CHECK(cmds[0].b == glm::vec2(110.0f, 50.0f));
	NOX_CHECK(cmds[0].color == 0xFF302010u);
	NOX_CHECK(cmds[0].rounding == 4.0f);
	NOX_CHECK(cmds[1].op == NoxLua::UiOp::Line);
	NOX_CHECK(cmds[1].color == Col(255, 0, 0, 128));
	NOX_CHECK(cmds[1].thickness == 2.0f);
	NOX_CHECK(cmds[2].op == NoxLua::UiOp::Polyline);
	NOX_CHECK(cmds[2].closed);
	NOX_CHECK_EQ(cmds[2].pointCount, 3u);
	NOX_CHECK(cmds[3].op == NoxLua::UiOp::PolygonFilled);
	NOX_CHECK_EQ(cmds[3].pointCount, 3u);
	NOX_CHECK(cmds[4].op == NoxLua::UiOp::Circle);
	NOX_CHECK(cmds[4].b.x == 12.0f);
	NOX_CHECK(cmds[5].op == NoxLua::UiOp::Polyline);
	NOX_CHECK(!cmds[5].closed);
	NOX_CHECK(cmds[5].pointCount >= 2u);
	NOX_CHECK(cmds[6].op == NoxLua::UiOp::ClipPush);
	NOX_CHECK(cmds[7].op == NoxLua::UiOp::Text);
	NOX_CHECK(cmds[7].text == "hello");
	NOX_CHECK(cmds[7].font.empty());
	NOX_CHECK(cmds[7].halign == NoxLua::UiAlign::Center);
	NOX_CHECK(cmds[7].valign == NoxLua::UiValign::Middle);
	NOX_CHECK(cmds[8].op == NoxLua::UiOp::ClipPop);
	NOX_CHECK(cmds[9].op == NoxLua::UiOp::Image);
	NOX_CHECK(cmds[9].text == "hud/speed");
	NOX_CHECK(cmds[9].color == Col(255, 255, 255, 255));
	NOX_CHECK_EQ(rec.ClipDepth(), 0u);
	NOX_CHECK(h.DiagnosticCount() == 0);

	// Points of the arc and polygon are stored in the shared pool, in order.
	const auto& pts = rec.Points();
	NOX_CHECK(pts[cmds[3].pointStart + 2] == glm::vec2(5.0f, 8.0f));
}

NOX_TEST(LuaUi, DrawErrorFaultsTheHandler) {
	Harness h;
	NOX_REQUIRE(h.Load("nox.on_draw(function(ctx, ui, dt) ui.clip_pop() end)\n"));
	NoxLua::UiRecorder rec;
	h.Draw(rec);
	NOX_CHECK(rec.Commands().empty());
	NOX_CHECK_EQ(h.DiagnosticCount(), 1u);
	NOX_CHECK(h.world.Diagnostics()[0].source == "draw handler #1");
	NOX_CHECK(h.world.Diagnostics()[0].message.find("clip_pop without a matching clip_push") != std::string::npos);
	// Faulted handlers are skipped until a reload or RetryFaulted.
	h.Draw(rec);
	NOX_CHECK_EQ(h.DiagnosticCount(), 1u);
}

NOX_TEST(LuaUi, InvalidArgumentsAreRejected) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'probe', update = function(ctx, dt)\n"
		"  ran = true\n"
		"  ok_outside = pcall(kept_ui.rect_filled, 0, 0, 1, 1, 0xFFFFFFFF)\n"
		"end }\n"
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  ok_color = pcall(ui.rect, 0, 0, 1, 1, {r=256, g=0, b=0})\n"
		"  ok_color_missing = pcall(ui.rect, 0, 0, 1, 1, {r=1, g=0})\n"
		"  ok_polyline = pcall(ui.polyline, {0, 0}, 0xFFFFFFFF)\n"
		"  ok_odd = pcall(ui.polyline, {0, 0, 1}, 0xFFFFFFFF)\n"
		"  ok_circle = pcall(ui.circle, 0, 0, 0, 0xFFFFFFFF)\n"
		"  ok_text = pcall(ui.text, '', string.rep('x', 600), 0, 0, 10, 0xFFFFFFFF)\n"
		"  ok_align = pcall(ui.text, '', 'a', 0, 0, 10, 0xFFFFFFFF, 'diagonal')\n"
		"  ok_clip = pcall(ui.clip_push, 0, 0, 1, 1)\n"
		"  kept_ui = ui\n"
		"end)\n"));
	NoxLua::UiRecorder rec;
	h.Draw(rec);
	NOX_CHECK(!h.GlobalBool("ok_color"));
	NOX_CHECK(!h.GlobalBool("ok_color_missing"));
	NOX_CHECK(!h.GlobalBool("ok_polyline"));
	NOX_CHECK(!h.GlobalBool("ok_odd"));
	NOX_CHECK(!h.GlobalBool("ok_circle"));
	NOX_CHECK(!h.GlobalBool("ok_text"));
	NOX_CHECK(!h.GlobalBool("ok_align"));
	NOX_CHECK(h.GlobalBool("ok_clip"));
	// Drawing with the ui table kept from a draw handler, outside any draw, is refused.
	h.world.BeginPlay();
	h.world.Update(1.0f / 60.0f, true);
	NOX_REQUIRE(h.GlobalBool("ran"));
	NOX_CHECK(!h.GlobalBool("ok_outside"));
}

NOX_TEST(LuaUi, DrawBudgetIsBoundedAndReported) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  for i = 1, 5000 do ui.rect_filled(i, i, 2, 2, 0xFFFFFFFF) end\n"
		"end)\n"));
	NoxLua::UiRecorder rec;
	h.Draw(rec);
	NOX_CHECK_EQ(rec.Commands().size(), NoxLua::ui_limits::kMaxCommands);
	NOX_CHECK_EQ(rec.Dropped(), 5000u - NoxLua::ui_limits::kMaxCommands);
	bool reported = false;
	for (const auto& d : h.world.Diagnostics()) {
		if (d.severity == NoxLua::Diagnostic::Severity::Warning && d.message.find("budget") != std::string::npos) reported = true;
	}
	NOX_CHECK(reported);
	// Next frame: the same warning is not repeated while the budget stays exceeded.
	const size_t count = h.DiagnosticCount();
	h.Draw(rec);
	NOX_CHECK_EQ(h.DiagnosticCount(), count);
}

NOX_TEST(LuaUi, DrawHandlersCannotChangeTheWorld) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.on_draw(function(ctx, ui, dt)\n"
		"  ctx:spawn('draw_probe')\n"
		"  ui.rect_filled(0, 0, 1, 1, 0xFFFFFFFF)\n"
		"end)\n"));
	NoxLua::UiRecorder rec;
	h.Draw(rec);
	NOX_CHECK_EQ(rec.Commands().size(), 1u);
	NOX_CHECK_EQ(h.world.PendingCommandCount(), 0u);
}

NOX_TEST(LuaUi, TextMeasureUsesFontMetrics) {
	ImGuiScope imgui;
	NoxLua::UiFontTable fonts;
	NoxLua::UiFontEntry entry;
	entry.id = "body";
	entry.font = ImGui::GetIO().FontDefault;
	entry.size = 13.0f;
	fonts.Add(entry);

	const glm::vec2 small = NoxLua::MeasureUiText(&fonts, "body", 13.0f, "Hello");
	const glm::vec2 large = NoxLua::MeasureUiText(&fonts, "body", 26.0f, "Hello");
	NOX_CHECK(small.x > 0.0f);
	NOX_CHECK(large.x > small.x);
	NOX_CHECK(large.y > small.y);
	// Unknown ids fall back to the default font; empty text has no width.
	NOX_CHECK(NoxLua::MeasureUiText(&fonts, "missing", 13.0f, "Hello").x > 0.0f);
	NOX_CHECK(NoxLua::MeasureUiText(&fonts, "body", 13.0f, "").x == 0.0f);

	// Through Lua (ui.text_size outside a draw handler is allowed; it needs only the font table).
	Harness h;
	NOX_REQUIRE(h.Load("measured_w, measured_h = 0, 0\n"
		"nox.on_draw(function(ctx, ui, dt) measured_w, measured_h = ui.text_size('', 'Hello', 13) end)\n"));
	NoxLua::UiRecorder rec;
	h.Draw(rec);
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "measured_w");
	NOX_CHECK(lua_tonumber(L, -1) > 0.0);
	lua_pop(L, 1);
}

NOX_TEST(LuaUi, PlaybackEmitsDrawListGeometry) {
	ImGuiScope imgui;
	NoxLua::UiRecorder rec;
	rec.Begin();
	NoxLua::UiCommand fill;
	fill.op = NoxLua::UiOp::RectFilled;
	fill.a = glm::vec2(0.0f);
	fill.b = glm::vec2(10.0f);
	fill.color = 0xFFFFFFFFu;
	rec.Add(fill);
	NoxLua::UiCommand text;
	text.op = NoxLua::UiOp::Text;
	text.text = "hi";
	text.size = 14.0f;
	text.color = 0xFFFFFFFFu;
	rec.Add(text);
	rec.PushClip(glm::vec2(0.0f), glm::vec2(5.0f));   // unbalanced on purpose: playback must close it

	ImDrawList list(ImGui::GetDrawListSharedData());
	list._ResetForNewFrame();   // what ImGui does per frame: a list needs its first command before clip changes
	NoxLua::PlaybackUi(rec, nullptr, &list, glm::vec2(4.0f, 4.0f));
	NOX_CHECK(list.VtxBuffer.Size > 0);
	NOX_CHECK(list.CmdBuffer.Size >= 1);
	// The unbalanced clip was closed: the draw list's clip stack is back to the base.
	NOX_CHECK_EQ(list._ClipRectStack.Size, 0);
}
