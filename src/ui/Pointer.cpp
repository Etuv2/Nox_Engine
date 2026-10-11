#include "ui/Pointer.h"

#include <SDL/SDL.h>
#include <algorithm>
#include <iostream>

namespace ui {

namespace {
	constexpr float kDragThreshold = 4.0f;       // logical px
	constexpr float kDoubleClickDist = 6.0f;     // logical px
	constexpr double kDoubleClickTime = 0.35;    // s

	int ButtonIndex(Uint8 sdlButton) {
		switch (sdlButton) {
		case SDL_BUTTON_LEFT: return 0;
		case SDL_BUTTON_RIGHT: return 1;
		case SDL_BUTTON_MIDDLE: return 2;
		default: return -1;
		}
	}
}

PointerId HashId(const char* name, int index)
{
	uint32_t h = 2166136261u;
	for (const char* c = name ? name : ""; *c; ++c) { h ^= static_cast<uint8_t>(*c); h *= 16777619u; }
	const uint32_t i = static_cast<uint32_t>(index);
	for (int k = 0; k < 4; ++k) { h ^= (i >> (k * 8)) & 0xffu; h *= 16777619u; }
	return h == 0 ? 1u : h;
}

Pointer& Pointer::Get()
{
	static Pointer instance;
	return instance;
}

void Pointer::SetWindow(SDL_Window* window)
{
	m_window = window;
	m_focus = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
	UpdateDpi();
}

void Pointer::UpdateDpi()
{
	if (!m_window) return;
	int w = 0, h = 0;
	SDL_GetWindowSize(m_window, &w, &h);
	m_windowSize = glm::vec2(static_cast<float>(w), static_cast<float>(h));
	float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
	const int display = SDL_GetWindowDisplayIndex(m_window);
	if (display >= 0 && SDL_GetDisplayDPI(display, &ddpi, &hdpi, &vdpi) == 0 && hdpi > 1.0f) {
		m_dpiScale = std::clamp(hdpi / 96.0f, 0.5f, 4.0f);
	}
	else {
		m_dpiScale = 1.0f;
	}
}

void Pointer::OnMove(const glm::vec2& p)
{
	m_pendingPos = p;
	m_inWindow = true;
}

void Pointer::OnButton(int b, bool down)
{
	if (b < 0 || b >= kPointerButtons) return;
	Pending& p = m_pending[b];
	if (down && !p.down) ++p.presses;
	if (!down && p.down) ++p.releases;
	p.down = down;
}

void Pointer::ProcessEvent(const SDL_Event& e)
{
	switch (e.type) {
	case SDL_MOUSEMOTION:
		if (m_injectionOnly || e.motion.which == SDL_TOUCH_MOUSEID) break;
		OnMove(glm::vec2(static_cast<float>(e.motion.x), static_cast<float>(e.motion.y)));
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		if (m_injectionOnly || e.button.which == SDL_TOUCH_MOUSEID) break;
		m_pendingPos = glm::vec2(static_cast<float>(e.button.x), static_cast<float>(e.button.y));
		OnButton(ButtonIndex(e.button.button), e.type == SDL_MOUSEBUTTONDOWN);
		break;
	case SDL_MOUSEWHEEL:
		if (m_injectionOnly) break;
	{
		const float v = e.wheel.preciseY != 0.0f ? e.wheel.preciseY : static_cast<float>(e.wheel.y);
		m_pendingWheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -v : v;
		break;
	}
	case SDL_WINDOWEVENT:
		switch (e.window.event) {
		case SDL_WINDOWEVENT_FOCUS_LOST:
			m_focus = false;
			m_pendingFocusLost = true;
			// Buttons whose release we will never see: drop them (no click is generated).
			for (int b = 0; b < kPointerButtons; ++b) {
				m_pending[b] = Pending();
				m_btn[b].down = false;
				m_btn[b].active = 0;
				m_btn[b].dragging = false;
			}
			if (m_cursorManaged && m_applied != CursorMode::Visible) {
				SDL_SetRelativeMouseMode(SDL_FALSE);
				SDL_ShowCursor(SDL_ENABLE);
			}
			m_appliedValid = false;   // re-applied on focus gain if still requested
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			m_focus = true;
			m_pendingFocusGained = true;
			m_appliedValid = false;
			break;
		case SDL_WINDOWEVENT_ENTER:
			if (!m_injectionOnly) m_inWindow = true;
			break;
		case SDL_WINDOWEVENT_LEAVE:
			if (!m_injectionOnly) m_inWindow = false;
			break;
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_RESIZED:
		case SDL_WINDOWEVENT_MOVED:
		case SDL_WINDOWEVENT_DISPLAY_CHANGED:
			UpdateDpi();
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

void Pointer::InjectMove(const glm::vec2& windowPos) { OnMove(windowPos); }
void Pointer::InjectButton(PointerButton b, bool down) { OnButton(Idx(b), down); }
void Pointer::InjectWheel(float notches) { m_pendingWheel += notches; }

bool Pointer::Contains(const Region& r, const glm::vec2& p) const
{
	if (r.circle) {
		const glm::vec2 d = p - r.a;
		return d.x * d.x + d.y * d.y <= r.r * r.r;
	}
	return p.x >= r.a.x && p.x <= r.b.x && p.y >= r.a.y && p.y <= r.b.y;
}

PointerId Pointer::HitTest(const glm::vec2& p, uint32_t requiredFlags) const
{
	const Region* best = nullptr;
	for (const Region& r : m_hit) {
		if ((r.flags & requiredFlags) != requiredFlags || !Contains(r, p)) continue;
		if (!best || r.z > best->z || (r.z == best->z && r.order > best->order)) best = &r;
	}
	return best ? best->id : 0;
}

bool Pointer::RegionCentre(PointerId id, glm::vec2& out) const
{
	for (const Region& r : m_hit) {
		if (r.id != id) continue;
		out = r.circle ? r.a : (r.a + r.b) * 0.5f;
		return true;
	}
	return false;
}

void Pointer::AddRect(PointerId id, const glm::vec2& min, const glm::vec2& max, int z, uint32_t flags)
{
	if (id == 0) return;
	m_building.push_back({ id, z, flags, false, glm::min(min, max), glm::max(min, max), 0.0f, m_order++ });
}

void Pointer::AddCircle(PointerId id, const glm::vec2& centre, float radius, int z, uint32_t flags)
{
	if (id == 0 || radius <= 0.0f) return;
	m_building.push_back({ id, z, flags, true, centre, centre, radius, m_order++ });
}

void Pointer::BeginFrame(float dt)
{
	m_time += std::max(0.0f, dt);
	m_frameFocusLost = m_pendingFocusLost;
	m_frameFocusGained = m_pendingFocusGained;
	m_pendingFocusLost = m_pendingFocusGained = false;

	const bool hadPos = m_pos.x > -1e5f;
	m_prevPos = m_pos;
	m_pos = m_pendingPos;
	m_delta = hadPos && m_pos.x > -1e5f ? m_pos - m_prevPos : glm::vec2(0.0f);
	m_moved = m_pos.x > -1e5f && (!hadPos || m_pos != m_prevPos);
	m_wheel = m_pendingWheel;
	m_pendingWheel = 0.0f;

	const bool valid = m_inWindow && m_pos.x > -1e5f;
	m_hovered = valid ? HitTest(m_pos) : 0;
	m_wheelTarget = valid ? HitTest(m_pos, RegionWheel) : 0;
	// Topmost region owns presses; a region without RegionClick blocks them.
	PointerId clickTarget = 0;
	if (m_hovered != 0) {
		for (const Region& r : m_hit) {
			if (r.id == m_hovered && Contains(r, m_pos)) { clickTarget = (r.flags & RegionClick) ? r.id : 0; break; }
		}
	}

	const float dragPx = kDragThreshold * m_dpiScale;
	for (int i = 0; i < kPointerButtons; ++i) {
		Button& b = m_btn[i];
		Pending& p = m_pending[i];
		// The previous frame's release ends the press (active/dragging stay readable on that frame).
		if (!b.down) { b.active = 0; b.dragging = false; }
		b.pressed = p.presses > 0;
		b.released = p.releases > 0;
		b.clicked = 0;
		b.doubleClicked = false;
		if (b.pressed) {
			b.active = clickTarget;
			b.dragging = false;
			b.pressPos = m_pos;
			b.doubleClicked = (m_time - b.lastPressTime) <= kDoubleClickTime &&
				glm::length(m_pos - b.lastPressPos) <= kDoubleClickDist * m_dpiScale;
			b.lastPressTime = b.doubleClicked ? -10.0 : m_time;
			b.lastPressPos = m_pos;
		}
		if ((b.down || b.pressed) && !b.dragging && glm::length(m_pos - b.pressPos) > dragPx) b.dragging = true;
		if (b.released && b.active != 0 && !b.dragging && clickTarget == b.active) b.clicked = b.active;
		b.down = p.down;
		p.presses = p.releases = 0;
	}
}

void Pointer::EndFrame()
{
	m_hit.swap(m_building);
	m_building.clear();
	m_order = 0;
	ApplyCursor();
}

void Pointer::ApplyCursor()
{
	if (!m_cursorManaged) {
		// Hand the cursor back if the host stopped managing it while we held a capture.
		if (m_appliedValid && m_applied == CursorMode::Captured) SDL_SetRelativeMouseMode(SDL_FALSE);
		m_appliedValid = false;
		m_applied = CursorMode::Visible;
		return;
	}
	if (!m_focus) return;   // never capture or hide for an unfocused window
	if (m_appliedValid && m_applied == m_requested) return;
	switch (m_requested) {
	case CursorMode::Visible:
		SDL_SetRelativeMouseMode(SDL_FALSE);
		SDL_ShowCursor(SDL_ENABLE);
		break;
	case CursorMode::Hidden:
		SDL_SetRelativeMouseMode(SDL_FALSE);
		SDL_ShowCursor(SDL_DISABLE);
		break;
	case CursorMode::Captured:
		SDL_ShowCursor(SDL_DISABLE);
		SDL_SetRelativeMouseMode(SDL_TRUE);
		break;
	}
	static const char* names[] = { "visible", "hidden", "captured" };
	std::cout << "[Pointer] Cursor " << names[static_cast<int>(m_requested)] << std::endl;
	m_applied = m_requested;
	m_appliedValid = true;
}

} // namespace ui
