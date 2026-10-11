#pragma once
// Engine pointer layer: per-frame mouse state, immediate-mode hit regions, consume rules and
// cursor/capture policy. Independent of ImGui (fed from SDL events by the window layer), but uses
// the same coordinate space as ImGui draw lists (window coordinates), so a region registered with
// the rectangle that was drawn is hit exactly where it is seen.
//
// Frame contract (driven by the window layer):
//   ProcessEvent(e) for every SDL event -> BeginFrame(dt) -> engine/game update (queries) ->
//   draw (AddRect/AddCircle with the drawn geometry) -> EndFrame() (regions become the hit set
//   of the next frame, the requested cursor mode is applied).
// Regions are resolved against the set registered by the previous frame's draw, which is the frame
// the user is looking at when they click.
//
// Consume rule: the topmost region under the pointer owns hover, presses and the wheel (wheel goes
// to the topmost region that accepts it). Anything below (gameplay, world cameras) must use
// WorldPressed()/WorldWheel(), which return nothing while the pointer is over a region.
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

union SDL_Event;
struct SDL_Window;

namespace ui {

using PointerId = uint32_t;   // 0 = none

// Stable id from a name and an index (FNV-1a); the same name/index always gives the same id.
PointerId HashId(const char* name, int index = 0);

enum class PointerButton : int { Left = 0, Right = 1, Middle = 2 };
constexpr int kPointerButtons = 3;

// Region behaviour flags.
enum PointerRegionFlags : uint32_t {
	RegionClick = 1u << 0,    // takes presses/clicks (becomes active on press)
	RegionWheel = 1u << 1,    // takes the wheel
	RegionAll = RegionClick | RegionWheel,
};

enum class CursorMode {
	Visible,   // absolute pointer, OS cursor shown
	Hidden,    // absolute pointer, OS cursor hidden over the window
	Captured,  // hidden + relative motion (mouse look); released on focus loss
};

class Pointer {
public:
	static Pointer& Get();

	// ---- Window layer ------------------------------------------------------------------------
	void SetWindow(SDL_Window* window);
	SDL_Window* Window() const { return m_window; }
	// Mouse, wheel and window (focus, enter/leave, size, display) events.
	void ProcessEvent(const SDL_Event& e);
	void BeginFrame(float dt);
	void EndFrame();
	// The host lets the pointer layer drive OS cursor visibility/capture (standalone game). When
	// false (editor) SetCursorMode requests are recorded but never applied.
	void SetCursorManaged(bool managed) { m_cursorManaged = managed; }
	bool CursorManaged() const { return m_cursorManaged; }

	// ---- Frame state ---------------------------------------------------------------------------
	bool HasFocus() const { return m_focus; }
	bool InWindow() const { return m_inWindow; }
	bool FocusLost() const { return m_frameFocusLost; }      // this frame
	bool FocusGained() const { return m_frameFocusGained; }  // this frame
	// Window coordinates (= ImGui display coordinates / draw-list space).
	glm::vec2 Position() const { return m_pos; }
	glm::vec2 Delta() const { return m_delta; }
	bool Moved() const { return m_moved; }   // position changed this frame (incl. first appearance)
	// Display scale (1 = 96 dpi) and position in DPI-independent logical units.
	float DpiScale() const { return m_dpiScale; }
	glm::vec2 LogicalPosition() const { return m_pos / m_dpiScale; }
	glm::vec2 WindowSize() const { return m_windowSize; }
	bool Down(PointerButton b) const { return m_btn[Idx(b)].down; }
	bool Pressed(PointerButton b) const { return m_btn[Idx(b)].pressed; }
	bool Released(PointerButton b) const { return m_btn[Idx(b)].released; }
	bool DoubleClicked(PointerButton b) const { return m_btn[Idx(b)].doubleClicked; }
	float Wheel() const { return m_wheel; }   // notches this frame, + = away from the user
	// Drag: press moved further than the threshold (4 logical px) while held.
	bool Dragging(PointerButton b) const { return m_btn[Idx(b)].dragging; }
	glm::vec2 DragStart(PointerButton b) const { return m_btn[Idx(b)].pressPos; }
	glm::vec2 DragDelta(PointerButton b) const { return m_btn[Idx(b)].down ? m_pos - m_btn[Idx(b)].pressPos : glm::vec2(0.0f); }

	// ---- Regions (immediate mode, re-registered every frame while visible) --------------------
	// Higher z wins; equal z: the later registration wins.
	void AddRect(PointerId id, const glm::vec2& min, const glm::vec2& max, int z = 0, uint32_t flags = RegionAll);
	void AddCircle(PointerId id, const glm::vec2& centre, float radius, int z = 0, uint32_t flags = RegionAll);
	// Topmost region under p in the current hit set (0 = none).
	PointerId HitTest(const glm::vec2& p, uint32_t requiredFlags = 0) const;
	// Centre of a region of the current hit set (tests and keyboard focus visuals).
	bool RegionCentre(PointerId id, glm::vec2& out) const;

	PointerId Hovered() const { return m_hovered; }
	bool IsHovered(PointerId id) const { return id != 0 && m_hovered == id; }
	// Active: the region that took the press of button b, until release.
	PointerId Active(PointerButton b = PointerButton::Left) const { return m_btn[Idx(b)].active; }
	bool IsActive(PointerId id, PointerButton b = PointerButton::Left) const { return id != 0 && m_btn[Idx(b)].active == id; }
	bool IsPressed(PointerId id, PointerButton b = PointerButton::Left) const { return Pressed(b) && IsActive(id, b); }
	// Released over the region that took the press, without dragging.
	bool IsClicked(PointerId id, PointerButton b = PointerButton::Left) const { return id != 0 && m_btn[Idx(b)].clicked == id; }
	// Wheel for the topmost wheel-accepting region under the pointer (0 when it is another one).
	float WheelFor(PointerId id) const { return id != 0 && m_wheelTarget == id ? m_wheel : 0.0f; }
	// Keyboard/pad focus owner (ids chosen by the UI; purely informational for consumers).
	PointerId Focused() const { return m_focused; }
	void SetFocused(PointerId id) { m_focused = id; }

	// ---- Consume -------------------------------------------------------------------------------
	bool OverUI() const { return m_hovered != 0; }
	// Pointer input for whatever is below the UI (gameplay, world/editor cameras).
	bool WorldPressed(PointerButton b) const { return Pressed(b) && m_btn[Idx(b)].active == 0 && !OverUI(); }
	float WorldWheel() const { return m_wheelTarget == 0 && !OverUI() ? m_wheel : 0.0f; }

	// ---- Cursor / capture ----------------------------------------------------------------------
	// Requested mode; applied at EndFrame while the window has focus (and the host manages the
	// cursor). Captured is released on focus loss and restored on focus gain only if still requested.
	void SetCursorMode(CursorMode mode) { m_requested = mode; }
	CursorMode RequestedCursorMode() const { return m_requested; }
	bool IsCaptured() const { return m_applied == CursorMode::Captured; }

	// ---- Synthetic input (scripted tests) ------------------------------------------------------
	// While injection-only is on, real mouse events are ignored so a test cannot be disturbed.
	void SetInjectionOnly(bool on) { m_injectionOnly = on; }
	void InjectMove(const glm::vec2& windowPos);
	void InjectButton(PointerButton b, bool down);
	void InjectWheel(float notches);

private:
	struct Region { PointerId id; int z; uint32_t flags; bool circle; glm::vec2 a, b; float r; uint32_t order; };
	struct Button {
		bool down = false, pressed = false, released = false, doubleClicked = false, dragging = false;
		glm::vec2 pressPos{ 0.0f };
		PointerId active = 0, clicked = 0;
		double lastPressTime = -10.0;
		glm::vec2 lastPressPos{ -1e6f };
	};
	struct Pending { int presses = 0, releases = 0; bool down = false; };

	static int Idx(PointerButton b) { return static_cast<int>(b); }
	bool Contains(const Region& r, const glm::vec2& p) const;
	void OnMove(const glm::vec2& p);
	void OnButton(int b, bool down);
	void UpdateDpi();
	void ApplyCursor();

	SDL_Window* m_window = nullptr;
	bool m_cursorManaged = false;
	bool m_injectionOnly = false;
	bool m_focus = true, m_inWindow = true;
	bool m_pendingFocusLost = false, m_pendingFocusGained = false;
	bool m_frameFocusLost = false, m_frameFocusGained = false;
	glm::vec2 m_pos{ -1e6f }, m_pendingPos{ -1e6f }, m_prevPos{ -1e6f }, m_delta{ 0.0f };
	bool m_moved = false;
	float m_pendingWheel = 0.0f, m_wheel = 0.0f;
	Pending m_pending[kPointerButtons];
	Button m_btn[kPointerButtons];
	float m_dpiScale = 1.0f;
	glm::vec2 m_windowSize{ 0.0f };
	double m_time = 0.0;

	std::vector<Region> m_hit;      // previous frame's regions (hit set)
	std::vector<Region> m_building; // regions registered this frame
	uint32_t m_order = 0;
	PointerId m_hovered = 0, m_wheelTarget = 0, m_focused = 0;

	CursorMode m_requested = CursorMode::Visible;
	CursorMode m_applied = CursorMode::Visible;
	bool m_appliedValid = false;
};

} // namespace ui
