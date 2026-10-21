#pragma once
// Project action map (docs/lua_api.md "Input"): projects/<name>/input.json declares named actions that map to
// keyboard keys, gamepad buttons and gamepad axes. The engine samples the devices once per frame on the
// gameplay thread (SDL state, no Lua). Scripts query the latched sample through ctx:action_down/pressed/released
// and ctx:axis, so every system and fixed substep of a frame sees the same values.
//
// File format (format 1):
//   { "format": 1, "actions": {
//       "accelerate": { "type": "button", "keys": ["W", "Up"], "pad_buttons": ["a"] },
//       "steer": { "type": "axis", "keys_negative": ["A", "Left"], "keys_positive": ["D", "Right"],
//                  "pad_axis": "leftx", "deadzone": 0.15 } } }
// Key names are SDL key names ("W", "Space", "Left Shift"). Pad buttons and axes use SDL controller names.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace NoxLua {

	struct InputActionDecl {
		enum class Kind { Button, Axis };
		std::string name;
		Kind kind = Kind::Button;
		std::vector<std::string> keys;            // button: any of these keys
		std::vector<std::string> keysNegative;    // axis: -1 while any is held
		std::vector<std::string> keysPositive;    // axis: +1 while any is held
		std::vector<std::string> padButtons;      // button: any of these pad buttons
		std::string padAxis;                      // axis: gamepad axis (-1..1, deadzone applied)
		float deadzone = 0.15f;
	};

	// Device readers. Codes are SDL_Scancode and SDL_GameControllerButton/Axis values.
	struct InputDevices {
		std::function<bool(int scancode)> key;
		std::function<bool(int button)> padButton;
		std::function<float(int axis)> padAxis;   // -1..1 (raw, before deadzone)
	};

	// The SDL keyboard and the first open game controller as device readers (enabled = false reads nothing).
	// Used by the action map and by the menu navigation (LuaUiFocus.h); both sample once per frame.
	InputDevices SdlInputDevices(bool enabled);

	class InputMap {
	public:
		InputMap() = default;
		~InputMap();
		InputMap(InputMap&& other) noexcept;
		InputMap& operator=(InputMap&&) = delete;
		InputMap(const InputMap&) = delete;
		InputMap& operator=(const InputMap&) = delete;
		// Parses the descriptor text. Unknown key/pad names and malformed entries are reported in `errors`.
		// Returns false when any error was found (the map keeps the valid actions).
		bool Parse(const std::string& jsonText, std::vector<std::string>& errors);
		// Logical path read through AssetIO; a missing file is an empty map (not an error).
		bool LoadFile(const std::string& logicalPath, std::vector<std::string>& errors);

		// Samples SDL. When `enabled` is false (no input focus) every action reads as released.
		void Sample(bool enabled);
		// Same rules with explicit device readers (tests, and any other input backend).
		void SampleWith(const InputDevices& devices, bool enabled);

		bool Has(const std::string& name) const { return Find(name) >= 0; }

		// Runtime rebinding (input.rebind). Replaces the keyboard keys of a button action with one key name. Returns
		// false with `error` set for an unknown action, an axis action or an unknown key name.
		bool Rebind(const std::string& action, const std::string& keyName, std::string& error);
		// Key and pad button names currently bound to an action; false when the action is unknown.
		bool KeyNames(const std::string& action, std::vector<std::string>& keys, std::vector<std::string>& padButtons) const;
		// Writes the rebound keys to the user settings directory (overrides only, one file per action map). The
		// overrides are applied again by LoadFile. False when nothing could be written.
		bool SaveOverrides() const;
		// Gamepad rumble on the first open controller. low/high are motor strengths in 0..1. False without a pad.
		bool Rumble(float low, float high, int durationMs);
		// Logical path of the last LoadFile (empty when none).
		const std::string& LogicalPath() const { return m_logicalPath; }
		// Unknown names return false / 0 and set *known to false.
		bool Down(const std::string& name, bool* known = nullptr) const;
		bool Pressed(const std::string& name, bool* known = nullptr) const;
		bool Released(const std::string& name, bool* known = nullptr) const;
		float Axis(const std::string& name, bool* known = nullptr) const;

		const std::vector<InputActionDecl>& Actions() const { return m_decls; }
		size_t Count() const { return m_decls.size(); }
		static constexpr size_t kMaxActions = 256;
		static constexpr size_t kMaxBindingsPerAction = 16;

	private:
		struct Resolved {
			std::vector<int> keys, keysNegative, keysPositive, padButtons;
			int padAxis = -1;
		};
		struct State {
			float value = 0.0f;
			bool down = false;
			bool prevDown = false;
			bool pressed = false;
			bool released = false;
		};
		int Find(const std::string& name) const;
		void ApplyOverrides(const std::string& jsonText, std::vector<std::string>& errors);
		void ClosePad();
		void* OpenPad();

		std::vector<InputActionDecl> m_decls;
		std::vector<Resolved> m_resolved;
		std::string m_logicalPath;
		std::vector<std::pair<std::string, std::vector<std::string>>> m_overrides;   // rebound keys per action
		std::vector<State> m_state;
		void* m_pad = nullptr;   // SDL_GameController*, opened on demand
	};
}
