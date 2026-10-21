#include "LuaUiFocus.h"

#include <SDL.h>

#include <cmath>
#include <limits>

namespace NoxLua {

	namespace {
		constexpr size_t kMaxWidgetsPerFrame = 256;
		constexpr float kStickThreshold = 0.5f;

		bool Contains(const UiNavigator::Widget& w, const glm::vec2& p) {
			return p.x >= w.min.x && p.x <= w.max.x && p.y >= w.min.y && p.y <= w.max.y;
		}

		glm::vec2 Centre(const UiNavigator::Widget& w) {
			return (w.min + w.max) * 0.5f;
		}
	}

	void UiNavigator::Sample(const InputDevices& devices, bool enabled) {
		bool down[ActionCount] = {};
		if (enabled) {
			const auto key = [&devices](int scancode) { return devices.key && devices.key(scancode); };
			const auto pad = [&devices](int button) { return devices.padButton && devices.padButton(button); };
			const auto stick = [&devices](int axis) { return devices.padAxis ? devices.padAxis(axis) : 0.0f; };
			down[ActUp] = key(SDL_SCANCODE_UP) || pad(SDL_CONTROLLER_BUTTON_DPAD_UP) || stick(SDL_CONTROLLER_AXIS_LEFTY) < -kStickThreshold;
			down[ActDown] = key(SDL_SCANCODE_DOWN) || pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || stick(SDL_CONTROLLER_AXIS_LEFTY) > kStickThreshold;
			down[ActLeft] = key(SDL_SCANCODE_LEFT) || pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || stick(SDL_CONTROLLER_AXIS_LEFTX) < -kStickThreshold;
			down[ActRight] = key(SDL_SCANCODE_RIGHT) || pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || stick(SDL_CONTROLLER_AXIS_LEFTX) > kStickThreshold;
			down[ActAccept] = key(SDL_SCANCODE_RETURN) || key(SDL_SCANCODE_KP_ENTER) || key(SDL_SCANCODE_SPACE) ||
				pad(SDL_CONTROLLER_BUTTON_A);
			down[ActBack] = key(SDL_SCANCODE_ESCAPE) || pad(SDL_CONTROLLER_BUTTON_B);
		}
		for (int i = 0; i < ActionCount; ++i) {
			Edge& e = m_edges[i];
			e.prevDown = e.down;
			e.down = down[i];
			e.pressed = e.down && !e.prevDown;
		}
	}

	const UiNavigator::Widget* UiNavigator::Find(uint32_t id) const {
		for (const Widget& w : m_layout) {
			if (w.id == id) return &w;
		}
		return nullptr;
	}

	uint32_t UiNavigator::FirstNavigable() const {
		for (const Widget& w : m_layout) {
			if (w.navigable) return w.id;
		}
		return 0;
	}

	// Nearest navigable widget in direction `dir` (unit axis). Candidates behind the origin are ignored; sideways
	// offset is penalised twice, so a column of buttons moves straight down and a row moves straight across.
	uint32_t UiNavigator::Neighbour(const Widget& from, const glm::vec2& dir) const {
		const glm::vec2 origin = Centre(from);
		uint32_t best = 0;
		float bestScore = std::numeric_limits<float>::max();
		for (const Widget& w : m_layout) {
			if (!w.navigable || w.id == from.id) continue;
			const glm::vec2 d = Centre(w) - origin;
			const float primary = d.x * dir.x + d.y * dir.y;
			if (primary <= 0.5f) continue;
			const float perp = std::fabs(d.x * dir.y - d.y * dir.x);
			const float score = primary + 2.0f * perp;
			if (score < bestScore) {
				bestScore = score;
				best = w.id;
			}
		}
		return best;
	}

	void UiNavigator::Resolve(const glm::vec2& pointer, bool pointerValid, bool moved, bool pressed) {
		m_accept = false;
		m_back = false;
		m_valueStep = 0;
		m_consumed = false;
		const auto pressedOf = [this](ActionIndex a) { return m_edges[a].pressed; };
		// Escape / B is read by gameplay and menus alike; it is consumed only while a menu owns the keys.
		m_back = pressedOf(ActBack);

		// Pointer presses over any widget (HUD buttons included) belong to the UI, so gameplay does not also see them.
		const bool pointerOk = pointerValid && pointer.x > -1e5f && pointer.y > -1e5f;
		const Widget* under = nullptr;
		if (pointerOk && (moved || pressed)) {
			for (auto it = m_layout.rbegin(); it != m_layout.rend(); ++it) {
				if (Contains(*it, pointer)) {
					under = &*it;   // the last registered widget under the pointer is the topmost one
					break;
				}
			}
		}
		if (pressed && under) m_consumed = true;

		if (!m_menu) {
			m_focus = 0;   // keyboard and pad navigation belongs to menus only
			return;
		}
		if (m_layout.empty()) {
			m_focus = 0;
			return;
		}
		if (m_focus != 0) {
			const Widget* f = Find(m_focus);
			if (!f || !f->navigable) m_focus = FirstNavigable();
		}
		if (under && under->navigable) {
			if (moved) m_focus = under->id;   // hover moves focus while the mouse moves
			if (pressed) m_focus = under->id; // a press focuses the widget it landed on
		}

		// A menu starts with its first navigable widget focused; arrows then move from there.
		if (m_focus == 0) m_focus = FirstNavigable();
		const Widget* f = Find(m_focus);
		if (!f) return;
		const bool anyMove = pressedOf(ActUp) || pressedOf(ActDown) || pressedOf(ActLeft) || pressedOf(ActRight);
		m_consumed = m_consumed || anyMove || pressedOf(ActAccept) || pressedOf(ActBack);
		if (f->value && (pressedOf(ActLeft) || pressedOf(ActRight))) {
			m_valueStep = pressedOf(ActRight) ? 1 : -1;
		}
		else if (anyMove) {
			glm::vec2 dir(0.0f);
			if (pressedOf(ActUp)) dir.y -= 1.0f;   // screen space: y grows downwards
			if (pressedOf(ActDown)) dir.y += 1.0f;
			if (pressedOf(ActLeft)) dir.x -= 1.0f;
			if (pressedOf(ActRight)) dir.x += 1.0f;
			if (dir.x != 0.0f && dir.y != 0.0f) dir = glm::normalize(dir);
			const uint32_t next = Neighbour(*f, dir);
			if (next != 0) m_focus = next;
		}
		if (pressedOf(ActAccept)) m_accept = true;
		if (pressedOf(ActBack)) m_consumed = true;
	}

	void UiNavigator::BeginLayout() {
		m_next.clear();
		m_nextMenu = false;
	}

	void UiNavigator::MarkMenu() {
		m_nextMenu = true;
	}

	void UiNavigator::AddWidget(const Widget& widget) {
		if (widget.id == 0 || m_next.size() >= kMaxWidgetsPerFrame) return;
		m_next.push_back(widget);
	}

	void UiNavigator::EndLayout() {
		m_layout.swap(m_next);
		m_next.clear();
		m_menu = m_nextMenu;
	}

	void UiNavigator::AppendWidget(const Widget& widget) {
		if (widget.id == 0 || m_layout.size() >= kMaxWidgetsPerFrame) return;
		m_layout.push_back(widget);
	}

	void UiNavigator::Reset() {
		m_layout.clear();
		m_next.clear();
		m_menu = m_nextMenu = false;
		m_focus = 0;
		m_valueStep = 0;
		m_accept = m_back = m_consumed = false;
	}
}
