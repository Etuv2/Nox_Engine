#pragma once
// Project action map (docs/lua_api.md "Input"): projects/<name>/input.json declares named actions that map to
// keyboard keys, mouse buttons, gamepad buttons and gamepad axes. The engine samples the devices once per frame on
// the gameplay thread (SDL state, no Lua). Scripts query the latched sample through ctx:action_down/pressed/released
// and ctx:axis, so every system and fixed substep of a frame sees the same values.
//
// File format (format 1):
//   { "format": 1, "actions": {
//       "accelerate": { "type": "button", "keys": ["W", "Up"], "pad_buttons": ["a"], "mouse_buttons": ["left"] },
//       "steer": { "type": "axis", "keys_negative": ["A", "Left"], "keys_positive": ["D", "Right"],
//                  "pad_axis": "leftx", "invert": false, "deadzone": 0.15 } } }
// Key names are SDL key names ("W", "Space", "Left Shift"). Pad buttons and axes use SDL controller names.
//
// Players: the keyboard and the mouse feed player 0. Gamepad slot i (0..3, in the order SDL opens them) feeds
// player i, so player 0 is "keyboard plus first pad". Queries take an optional player index (default 0).
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace NoxLua {

	constexpr int kMaxPads = 4;

	struct InputActionDecl {
		enum class Kind { Button, Axis };
		std::string name;
		Kind kind = Kind::Button;
		std::vector<std::string> keys;            // button: any of these keys
		std::vector<std::string> keysNegative;    // axis: -1 while any is held
		std::vector<std::string> keysPositive;    // axis: +1 while any is held
		std::vector<std::string> padButtons;      // button: any of these pad buttons
		std::vector<std::string> mouseButtons;    // button: any of "left", "middle", "right", "x1", "x2"
		std::string padAxis;                      // axis: gamepad axis (-1..1, deadzone applied)
		bool invert = false;                      // axis: negate the pad axis
		float deadzone = 0.15f;
	};

	// One binding to put on an action (input.rebind and the result of input.listen_next).
	struct InputBinding {
		enum class Type { Key, Mouse, PadButton, PadAxis };
		enum class Role { Single, Negative, Positive };   // Key on an axis action: the negative or positive side
		Type type = Type::Key;
		Role role = Role::Single;
		std::string name;             // key name, mouse button name, pad button name or pad axis name
		bool invert = false;          // PadAxis
		float deadzone = -1.0f;       // PadAxis: negative keeps the action's deadzone
	};

	// Device readers. Codes are SDL_Scancode, SDL_BUTTON_* (1..5), SDL_GameControllerButton and SDL_GameControllerAxis.
	struct InputDevices {
		std::function<bool(int scancode)> key;
		std::function<bool(int mouseButton)> mouse;
		std::function<bool(int pad, int button)> padButton;
		std::function<float(int pad, int axis)> padAxis;   // -1..1 (raw, before deadzone)
	};

	// The SDL keyboard, mouse and gamepad slots as device readers (enabled = false reads nothing).
	// Used by the action map and by the menu navigation (LuaUiFocus.h); both sample once per frame.
	InputDevices SdlInputDevices(bool enabled);

	// Gamepad slots (process wide). The first kMaxPads controllers SDL reports fill slots 0..3 in order; a slot keeps
	// its controller while it stays attached. RefreshGamepadSlots opens new controllers and closes detached ones.
	void RefreshGamepadSlots();
	bool GamepadConnected(int slot);
	std::string GamepadName(int slot);
	// Human-readable label of a binding ("Space", "Mouse Left", "Pad A", "Left Stick X").
	std::string BindingLabel(const InputBinding& binding);

	class InputMap {
	public:
		// Binding codes of an action, parallel to its InputActionDecl (internal; exposed for the parser helpers).
		struct Resolved {
			std::vector<int> keys, keysNegative, keysPositive, padButtons, mouseButtons;
			int padAxis = -1;
		};
		struct DeviceEvent {
			bool connected = false;
			int player = 0;
			std::string name;
		};
		// A raw input found by the listen capture (also the shape input.rebind takes).
		struct Captured {
			InputBinding binding;
			int pad = -1;       // gamepad slot where the input was pressed (-1 for keyboard and mouse)
			int sign = 0;       // PadAxis: +1 or -1, the direction the stick or trigger moved
		};

		InputMap() = default;
		InputMap(InputMap&& other) noexcept = default;
		InputMap& operator=(InputMap&&) = delete;
		InputMap(const InputMap&) = delete;
		InputMap& operator=(const InputMap&) = delete;
		// Parses the descriptor text. Unknown key/pad names and malformed entries are reported in `errors`.
		// Returns false when any error was found (the map keeps the valid actions).
		bool Parse(const std::string& jsonText, std::vector<std::string>& errors);
		// Logical path read through AssetIO; a missing file is an empty map (not an error).
		// The player's rebinds from the user directory are applied after the authored actions.
		bool LoadFile(const std::string& logicalPath, std::vector<std::string>& errors);

		// Samples the devices. When `enabled` is false (no input focus) every action reads as released.
		void Sample(bool enabled);
		// Same rules with explicit device readers (tests, and any other input backend).
		void SampleWith(const InputDevices& devices, bool enabled);

		bool Has(const std::string& name) const { return Find(name) >= 0; }

		// Runtime rebinding (input.rebind). Replaces the bindings of one kind on an action: a key list, mouse
		// list, pad button list, or (axis actions) the pad axis or one composite key side. Other kinds are kept.
		// `conflicts` receives the other actions that already use the same input. False with `error` set on an
		// unknown action, a binding that does not fit the action kind, or an unknown name.
		bool Rebind(const std::string& action, const InputBinding& binding, std::vector<std::string>& conflicts,
			std::string& error);
		// Restores the authored bindings of one action (or of all actions when `action` is empty) and drops the
		// player's overrides for them. False when the action is unknown.
		bool ResetBindings(const std::string& action, std::string& error);
		// Other actions that share a binding with `action` (empty when none or unknown).
		std::vector<std::string> Conflicts(const std::string& action) const;
		// Current bindings of an action, for display. False when the action is unknown.
		bool Bindings(const std::string& action, InputActionDecl& out) const;
		// Writes the player's overrides to the user settings directory (one file per action map). The overrides are
		// applied again by LoadFile. False when nothing could be written.
		bool SaveOverrides() const;

		// Listen capture (input.listen_next): arms a capture of the next press of a key, mouse button, pad button or
		// pad axis that is not already held when the capture starts. Poll TakeCapture() once per frame.
		void BeginListen();
		void CancelListen();
		bool Listening() const { return m_listening; }
		// The captured input once (then the capture ends). False while nothing has been pressed yet.
		bool TakeCapture(Captured& out);

		// Gamepad rumble on one slot. low/high are motor strengths in 0..1. False without a pad in that slot.
		bool Rumble(float low, float high, int durationMs, int pad);
		// Connection changes seen since the last call (connected / disconnected gamepad slots).
		std::vector<DeviceEvent> TakeDeviceEvents();

		// Logical path of the last LoadFile (empty when none).
		const std::string& LogicalPath() const { return m_logicalPath; }
		// Unknown names return false / 0 and set *known to false. `player` is 0..kMaxPads-1.
		bool Down(const std::string& name, bool* known = nullptr, int player = 0) const;
		bool Pressed(const std::string& name, bool* known = nullptr, int player = 0) const;
		bool Released(const std::string& name, bool* known = nullptr, int player = 0) const;
		float Axis(const std::string& name, bool* known = nullptr, int player = 0) const;

		const std::vector<InputActionDecl>& Actions() const { return m_decls; }
		size_t Count() const { return m_decls.size(); }
		static constexpr size_t kMaxActions = 256;
		static constexpr size_t kMaxBindingsPerAction = 16;

	private:
		struct State {
			float value = 0.0f;
			bool down = false;
			bool prevDown = false;
			bool pressed = false;
			bool released = false;
		};
		int Find(const std::string& name) const;
		void ApplyOverrides(const std::string& jsonText, std::vector<std::string>& errors);
		void MarkOverridden(const std::string& action);
		void SampleListen(const InputDevices& devices);

		std::vector<InputActionDecl> m_decls;
		std::vector<Resolved> m_resolved;
		std::vector<InputActionDecl> m_authoredDecls;      // as parsed from input.json (for ResetBindings)
		std::vector<Resolved> m_authoredResolved;
		std::string m_logicalPath;
		std::vector<std::string> m_overridden;             // actions whose bindings differ from the authored ones
		std::vector<State> m_state;                        // [action * kMaxPads + player]
		bool m_padSeen[kMaxPads] = {};
		std::vector<DeviceEvent> m_deviceEvents;

		bool m_listening = false;
		bool m_listenArmed = false;                        // baseline taken, presses count from now on
		bool m_capturedReady = false;
		Captured m_captured;
		std::vector<bool> m_baseKeys;
		std::vector<bool> m_baseMouse;
		std::vector<bool> m_basePad;                       // [pad * 32 + button]
		std::vector<float> m_baseAxis;                     // [pad * 8 + axis]
	};
}
