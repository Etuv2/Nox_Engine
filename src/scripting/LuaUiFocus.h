#pragma once
// Focus and navigation for the Lua widgets (ui.button, ui.toggle, ui.slider; docs/lua_api.md "UI widgets and menus").
//
// One navigator per module world. The frame goes:
//   Sample(devices)  -> keyboard/pad edges (before gameplay Update, so gameplay sees the same frame)
//   Resolve(pointer) -> focus, hover focus and consumption, using the widget layout of the previous draw
//   gameplay Update  -> ctx:ui_consumed() / ctx:ui_back_pressed() answer for this frame
//   draw             -> BeginLayout, every widget calls Widget state queries and AddWidget, EndLayout
// Widgets therefore react one frame later to layout changes, which is invisible at 60 Hz.
//
// Navigation is active only while the previous draw registered at least one widget. Arrow keys, Enter/Space and
// Escape/B are consumed by the menu while it has a focus (or a widget was clicked); ui.consumed() reports it.
#include "LuaInput.h"

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace NoxLua {

	class UiNavigator {
	public:
		struct Widget {
			uint32_t id = 0;               // nonzero, from HashId(name)
			glm::vec2 min{ 0.0f }, max{ 0.0f };   // window coordinates
			bool navigable = true;         // enabled widgets take focus
			bool value = false;            // left/right change its value (slider, toggle) instead of moving focus
		};

		// Keyboard and pad edges. Arrow keys / d-pad / left stick move focus, Return, Space and pad A accept,
		// Escape and pad B go back. Disabled sampling reads every key as released.
		void Sample(const InputDevices& devices, bool enabled);
		// Pointer state of this frame in window coordinates. `pressed` is any button pressed this frame.
		void Resolve(const glm::vec2& pointer, bool pointerValid, bool moved, bool pressed);

		// Draw phase: the layout registered now becomes the layout of the next Resolve.
		void BeginLayout();
		void MarkMenu();                // this draw is a menu: keyboard and pad navigation is active
		void AddWidget(const Widget& widget);
		void EndLayout();
		// Adds a widget to the layout that Resolve reads next. Engine-drawn elements (GUI nodes) call this after the
		// draw handlers, so they take part in keyboard and pad focus like the Lua widgets.
		void AppendWidget(const Widget& widget);

		// Per-widget queries for this frame (valid after Resolve, during draw).
		uint32_t Focus() const { return m_focus; }
		bool IsFocused(uint32_t id) const { return id != 0 && id == m_focus; }
		// Accept edge while `id` has focus (Enter, Space or pad A).
		bool Accept(uint32_t id) const { return IsFocused(id) && m_accept; }
		// -1 or +1 for a focused value widget when left/right was pressed this frame, else 0.
		int ValueStep(uint32_t id) const { return IsFocused(id) ? m_valueStep : 0; }
		// Frame-wide answers for gameplay.
		bool Back() const { return m_back; }
		bool Consumed() const { return m_consumed; }
		bool HasLayout() const { return !m_layout.empty(); }
		bool Menu() const { return m_menu; }
		const std::vector<Widget>& Layout() const { return m_layout; }

		// Forgets focus and the layout (world reload, module without a menu).
		void Reset();

	private:
		struct Edge { bool down = false, prevDown = false, pressed = false; };
		enum ActionIndex { ActUp, ActDown, ActLeft, ActRight, ActAccept, ActBack, ActionCount };

		uint32_t FirstNavigable() const;
		uint32_t Neighbour(const Widget& from, const glm::vec2& dir) const;
		const Widget* Find(uint32_t id) const;

		Edge m_edges[ActionCount];
		std::vector<Widget> m_layout;   // registered by the last draw
		std::vector<Widget> m_next;     // registered by the draw in progress
		bool m_menu = false;            // last draw marked a menu
		bool m_nextMenu = false;
		uint32_t m_focus = 0;
		int m_valueStep = 0;
		bool m_accept = false;
		bool m_back = false;
		bool m_consumed = false;
	};
}
