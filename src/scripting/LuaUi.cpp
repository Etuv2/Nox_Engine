#include "LuaUi.h"

#include "LuaData.h"
#include "LuaUiFocus.h"
#include "../Texture.h"
#include "../assets/AssetIO.h"
#include "../UiIconAtlas.h"
#include "../rhi/RHI.h"
#include "../ui/Pointer.h"
#include "lauxlib.h"
#include "lua.h"

#include <IMGUI/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace NoxLua {

	namespace {
		constexpr const char* kStateKey = "nox.ui.state";

		void MinMax(glm::vec2 a, glm::vec2 b, glm::vec2& min, glm::vec2& max) {
			min = glm::vec2(std::min(a.x, b.x), std::min(a.y, b.y));
			max = glm::vec2(std::max(a.x, b.x), std::max(a.y, b.y));
		}

		ImFont* DefaultFont() {
			ImGuiIO& io = ImGui::GetIO();
			if (io.FontDefault) return io.FontDefault;
			return io.Fonts && !io.Fonts->Fonts.empty() ? io.Fonts->Fonts[0] : nullptr;
		}

		ImFont* FontFor(const UiFontTable* fonts, const std::string& id) {
			if (fonts && !id.empty()) {
				if (const UiFontEntry* entry = fonts->Find(id)) return entry->font ? entry->font : DefaultFont();
			}
			return DefaultFont();
		}
	}

	// ---- UiRecorder -------------------------------------------------------------------------------

	void UiRecorder::Begin() {
		m_commands.clear();
		m_points.clear();
		m_clipDepth = 0;
		m_dropped = 0;
	}

	bool UiRecorder::Add(UiCommand command) {
		if (m_commands.size() >= ui_limits::kMaxCommands) {
			++m_dropped;
			return false;
		}
		m_commands.push_back(std::move(command));
		return true;
	}

	bool UiRecorder::AddPoints(UiCommand command, const std::vector<glm::vec2>& points) {
		if (m_commands.size() >= ui_limits::kMaxCommands || m_points.size() + points.size() > ui_limits::kMaxPoints) {
			++m_dropped;
			return false;
		}
		command.pointStart = static_cast<uint32_t>(m_points.size());
		command.pointCount = static_cast<uint32_t>(points.size());
		m_points.insert(m_points.end(), points.begin(), points.end());
		m_commands.push_back(std::move(command));
		return true;
	}

	bool UiRecorder::PushClip(const glm::vec2& min, const glm::vec2& max) {
		if (m_clipDepth >= ui_limits::kMaxClipDepth) {
			++m_dropped;
			return false;
		}
		UiCommand c;
		c.op = UiOp::ClipPush;
		c.a = min;
		c.b = max;
		if (!Add(std::move(c))) return false;
		++m_clipDepth;
		return true;
	}

	bool UiRecorder::PopClip() {
		if (m_clipDepth == 0) return false;
		UiCommand c;
		c.op = UiOp::ClipPop;
		if (!Add(std::move(c))) return false;
		--m_clipDepth;
		return true;
	}

	// ---- Fonts ------------------------------------------------------------------------------------

	void UiFontTable::Add(UiFontEntry entry) {
		for (UiFontEntry& existing : m_entries) {
			if (existing.id == entry.id) {
				existing = std::move(entry);
				return;
			}
		}
		m_entries.push_back(std::move(entry));
	}

	const UiFontEntry* UiFontTable::Find(const std::string& id) const {
		for (const UiFontEntry& entry : m_entries) {
			if (entry.id == id) return &entry;
		}
		return nullptr;
	}

	void LoadUiFonts(const std::vector<UiFontDecl>& decls, UiFontTable& out, std::vector<std::string>& messages) {
		ImGuiIO& io = ImGui::GetIO();
		for (const UiFontDecl& decl : decls) {
			ImFont* font = nullptr;
			std::string error;
			if (AssetIO::Buffer bytes = AssetIO::ReadAll(decl.path, &error)) {
				if (bytes->empty() || bytes->size() > static_cast<size_t>(INT32_MAX)) {
					error = "font file is empty or too large";
				}
				else {
					// ImGui frees the atlas-owned copy (IM_FREE); the AssetIO buffer does not outlive this call.
					void* copy = IM_ALLOC(bytes->size());
					std::memcpy(copy, bytes->data(), bytes->size());
					font = io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(bytes->size()), decl.size);
					if (!font) error = "ImGui rejected the font file";
				}
			}
			if (!font) {
				messages.push_back("font '" + decl.id + "' (" + decl.path + "): " + error + "; using the default font");
				font = DefaultFont();
			}
			UiFontEntry entry;
			entry.id = decl.id;
			entry.font = font;
			entry.size = decl.size;
			out.Add(std::move(entry));
		}
	}

	glm::vec2 MeasureUiText(const UiFontTable* fonts, const std::string& fontId, float size, const std::string& text) {
		ImFont* font = FontFor(fonts, fontId);
		if (!font || size <= 0.0f) return glm::vec2(0.0f);
		const ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
		return glm::vec2(s.x, s.y);
	}

	// ---- Lua binding ------------------------------------------------------------------------------

	namespace {
		UiState* StateOf(lua_State* L) {
			lua_getfield(L, LUA_REGISTRYINDEX, kStateKey);
			UiState* state = static_cast<UiState*>(lua_touserdata(L, -1));
			lua_pop(L, 1);
			return state;
		}

		UiRecorder& Recorder(lua_State* L) {
			UiState* state = StateOf(L);
			if (!state || !state->recorder) {
				luaL_error(L, "ui drawing is only available inside nox.on_draw handlers");
			}
			return *state->recorder;
		}

		// Color: 0xAARRGGBB integer, or {r=, g=, b=, a=} with 0..255 integers (a defaults to 255).
		uint32_t CheckColor(lua_State* L, int idx) {
			if (lua_type(L, idx) == LUA_TNUMBER) {
				const lua_Integer v = luaL_checkinteger(L, idx);
				if (v < 0 || v > 0xFFFFFFFFLL) {
					luaL_error(L, "color integer must be 0x00000000..0xFFFFFFFF (0xAARRGGBB)");
				}
				const uint32_t c = static_cast<uint32_t>(v);
				return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
			}
			if (lua_type(L, idx) != LUA_TTABLE) {
				luaL_error(L, "color must be 0xAARRGGBB or a table {r=, g=, b=, a=}");
			}
			int channels[4] = { 0, 0, 0, 255 };
			const char* names[4] = { "r", "g", "b", "a" };
			for (int i = 0; i < 4; ++i) {
				lua_getfield(L, idx, names[i]);
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					if (i == 3) continue;
					luaL_error(L, "color table needs r, g and b");
				}
				if (!lua_isinteger(L, -1)) {
					luaL_error(L, "color channel %s must be an integer 0..255", names[i]);
				}
				const lua_Integer v = lua_tointeger(L, -1);
				lua_pop(L, 1);
				if (v < 0 || v > 255) luaL_error(L, "color channel %s must be 0..255", names[i]);
				channels[i] = static_cast<int>(v);
			}
			return IM_COL32(channels[0], channels[1], channels[2], channels[3]);
		}

		// Points: flat {x1, y1, x2, y2, ...} or {{x, y}, {x, y}, ...}.
		void CheckPoints(lua_State* L, int idx, size_t minCount, std::vector<glm::vec2>& out) {
			luaL_checktype(L, idx, LUA_TTABLE);
			const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
			if (n < 1) luaL_error(L, "point list is empty");
			lua_rawgeti(L, idx, 1);
			const bool nested = lua_istable(L, -1);
			lua_pop(L, 1);
			out.clear();
			if (nested) {
				if (static_cast<size_t>(n) > ui_limits::kMaxPointsPerCall) {
					luaL_error(L, "point list has more than %d points", static_cast<int>(ui_limits::kMaxPointsPerCall));
				}
				for (lua_Integer i = 1; i <= n; ++i) {
					lua_rawgeti(L, idx, i);
					if (!lua_istable(L, -1)) luaL_error(L, "point %d is not a {x, y} table", static_cast<int>(i));
					lua_rawgeti(L, -1, 1);
					const float x = static_cast<float>(luaL_checknumber(L, -1));
					lua_pop(L, 1);
					lua_rawgeti(L, -1, 2);
					const float y = static_cast<float>(luaL_checknumber(L, -1));
					lua_pop(L, 2);
					out.emplace_back(x, y);
				}
			}
			else {
				if (n % 2 != 0) luaL_error(L, "flat point list needs an even count of numbers");
				if (static_cast<size_t>(n / 2) > ui_limits::kMaxPointsPerCall) {
					luaL_error(L, "point list has more than %d points", static_cast<int>(ui_limits::kMaxPointsPerCall));
				}
				for (lua_Integer i = 1; i <= n; i += 2) {
					lua_rawgeti(L, idx, i);
					const float x = static_cast<float>(luaL_checknumber(L, -1));
					lua_pop(L, 1);
					lua_rawgeti(L, idx, i + 1);
					const float y = static_cast<float>(luaL_checknumber(L, -1));
					lua_pop(L, 1);
					out.emplace_back(x, y);
				}
			}
			if (out.size() < minCount) luaL_error(L, "this call needs at least %d points", static_cast<int>(minCount));
		}

		UiAlign ParseHAlign(lua_State* L, int idx) {
			if (lua_isnoneornil(L, idx)) return UiAlign::Left;
			const char* s = luaL_checkstring(L, idx);
			if (std::strcmp(s, "left") == 0) return UiAlign::Left;
			if (std::strcmp(s, "center") == 0) return UiAlign::Center;
			if (std::strcmp(s, "right") == 0) return UiAlign::Right;
			luaL_error(L, "horizontal align must be left, center or right");
			return UiAlign::Left;
		}

		UiValign ParseVAlign(lua_State* L, int idx) {
			if (lua_isnoneornil(L, idx)) return UiValign::Top;
			const char* s = luaL_checkstring(L, idx);
			if (std::strcmp(s, "top") == 0) return UiValign::Top;
			if (std::strcmp(s, "middle") == 0) return UiValign::Middle;
			if (std::strcmp(s, "bottom") == 0) return UiValign::Bottom;
			luaL_error(L, "vertical align must be top, middle or bottom");
			return UiValign::Top;
		}

		float OptNumber(lua_State* L, int idx, float fallback) {
			return lua_isnoneornil(L, idx) ? fallback : static_cast<float>(luaL_checknumber(L, idx));
		}

		// rect(x, y, w, h, color, thickness?, rounding?)
		int UiRect(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
			const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
			UiCommand c;
			c.op = UiOp::Rect;
			MinMax({ x, y }, { x + w, y + h }, c.a, c.b);
			c.color = CheckColor(L, 5);
			c.thickness = OptNumber(L, 6, 1.0f);
			c.rounding = OptNumber(L, 7, 0.0f);
			rec.Add(std::move(c));
			return 0;
		}

		// rect_filled(x, y, w, h, color, rounding?)
		int UiRectFilled(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
			const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
			UiCommand c;
			c.op = UiOp::RectFilled;
			MinMax({ x, y }, { x + w, y + h }, c.a, c.b);
			c.color = CheckColor(L, 5);
			c.rounding = OptNumber(L, 6, 0.0f);
			rec.Add(std::move(c));
			return 0;
		}

		// line(x1, y1, x2, y2, color, thickness?)
		int UiLine(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			UiCommand c;
			c.op = UiOp::Line;
			c.a = glm::vec2(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
			c.b = glm::vec2(static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)));
			c.color = CheckColor(L, 5);
			c.thickness = OptNumber(L, 6, 1.0f);
			rec.Add(std::move(c));
			return 0;
		}

		// polyline(points, color, thickness?, closed?)
		int UiPolyline(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			std::vector<glm::vec2> points;
			CheckPoints(L, 1, 2, points);
			UiCommand c;
			c.op = UiOp::Polyline;
			c.color = CheckColor(L, 2);
			c.thickness = OptNumber(L, 3, 1.0f);
			c.closed = !lua_isnoneornil(L, 4) && lua_toboolean(L, 4) != 0;
			rec.AddPoints(std::move(c), points);
			return 0;
		}

		// polygon_filled(points, color): convex or concave simple polygon (ImGui AddConcavePolyFilled).
		int UiPolygonFilled(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			std::vector<glm::vec2> points;
			CheckPoints(L, 1, 3, points);
			UiCommand c;
			c.op = UiOp::PolygonFilled;
			c.color = CheckColor(L, 2);
			rec.AddPoints(std::move(c), points);
			return 0;
		}

		// circle(cx, cy, r, color, thickness?)
		int UiCircle(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float r = static_cast<float>(luaL_checknumber(L, 3));
			if (!(r > 0.0f)) luaL_error(L, "circle radius must be positive");
			UiCommand c;
			c.op = UiOp::Circle;
			c.a = glm::vec2(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
			c.b = glm::vec2(r, 0.0f);
			c.color = CheckColor(L, 4);
			c.thickness = OptNumber(L, 5, 1.0f);
			rec.Add(std::move(c));
			return 0;
		}

		// circle_filled(cx, cy, r, color)
		int UiCircleFilled(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float r = static_cast<float>(luaL_checknumber(L, 3));
			if (!(r > 0.0f)) luaL_error(L, "circle radius must be positive");
			UiCommand c;
			c.op = UiOp::CircleFilled;
			c.a = glm::vec2(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
			c.b = glm::vec2(r, 0.0f);
			c.color = CheckColor(L, 4);
			rec.Add(std::move(c));
			return 0;
		}

		// arc(cx, cy, r, a0, a1, color, thickness?): radians (0 = +x, positive towards +y); recorded as an open polyline.
		int UiArc(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float cx = static_cast<float>(luaL_checknumber(L, 1)), cy = static_cast<float>(luaL_checknumber(L, 2));
			const float r = static_cast<float>(luaL_checknumber(L, 3));
			const float a0 = static_cast<float>(luaL_checknumber(L, 4)), a1 = static_cast<float>(luaL_checknumber(L, 5));
			if (!(r > 0.0f)) luaL_error(L, "arc radius must be positive");
			const float span = std::fabs(a1 - a0);
			const float wanted = std::ceil(span * r / 4.0f) + 2.0f;
			const int segments = static_cast<int>(std::clamp(wanted, 2.0f, static_cast<float>(ui_limits::kMaxArcSegments)));
			std::vector<glm::vec2> points;
			points.reserve(static_cast<size_t>(segments));
			for (int i = 0; i < segments; ++i) {
				const float t = static_cast<float>(i) / static_cast<float>(segments - 1);
				const float angle = a0 + (a1 - a0) * t;
				points.emplace_back(cx + r * std::cos(angle), cy + r * std::sin(angle));
			}
			UiCommand c;
			c.op = UiOp::Polyline;
			c.color = CheckColor(L, 6);
			c.thickness = OptNumber(L, 7, 1.0f);
			rec.AddPoints(std::move(c), points);
			return 0;
		}

		// text(font, text, x, y, size, color, halign?, valign?)
		int UiText(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			UiCommand c;
			c.op = UiOp::Text;
			c.font = lua_isnoneornil(L, 1) ? std::string() : std::string(luaL_checkstring(L, 1));
			size_t len = 0;
			const char* text = luaL_checklstring(L, 2, &len);
			if (len > ui_limits::kMaxTextBytes) luaL_error(L, "text is longer than %d bytes", static_cast<int>(ui_limits::kMaxTextBytes));
			c.text.assign(text, len);
			c.a = glm::vec2(static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)));
			c.size = static_cast<float>(luaL_checknumber(L, 5));
			if (!(c.size > 0.0f)) luaL_error(L, "text size must be positive");
			c.color = CheckColor(L, 6);
			c.halign = ParseHAlign(L, 7);
			c.valign = ParseVAlign(L, 8);
			rec.Add(std::move(c));
			return 0;
		}

		// text_size(font, text, size) -> width, height. Needs only the font table, so it is valid outside draw handlers.
		int UiTextSize(lua_State* L) {
			UiState* state = StateOf(L);
			const std::string font = lua_isnoneornil(L, 1) ? std::string() : std::string(luaL_checkstring(L, 1));
			size_t len = 0;
			const char* text = luaL_checklstring(L, 2, &len);
			if (len > ui_limits::kMaxTextBytes) luaL_error(L, "text is longer than %d bytes", static_cast<int>(ui_limits::kMaxTextBytes));
			const float size = static_cast<float>(luaL_checknumber(L, 3));
			const glm::vec2 s = MeasureUiText(state ? state->fonts : nullptr, font, size, std::string(text, len));
			lua_pushnumber(L, s.x);
			lua_pushnumber(L, s.y);
			return 2;
		}

		// image(icon_id, x, y, w, h, tint?)
		int UiImage(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			UiCommand c;
			c.op = UiOp::Image;
			c.text = std::string(luaL_checkstring(L, 1));
			const float x = static_cast<float>(luaL_checknumber(L, 2)), y = static_cast<float>(luaL_checknumber(L, 3));
			const float w = static_cast<float>(luaL_checknumber(L, 4)), h = static_cast<float>(luaL_checknumber(L, 5));
			MinMax({ x, y }, { x + w, y + h }, c.a, c.b);
			c.color = lua_isnoneornil(L, 6) ? IM_COL32_WHITE : CheckColor(L, 6);
			rec.Add(std::move(c));
			return 0;
		}

		// clip_push(x, y, w, h)
		int UiClipPush(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
			const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
			glm::vec2 min, max;
			MinMax({ x, y }, { x + w, y + h }, min, max);
			if (!rec.PushClip(min, max)) luaL_error(L, "clip stack is full (max %d)", static_cast<int>(ui_limits::kMaxClipDepth));
			return 0;
		}

		int UiClipPop(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			if (!rec.PopClip()) luaL_error(L, "clip_pop without a matching clip_push");
			return 0;
		}

		int UiViewport(lua_State* L) {
			UiState* state = StateOf(L);
			lua_pushnumber(L, state ? state->viewport.x : 0.0);
			lua_pushnumber(L, state ? state->viewport.y : 0.0);
			return 2;
		}

		// pointer() -> x, y relative to the overlay (valid while drawing).
		int UiPointer(lua_State* L) {
			UiState* state = StateOf(L);
			lua_pushnumber(L, state ? state->pointerOverlay.x : 0.0);
			lua_pushnumber(L, state ? state->pointerOverlay.y : 0.0);
			return 2;
		}

		// icon_exists(id) -> bool: the id is in the project's icon atlas (false when no atlas is loaded).
		int UiIconExists(lua_State* L) {
			const std::string id = luaL_checkstring(L, 1);
			lua_pushboolean(L, UiIconAtlas::Get().Has(id) ? 1 : 0);
			return 1;
		}

		// ---- Widgets (ui.button, ui.toggle, ui.slider, ui.label, ui.panel) ----------------------------------------
		// Every draw a widget registers a window-space hit region (ui::Pointer) and a navigation entry (UiNavigator).
		// Its state (hover, press, focus, click) comes from those two, so one code path serves mouse, keyboard and pad.

		// Style colour defaults are written as 0xAARRGGBB (like Lua colours) and stored in the packed IM_COL32 order.
		constexpr uint32_t Argb(uint32_t c) {
			return IM_COL32((c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu, (c >> 24) & 0xFFu);
		}

		constexpr int kWidgetZ = 100;   // above GUI nodes and other pointer regions
		constexpr float kFocusRingWidth = 2.0f;

		struct WidgetStyle {
			uint32_t bg = Argb(0xE61E2A38u);
			uint32_t hover = Argb(0xF0304256u);
			uint32_t active = Argb(0xF0162330u);
			uint32_t text = Argb(0xFFEEF3F8u);
			uint32_t textDisabled = Argb(0xFF6E7B8Au);
			uint32_t focus = Argb(0xFF4FC3F7u);
			uint32_t accent = Argb(0xFF4FC3F7u);
			uint32_t track = Argb(0xFF3A4756u);
			uint32_t knob = Argb(0xFFFFFFFFu);
			uint32_t disabled = Argb(0xB0202830u);
			uint32_t border = Argb(0x00000000u);
			float rounding = 6.0f;
			float size = 22.0f;
			std::string font;
		};

		struct PanelStyle {
			uint32_t bg = Argb(0xD0141C26u);
			uint32_t border = Argb(0xFF2E3A48u);
			float rounding = 8.0f;
		};

		// Optional fields of the table at absolute index t.
		bool OptNumberField(lua_State* L, int t, const char* name, float& out) {
			lua_getfield(L, t, name);
			const bool present = !lua_isnil(L, -1);
			if (present) {
				if (!lua_isnumber(L, -1)) luaL_error(L, "'%s' must be a number", name);
				out = static_cast<float>(lua_tonumber(L, -1));
			}
			lua_pop(L, 1);
			return present;
		}

		bool OptColorField(lua_State* L, int t, const char* name, uint32_t& out) {
			lua_getfield(L, t, name);
			const bool present = !lua_isnil(L, -1);
			if (present) out = CheckColor(L, lua_gettop(L));
			lua_pop(L, 1);
			return present;
		}

		bool OptStringField(lua_State* L, int t, const char* name, std::string& out) {
			lua_getfield(L, t, name);
			const bool present = !lua_isnil(L, -1);
			if (present) out = luaL_checkstring(L, -1);
			lua_pop(L, 1);
			return present;
		}

		bool OptBoolField(lua_State* L, int t, const char* name, bool& out) {
			lua_getfield(L, t, name);
			const bool present = !lua_isnil(L, -1);
			if (present) {
				if (!lua_isboolean(L, -1)) luaL_error(L, "'%s' must be a boolean", name);
				out = lua_toboolean(L, -1) != 0;
			}
			lua_pop(L, 1);
			return present;
		}

		// Style table (optional): colours, rounding, size, font. Unknown keys are ignored.
		WidgetStyle ReadWidgetStyle(lua_State* L, int idx) {
			WidgetStyle s;
			if (lua_isnoneornil(L, idx)) return s;
			luaL_checktype(L, idx, LUA_TTABLE);
			const int t = lua_absindex(L, idx);
			OptColorField(L, t, "bg", s.bg);
			OptColorField(L, t, "hover", s.hover);
			OptColorField(L, t, "active", s.active);
			OptColorField(L, t, "text", s.text);
			OptColorField(L, t, "text_disabled", s.textDisabled);
			OptColorField(L, t, "focus", s.focus);
			OptColorField(L, t, "accent", s.accent);
			OptColorField(L, t, "track", s.track);
			OptColorField(L, t, "knob", s.knob);
			OptColorField(L, t, "disabled", s.disabled);
			OptColorField(L, t, "border", s.border);
			OptNumberField(L, t, "rounding", s.rounding);
			OptNumberField(L, t, "size", s.size);
			OptStringField(L, t, "font", s.font);
			if (!(s.size > 0.0f)) luaL_error(L, "style size must be positive");
			return s;
		}

		PanelStyle ReadPanelStyle(lua_State* L, int idx) {
			PanelStyle s;
			if (lua_isnoneornil(L, idx)) return s;
			luaL_checktype(L, idx, LUA_TTABLE);
			const int t = lua_absindex(L, idx);
			OptColorField(L, t, "bg", s.bg);
			OptColorField(L, t, "border", s.border);
			OptNumberField(L, t, "rounding", s.rounding);
			return s;
		}

		// Rectangle argument: {x=, y=, w=, h=} or {x, y, w, h}.
		glm::vec4 RectArg(lua_State* L, int idx) {
			luaL_checktype(L, idx, LUA_TTABLE);
			const int t = lua_absindex(L, idx);
			const char* names[4] = { "x", "y", "w", "h" };
			float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < 4; ++i) {
				lua_getfield(L, t, names[i]);
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_rawgeti(L, t, i + 1);
				}
				if (!lua_isnumber(L, -1)) luaL_error(L, "rect needs numeric x, y, w and h");
				v[i] = static_cast<float>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			return glm::vec4(v[0], v[1], v[2], v[3]);
		}

		uint32_t IdArg(lua_State* L, int idx) {
			size_t len = 0;
			const char* s = luaL_checklstring(L, idx, &len);
			if (len == 0 || len > 128) luaL_error(L, "widget id must be 1..128 characters");
			return ui::HashId(s);
		}

		UiNavigator& NavOf(lua_State* L) {
			UiState* state = StateOf(L);
			if (!state || !state->nav) luaL_error(L, "ui widgets are only available inside nox.on_draw handlers");
			return *state->nav;
		}

		void AddRectCmd(UiRecorder& rec, UiOp op, const glm::vec2& min, const glm::vec2& max, uint32_t color,
			float rounding, float thickness) {
			UiCommand c;
			c.op = op;
			c.a = min;
			c.b = max;
			c.color = color;
			c.rounding = rounding;
			c.thickness = thickness;
			rec.Add(std::move(c));
		}

		void AddTextCmd(UiRecorder& rec, const std::string& font, const std::string& text, const glm::vec2& pos,
			float size, uint32_t color, UiAlign halign, UiValign valign) {
			UiCommand c;
			c.op = UiOp::Text;
			c.font = font;
			c.text = text;
			c.a = pos;
			c.size = size;
			c.color = color;
			c.halign = halign;
			c.valign = valign;
			rec.Add(std::move(c));
		}

		// State of one widget this frame. min/max are overlay coordinates (drawing).
		struct WidgetFrame {
			glm::vec2 min{ 0.0f }, max{ 0.0f };
			bool enabled = true;
			bool hovered = false, pressed = false, focused = false, clicked = false;
			int step = 0;   // left/right this frame, focused value widgets only
		};

		WidgetFrame BeginWidget(lua_State* L, uint32_t id, const glm::vec4& r, bool enabled, bool value) {
			UiState* state = StateOf(L);
			UiNavigator& nav = NavOf(L);
			WidgetFrame f;
			f.enabled = enabled;
			f.min = glm::vec2(r.x, r.y);
			f.max = glm::max(f.min + glm::vec2(r.z, r.w), f.min);
			const glm::vec2 windowMin = f.min + state->origin;
			const glm::vec2 windowMax = f.max + state->origin;
			ui::Pointer& pointer = ui::Pointer::Get();
			pointer.AddRect(id, windowMin, windowMax, kWidgetZ, ui::RegionAll);
			UiNavigator::Widget w;
			w.id = id;
			w.min = windowMin;
			w.max = windowMax;
			w.navigable = enabled;
			w.value = value;
			nav.AddWidget(w);
			f.hovered = enabled && pointer.IsHovered(id);
			f.pressed = enabled && pointer.IsActive(id) && pointer.Down(ui::PointerButton::Left);
			f.focused = enabled && nav.IsFocused(id);
			f.clicked = enabled && (pointer.IsClicked(id) || nav.Accept(id));
			f.step = enabled ? nav.ValueStep(id) : 0;
			return f;
		}

		void DrawFocusRing(UiRecorder& rec, const WidgetFrame& f, const WidgetStyle& s) {
			if (!f.focused) return;
			const glm::vec2 pad(kFocusRingWidth + 1.0f);
			AddRectCmd(rec, UiOp::Rect, f.min - pad, f.max + pad, s.focus, s.rounding + kFocusRingWidth, kFocusRingWidth);
		}

		// button(id, rect, label, style?) -> clicked. Clicked by a mouse press and release on it, or by Enter, Space or
		// pad A while it has keyboard focus. style.enabled = false dims it and takes it out of navigation.
		int UiButton(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const uint32_t id = IdArg(L, 1);
			const glm::vec4 r = RectArg(L, 2);
			const std::string label = lua_isnoneornil(L, 3) ? std::string() : std::string(luaL_checkstring(L, 3));
			const WidgetStyle s = ReadWidgetStyle(L, 4);
			bool enabled = true;
			if (!lua_isnoneornil(L, 4)) OptBoolField(L, lua_absindex(L, 4), "enabled", enabled);
			const WidgetFrame f = BeginWidget(L, id, r, enabled, false);
			const uint32_t bg = !f.enabled ? s.disabled : f.pressed ? s.active : (f.hovered || f.focused) ? s.hover : s.bg;
			AddRectCmd(rec, UiOp::RectFilled, f.min, f.max, bg, s.rounding, 1.0f);
			if ((s.border >> 24) != 0) AddRectCmd(rec, UiOp::Rect, f.min, f.max, s.border, s.rounding, 1.0f);
			DrawFocusRing(rec, f, s);
			if (!label.empty()) {
				AddTextCmd(rec, s.font, label, (f.min + f.max) * 0.5f, s.size, f.enabled ? s.text : s.textDisabled,
					UiAlign::Center, UiValign::Middle);
			}
			lua_pushboolean(L, f.clicked ? 1 : 0);
			return 1;
		}

		// toggle(id, rect, label, value, style?) -> value, changed. A box and a label; click, Enter, or left/right flips it.
		int UiToggle(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const uint32_t id = IdArg(L, 1);
			const glm::vec4 r = RectArg(L, 2);
			const std::string label = lua_isnoneornil(L, 3) ? std::string() : std::string(luaL_checkstring(L, 3));
			const bool value = lua_toboolean(L, 4) != 0;
			const WidgetStyle s = ReadWidgetStyle(L, 5);
			bool enabled = true;
			if (!lua_isnoneornil(L, 5)) OptBoolField(L, lua_absindex(L, 5), "enabled", enabled);
			const WidgetFrame f = BeginWidget(L, id, r, enabled, true);
			bool next = value;
			if (f.clicked) next = !value;
			else if (f.step != 0) next = f.step > 0;
			const float height = f.max.y - f.min.y;
			const float side = std::max(0.0f, std::min(height, 28.0f) - 6.0f);
			const float boxY = f.min.y + (height - side) * 0.5f;
			const glm::vec2 boxMin(f.min.x + 6.0f, boxY), boxMax(boxMin.x + side, boxY + side);
			AddRectCmd(rec, UiOp::RectFilled, f.min, f.max, f.hovered || f.focused ? s.hover : s.bg, s.rounding, 1.0f);
			AddRectCmd(rec, UiOp::RectFilled, boxMin, boxMax, next ? s.accent : s.track, 3.0f, 1.0f);
			if (next) {
				const glm::vec2 inset(side * 0.28f);
				AddRectCmd(rec, UiOp::RectFilled, boxMin + inset, boxMax - inset, s.text, 1.0f, 1.0f);
			}
			DrawFocusRing(rec, f, s);
			if (!label.empty()) {
				AddTextCmd(rec, s.font, label, glm::vec2(boxMax.x + 10.0f, (f.min.y + f.max.y) * 0.5f), s.size,
					f.enabled ? s.text : s.textDisabled, UiAlign::Left, UiValign::Middle);
			}
			lua_pushboolean(L, next ? 1 : 0);
			lua_pushboolean(L, next != value ? 1 : 0);
			return 2;
		}

		// slider(id, rect, value, min, max, opts?) -> value, changed. opts: step, decimals, show_value (default true),
		// enabled, and the colour keys of a style. The mouse drags it; left and right move it by step (default range / 20).
		int UiSlider(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			UiState* state = StateOf(L);
			const uint32_t id = IdArg(L, 1);
			const glm::vec4 r = RectArg(L, 2);
			const float value = static_cast<float>(luaL_checknumber(L, 3));
			const float lo = static_cast<float>(luaL_checknumber(L, 4));
			const float hi = static_cast<float>(luaL_checknumber(L, 5));
			if (!(hi > lo)) luaL_error(L, "slider max must be greater than min");
			float step = 0.0f;
			int decimals = 0;
			bool showValue = true;
			bool enabled = true;
			WidgetStyle s;
			if (!lua_isnoneornil(L, 6)) {
				luaL_checktype(L, 6, LUA_TTABLE);
				const int t = lua_absindex(L, 6);
				OptNumberField(L, t, "step", step);
				float dec = 0.0f;
				if (OptNumberField(L, t, "decimals", dec)) decimals = static_cast<int>(std::clamp(dec, 0.0f, 6.0f));
				OptBoolField(L, t, "show_value", showValue);
				OptBoolField(L, t, "enabled", enabled);
				s = ReadWidgetStyle(L, 6);
			}
			if (step < 0.0f) luaL_error(L, "slider step must not be negative");
			if (step == 0.0f) step = (hi - lo) / 20.0f;
			const WidgetFrame f = BeginWidget(L, id, r, enabled, true);

			const float height = f.max.y - f.min.y;
			std::string text;
			float textWidth = 0.0f;
			if (showValue) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
				text = buf;
				textWidth = MeasureUiText(state->fonts, s.font, s.size, text).x + 12.0f;
			}
			const float trackLeft = f.min.x + 6.0f;
			const float trackRight = std::max(trackLeft + 1.0f, f.max.x - textWidth - 6.0f);
			const float trackWidth = trackRight - trackLeft;
			float v = std::clamp(value, lo, hi);
			if (f.pressed) {
				const float px = ui::Pointer::Get().Position().x - state->origin.x;
				v = lo + std::clamp((px - trackLeft) / trackWidth, 0.0f, 1.0f) * (hi - lo);
			}
			else if (f.step != 0) {
				v += static_cast<float>(f.step) * step;
			}
			if (f.pressed || f.step != 0) v = lo + std::round((v - lo) / step) * step;
			v = std::clamp(v, lo, hi);
			const bool changed = v != value;

			const float frac = (v - lo) / (hi - lo);
			const float trackH = std::max(4.0f, height * 0.18f);
			const float cy = (f.min.y + f.max.y) * 0.5f;
			const float knobX = trackLeft + frac * trackWidth;
			AddRectCmd(rec, UiOp::RectFilled, f.min, f.max, f.hovered || f.focused ? s.hover : s.bg, s.rounding, 1.0f);
			AddRectCmd(rec, UiOp::RectFilled, glm::vec2(trackLeft, cy - trackH * 0.5f), glm::vec2(trackRight, cy + trackH * 0.5f),
				s.track, trackH * 0.5f, 1.0f);
			AddRectCmd(rec, UiOp::RectFilled, glm::vec2(trackLeft, cy - trackH * 0.5f), glm::vec2(knobX, cy + trackH * 0.5f),
				f.enabled ? s.accent : s.textDisabled, trackH * 0.5f, 1.0f);
			UiCommand knob;
			knob.op = UiOp::CircleFilled;
			knob.a = glm::vec2(knobX, cy);
			knob.b = glm::vec2(std::max(4.0f, height * 0.3f), 0.0f);
			knob.color = !f.enabled ? s.textDisabled : (f.pressed ? s.accent : s.knob);
			rec.Add(std::move(knob));
			DrawFocusRing(rec, f, s);
			if (showValue) {
				AddTextCmd(rec, s.font, text, glm::vec2(f.max.x - 6.0f, cy), s.size, f.enabled ? s.text : s.textDisabled,
					UiAlign::Right, UiValign::Middle);
			}
			lua_pushnumber(L, v);
			lua_pushboolean(L, changed ? 1 : 0);
			return 2;
		}

		// label(rect, text, opts?): text inside a rectangle. opts: font, size, color, align (left|center|right),
		// valign (top|middle|bottom, default middle).
		int UiLabel(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const glm::vec4 r = RectArg(L, 1);
			size_t len = 0;
			const char* text = luaL_checklstring(L, 2, &len);
			if (len > ui_limits::kMaxTextBytes) luaL_error(L, "text is longer than %d bytes", static_cast<int>(ui_limits::kMaxTextBytes));
			std::string font;
			float size = 22.0f;
			uint32_t color = Argb(0xFFEEF3F8u);
			UiAlign halign = UiAlign::Left;
			UiValign valign = UiValign::Middle;
			if (!lua_isnoneornil(L, 3)) {
				luaL_checktype(L, 3, LUA_TTABLE);
				const int t = lua_absindex(L, 3);
				OptStringField(L, t, "font", font);
				OptNumberField(L, t, "size", size);
				OptColorField(L, t, "color", color);
				std::string align, vert;
				if (OptStringField(L, t, "align", align)) {
					if (align == "center") halign = UiAlign::Center;
					else if (align == "right") halign = UiAlign::Right;
					else if (align != "left") luaL_error(L, "label align must be left, center or right");
				}
				if (OptStringField(L, t, "valign", vert)) {
					if (vert == "top") valign = UiValign::Top;
					else if (vert == "bottom") valign = UiValign::Bottom;
					else if (vert != "middle") luaL_error(L, "label valign must be top, middle or bottom");
				}
			}
			if (!(size > 0.0f)) luaL_error(L, "label size must be positive");
			const glm::vec2 min(r.x, r.y), max = min + glm::vec2(r.z, r.w);
			const glm::vec2 pos(halign == UiAlign::Center ? (min.x + max.x) * 0.5f : halign == UiAlign::Right ? max.x : min.x,
				valign == UiValign::Middle ? (min.y + max.y) * 0.5f : valign == UiValign::Bottom ? max.y : min.y);
			AddTextCmd(rec, font, std::string(text, len), pos, size, color, halign, valign);
			return 0;
		}

		// panel(rect, style?): rounded background (bg, border, rounding).
		int UiPanel(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			const glm::vec4 r = RectArg(L, 1);
			const PanelStyle s = ReadPanelStyle(L, 2);
			const glm::vec2 min(r.x, r.y), max = min + glm::vec2(r.z, r.w);
			AddRectCmd(rec, UiOp::RectFilled, min, max, s.bg, s.rounding, 1.0f);
			if ((s.border >> 24) != 0) AddRectCmd(rec, UiOp::Rect, min, max, s.border, s.rounding, 1.0f);
			return 0;
		}

		// measure_text(text, font, size) -> width, height.
		int UiMeasureText(lua_State* L) {
			UiState* state = StateOf(L);
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			if (len > ui_limits::kMaxTextBytes) luaL_error(L, "text is longer than %d bytes", static_cast<int>(ui_limits::kMaxTextBytes));
			const std::string font = lua_isnoneornil(L, 2) ? std::string() : std::string(luaL_checkstring(L, 2));
			const float size = static_cast<float>(luaL_checknumber(L, 3));
			const glm::vec2 s = MeasureUiText(state ? state->fonts : nullptr, font, size, std::string(text, len));
			lua_pushnumber(L, s.x);
			lua_pushnumber(L, s.y);
			return 2;
		}

		// consumed() -> bool: the menu used this frame's arrow, accept or back keys, or a press landed on a widget.
		int UiConsumed(lua_State* L) {
			lua_pushboolean(L, NavOf(L).Consumed() ? 1 : 0);
			return 1;
		}

		// back_pressed() -> bool: Escape or pad B this frame.
		int UiBackPressed(lua_State* L) {
			lua_pushboolean(L, NavOf(L).Back() ? 1 : 0);
			return 1;
		}

		// menu(): marks this draw as a menu. Keyboard and pad navigation acts only while a menu is drawn.
		int UiMenu(lua_State* L) {
			Recorder(L);
			NavOf(L).MarkMenu();
			return 0;
		}

		float UvComponent(lua_State* L, int tableIdx, lua_Integer i) {
			lua_geti(L, tableIdx, i);
			const float v = static_cast<float>(luaL_checknumber(L, -1));
			lua_pop(L, 1);
			return v;
		}

		// texture(logical, x, y, w, h [, tint, uv]) -> true | false, reason
		// The image is read through AssetIO under the project's read policy (plus textures/, images/, fonts/) and kept
		// in the module's cache. uv = {u0, v0, u1, v1} selects part of it (default: the whole image).
		int UiTexture(lua_State* L) {
			UiRecorder& rec = Recorder(L);
			UiState* state = StateOf(L);
			if (!state || !state->textures || !state->data) return luaL_error(L, "ui.texture is not available here");
			const std::string requested = luaL_checkstring(L, 1);
			std::string reason;
			auto fail = [L](const std::string& message) {
				lua_pushboolean(L, 0);
				lua_pushstring(L, message.c_str());
				return 2;
			};
			const auto normalized = AssetIO::Normalize(requested, &reason);
			if (!normalized) return fail(reason);
			if (!DataReadAllowed(*state->data, *normalized, reason, true)) return fail(reason);
			if (!state->textures->Acquire(*normalized, reason)) return fail(reason);

			UiCommand c;
			c.op = UiOp::Picture;
			c.text = *normalized;
			const float x = static_cast<float>(luaL_checknumber(L, 2)), y = static_cast<float>(luaL_checknumber(L, 3));
			const float w = static_cast<float>(luaL_checknumber(L, 4)), h = static_cast<float>(luaL_checknumber(L, 5));
			MinMax({ x, y }, { x + w, y + h }, c.a, c.b);
			c.color = lua_isnoneornil(L, 6) ? IM_COL32_WHITE : CheckColor(L, 6);
			if (!lua_isnoneornil(L, 7)) {
				luaL_checktype(L, 7, LUA_TTABLE);
				c.uv = glm::vec4(UvComponent(L, 7, 1), UvComponent(L, 7, 2), UvComponent(L, 7, 3), UvComponent(L, 7, 4));
			}
			rec.Add(std::move(c));
			lua_pushboolean(L, 1);
			return 1;
		}
	}

	int InstallUiApi(lua_State* L, UiState* state) {
		lua_pushlightuserdata(L, state);
		lua_setfield(L, LUA_REGISTRYINDEX, kStateKey);

		lua_newtable(L);
		struct Entry { const char* name; lua_CFunction fn; };
		const Entry entries[] = {
			{ "rect", UiRect }, { "rect_filled", UiRectFilled }, { "line", UiLine },
			{ "polyline", UiPolyline }, { "polygon_filled", UiPolygonFilled },
			{ "circle", UiCircle }, { "circle_filled", UiCircleFilled }, { "arc", UiArc },
			{ "text", UiText }, { "text_size", UiTextSize }, { "image", UiImage },
			{ "clip_push", UiClipPush }, { "clip_pop", UiClipPop },
			{ "viewport", UiViewport }, { "pointer", UiPointer },
			{ "texture", UiTexture }, { "icon_exists", UiIconExists },
			{ "button", UiButton }, { "toggle", UiToggle }, { "slider", UiSlider }, { "label", UiLabel },
			{ "panel", UiPanel }, { "measure_text", UiMeasureText }, { "menu", UiMenu },
			{ "consumed", UiConsumed }, { "back_pressed", UiBackPressed },
		};
		for (const Entry& e : entries) {
			lua_pushcfunction(L, e.fn);
			lua_setfield(L, -2, e.name);
		}
		// Layout helpers written in Lua (no state of their own): ui.column(x, y, w, spacing) -> col; col:next(h) -> rect.
		static const char* const kPrelude =
			"local ui = ...\n"
			"function ui.column(x, y, w, spacing)\n"
			"  local col = { _x = x, _y = y, _w = w, _s = spacing or 0 }\n"
			"  function col.next(self, h)\n"
			"    local r = { x = self._x, y = self._y, w = self._w, h = h }\n"
			"    self._y = self._y + h + self._s\n"
			"    return r\n"
			"  end\n"
			"  return col\n"
			"end\n";
		if (luaL_loadstring(L, kPrelude) == LUA_OK) {
			lua_pushvalue(L, -2);
			if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
		}
		else {
			lua_pop(L, 1);
		}
		return luaL_ref(L, LUA_REGISTRYINDEX);
	}

	// ---- Playback ---------------------------------------------------------------------------------

	UiTextureCache::~UiTextureCache() = default;

	bool UiTextureCache::Acquire(const std::string& logical, std::string& error) {
		if (m_entries.count(logical)) return true;
		if (m_entries.size() >= ui_limits::kMaxTextures) {
			error = "too many images (max " + std::to_string(ui_limits::kMaxTextures) + " per module)";
			return false;
		}
		const auto stat = AssetIO::Stat(logical);
		if (!stat || stat->isDirectory) {
			error = "no such image: " + logical;
			return false;
		}
		if (stat->size > ui_limits::kMaxTextureFileBytes) {
			error = "image file is larger than 16 MiB";
			return false;
		}
		const AssetIO::Buffer bytes = AssetIO::ReadAll(logical, &error);
		if (!bytes) return false;
		// Dimensions are checked from the header before any pixel memory is allocated.
		const int size = static_cast<int>(bytes->size());
		int w = 0, h = 0, comp = 0;
		if (!stbi_info_from_memory(bytes->data(), size, &w, &h, &comp)) {
			error = "not a decodable image: " + logical;
			return false;
		}
		if (w <= 0 || h <= 0 || w > ui_limits::kMaxTextureSide || h > ui_limits::kMaxTextureSide) {
			error = "image is larger than " + std::to_string(ui_limits::kMaxTextureSide) + " pixels per side";
			return false;
		}
		const size_t decodedBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
		if (m_bytes + decodedBytes > ui_limits::kMaxTextureBytes) {
			error = "image memory budget is full (64 MiB per module)";
			return false;
		}
		stbi_set_flip_vertically_on_load_thread(0);
		unsigned char* pixels = stbi_load_from_memory(bytes->data(), size, &w, &h, &comp, 4);
		if (!pixels) {
			error = "cannot decode image: " + logical;
			return false;
		}
		auto texture = std::make_unique<Texture>(w, h, rhi::Format::RGBA8, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, pixels, false);
		stbi_image_free(pixels);
		texture->SetParameters(rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Filter::Linear, rhi::Filter::Linear);
		Entry entry;
		entry.texture = std::move(texture);
		entry.bytes = decodedBytes;
		m_bytes += decodedBytes;
		m_entries.emplace(logical, std::move(entry));
		return true;
	}

	const void* UiTextureCache::Handle(const std::string& logical) const {
		const auto it = m_entries.find(logical);
		if (it == m_entries.end() || !it->second.texture) return nullptr;
		return rhi::Gpu().ImGuiTextureId(it->second.texture->ID());
	}

	void UiTextureCache::Clear() {
		m_entries.clear();
		m_bytes = 0;
	}

	void PlaybackUi(const UiRecorder& recorder, const UiFontTable* fonts, ImDrawList* dl, const glm::vec2& origin,
		const UiTextureCache* textures) {
		if (!dl) return;
		const ImVec2 o(origin.x, origin.y);
		const std::vector<glm::vec2>& pts = recorder.Points();
		size_t depth = 0;
		std::vector<ImVec2> buffer;
		for (const UiCommand& c : recorder.Commands()) {
			switch (c.op) {
			case UiOp::Rect:
				dl->AddRect(ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o, c.color, c.rounding, 0, c.thickness);
				break;
			case UiOp::RectFilled:
				dl->AddRectFilled(ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o, c.color, c.rounding);
				break;
			case UiOp::Line:
				dl->AddLine(ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o, c.color, c.thickness);
				break;
			case UiOp::Polyline:
			case UiOp::PolygonFilled: {
				buffer.clear();
				for (uint32_t i = 0; i < c.pointCount; ++i) {
					const glm::vec2& p = pts[c.pointStart + i];
					buffer.push_back(ImVec2(p.x, p.y) + o);
				}
				if (c.op == UiOp::Polyline) {
					dl->AddPolyline(buffer.data(), static_cast<int>(buffer.size()), c.color, c.closed ? ImDrawFlags_Closed : 0, c.thickness);
				}
				else {
					dl->AddConcavePolyFilled(buffer.data(), static_cast<int>(buffer.size()), c.color);
				}
				break;
			}
			case UiOp::Circle:
				dl->AddCircle(ImVec2(c.a.x, c.a.y) + o, c.b.x, c.color, 0, c.thickness);
				break;
			case UiOp::CircleFilled:
				dl->AddCircleFilled(ImVec2(c.a.x, c.a.y) + o, c.b.x, c.color, 0);
				break;
			case UiOp::Text: {
				ImFont* font = FontFor(fonts, c.font);
				if (!font) break;
				const ImVec2 size = font->CalcTextSizeA(c.size, FLT_MAX, 0.0f, c.text.c_str());
				ImVec2 pos = ImVec2(c.a.x, c.a.y) + o;
				if (c.halign == UiAlign::Center) pos.x -= size.x * 0.5f;
				else if (c.halign == UiAlign::Right) pos.x -= size.x;
				if (c.valign == UiValign::Middle) pos.y -= size.y * 0.5f;
				else if (c.valign == UiValign::Bottom) pos.y -= size.y;
				dl->AddText(font, c.size, pos, c.color, c.text.c_str());
				break;
			}
			case UiOp::Image:
				UiIconAtlas::Get().Draw(dl, c.text, ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o, c.color);
				break;
			case UiOp::Picture: {
				const void* handle = textures ? textures->Handle(c.text) : nullptr;
				if (handle) {
					dl->AddImage((ImTextureID)handle, ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o,
						ImVec2(c.uv.x, c.uv.y), ImVec2(c.uv.z, c.uv.w), c.color);
				}
				break;
			}
			case UiOp::ClipPush:
				dl->PushClipRect(ImVec2(c.a.x, c.a.y) + o, ImVec2(c.b.x, c.b.y) + o, true);
				++depth;
				break;
			case UiOp::ClipPop:
				if (depth > 0) {
					dl->PopClipRect();
					--depth;
				}
				break;
			}
		}
		while (depth > 0) {
			dl->PopClipRect();
			--depth;
		}
	}
}
