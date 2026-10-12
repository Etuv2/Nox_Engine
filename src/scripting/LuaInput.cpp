#include "LuaInput.h"

#include "../assets/AssetIO.h"

#include "json.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace NoxLua {

	namespace {
		bool IsIdentifier(const std::string& name) {
			if (name.empty() || name.size() > 64) return false;
			for (size_t i = 0; i < name.size(); ++i) {
				const char c = name[i];
				const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
				const bool digit = c >= '0' && c <= '9';
				if (i == 0 ? !alpha : !(alpha || digit)) return false;
			}
			return true;
		}

		// Reads an optional array of strings; each entry resolved through `resolve` (returns -1 when unknown).
		bool ReadNames(const nlohmann::json& action, const char* field, const std::string& actionName,
			std::vector<std::string>& names, const std::function<int(const std::string&)>& resolve,
			std::vector<int>& codes, std::vector<std::string>& errors) {
			if (!action.contains(field)) return true;
			if (!action[field].is_array()) {
				errors.push_back("action '" + actionName + "': " + field + " must be an array of names");
				return false;
			}
			for (const auto& entry : action[field]) {
				if (!entry.is_string()) {
					errors.push_back("action '" + actionName + "': " + field + " entries must be strings");
					return false;
				}
				const std::string name = entry.get<std::string>();
				const int code = resolve(name);
				if (code < 0) {
					errors.push_back("action '" + actionName + "': unknown " + field + " name '" + name + "'");
					return false;
				}
				names.push_back(name);
				codes.push_back(code);
			}
			if (codes.size() > InputMap::kMaxBindingsPerAction) {
				errors.push_back("action '" + actionName + "': more than " + std::to_string(InputMap::kMaxBindingsPerAction) + " bindings in " + field);
				return false;
			}
			return true;
		}

		int ResolveKey(const std::string& name) {
			const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
			return sc == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(sc);
		}
		int ResolvePadButton(const std::string& name) {
			const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
			return b == SDL_CONTROLLER_BUTTON_INVALID ? -1 : static_cast<int>(b);
		}
		int ResolvePadAxis(const std::string& name) {
			const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
			return a == SDL_CONTROLLER_AXIS_INVALID ? -1 : static_cast<int>(a);
		}
	}

	InputMap::~InputMap() {
		ClosePad();
	}

	InputMap::InputMap(InputMap&& other) noexcept
		: m_decls(std::move(other.m_decls))
		, m_resolved(std::move(other.m_resolved))
		, m_state(std::move(other.m_state))
		, m_pad(other.m_pad) {
		other.m_pad = nullptr;   // the controller handle moves with the map
	}

	bool InputMap::Parse(const std::string& jsonText, std::vector<std::string>& errors) {
		m_decls.clear();
		m_resolved.clear();
		m_state.clear();
		const size_t before = errors.size();
		const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false);
		if (root.is_discarded() || !root.is_object()) {
			errors.push_back("input map is not a JSON object");
			return false;
		}
		if (root.value("format", 0) != 1) {
			errors.push_back("input map format must be 1");
			return false;
		}
		if (!root.contains("actions") || !root["actions"].is_object()) {
			return errors.size() == before;
		}
		if (root["actions"].size() > kMaxActions) {
			errors.push_back("input map has more than " + std::to_string(kMaxActions) + " actions");
			return false;
		}
		for (const auto& item : root["actions"].items()) {
			const std::string name = item.key();
			const nlohmann::json& action = item.value();
			if (!IsIdentifier(name)) {
				errors.push_back("action name '" + name + "' must be an identifier (letters, digits, underscore)");
				continue;
			}
			if (!action.is_object()) {
				errors.push_back("action '" + name + "' must be an object");
				continue;
			}
			InputActionDecl decl;
			decl.name = name;
			const std::string type = action.value("type", std::string("button"));
			if (type == "button") decl.kind = InputActionDecl::Kind::Button;
			else if (type == "axis") decl.kind = InputActionDecl::Kind::Axis;
			else {
				errors.push_back("action '" + name + "': type must be \"button\" or \"axis\"");
				continue;
			}
			decl.deadzone = action.value("deadzone", 0.15f);
			if (!(decl.deadzone >= 0.0f && decl.deadzone < 0.9f)) {
				errors.push_back("action '" + name + "': deadzone must be in [0, 0.9)");
				continue;
			}
			Resolved res;
			bool ok = true;
			ok = ok && ReadNames(action, "keys", name, decl.keys, ResolveKey, res.keys, errors);
			ok = ok && ReadNames(action, "pad_buttons", name, decl.padButtons, ResolvePadButton, res.padButtons, errors);
			if (decl.kind == InputActionDecl::Kind::Button) {
				if (action.contains("keys_negative") || action.contains("keys_positive") || action.contains("pad_axis")) {
					errors.push_back("action '" + name + "': button actions take keys and pad_buttons only");
					ok = false;
				}
				if (decl.keys.empty() && decl.padButtons.empty()) {
					errors.push_back("action '" + name + "': button action needs keys or pad_buttons");
					ok = false;
				}
			}
			else {
				ok = ok && ReadNames(action, "keys_negative", name, decl.keysNegative, ResolveKey, res.keysNegative, errors);
				ok = ok && ReadNames(action, "keys_positive", name, decl.keysPositive, ResolveKey, res.keysPositive, errors);
				if (action.contains("pad_buttons")) {
					errors.push_back("action '" + name + "': axis actions take pad_axis, not pad_buttons");
					ok = false;
				}
				if (action.contains("keys")) {
					errors.push_back("action '" + name + "': axis actions take keys_negative and keys_positive, not keys");
					ok = false;
				}
				if (action.contains("pad_axis")) {
					const std::string axis = action.value("pad_axis", std::string());
					decl.padAxis = axis;
					res.padAxis = ResolvePadAxis(axis);
					if (res.padAxis < 0) {
						errors.push_back("action '" + name + "': unknown pad_axis '" + axis + "'");
						ok = false;
					}
				}
				if (decl.keysNegative.empty() && decl.keysPositive.empty() && res.padAxis < 0) {
					errors.push_back("action '" + name + "': axis action needs pad_axis or keys_negative/keys_positive");
					ok = false;
				}
			}
			if (!ok) continue;
			m_decls.push_back(std::move(decl));
			m_resolved.push_back(std::move(res));
			m_state.emplace_back();
		}
		return errors.size() == before;
	}

	bool InputMap::LoadFile(const std::string& logicalPath, std::vector<std::string>& errors) {
		std::string error;
		const auto text = AssetIO::ReadText(logicalPath, &error);
		if (!text) {
			m_decls.clear();
			m_resolved.clear();
			m_state.clear();
			return true;   // no input map: no actions
		}
		return Parse(*text, errors);
	}

	int InputMap::Find(const std::string& name) const {
		for (size_t i = 0; i < m_decls.size(); ++i) {
			if (m_decls[i].name == name) return static_cast<int>(i);
		}
		return -1;
	}

	void InputMap::SampleWith(const InputDevices& devices, bool enabled) {
		for (size_t i = 0; i < m_decls.size(); ++i) {
			const InputActionDecl& decl = m_decls[i];
			const Resolved& res = m_resolved[i];
			State& s = m_state[i];
			float value = 0.0f;
			if (enabled) {
				if (decl.kind == InputActionDecl::Kind::Button) {
					bool down = false;
					for (int k : res.keys) down = down || (devices.key && devices.key(k));
					for (int b : res.padButtons) down = down || (devices.padButton && devices.padButton(b));
					value = down ? 1.0f : 0.0f;
				}
				else {
					if (res.padAxis >= 0 && devices.padAxis) {
						const float raw = devices.padAxis(res.padAxis);
						const float mag = std::fabs(raw);
						if (mag > decl.deadzone) {
							const float scaled = (mag - decl.deadzone) / (1.0f - decl.deadzone);
							value += raw < 0.0f ? -scaled : scaled;
						}
					}
					bool positive = false, negative = false;
					for (int k : res.keysPositive) positive = positive || (devices.key && devices.key(k));
					for (int k : res.keysNegative) negative = negative || (devices.key && devices.key(k));
					value += (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
					value = std::clamp(value, -1.0f, 1.0f);
				}
			}
			s.value = value;
			s.prevDown = s.down;
			s.down = value != 0.0f;
			s.pressed = s.down && !s.prevDown;
			s.released = !s.down && s.prevDown;
		}
	}

	void* InputMap::OpenPad() {
		if (m_pad && !SDL_GameControllerGetAttached(static_cast<SDL_GameController*>(m_pad))) ClosePad();
		if (!m_pad) {
			const int count = SDL_NumJoysticks();
			for (int i = 0; i < count && !m_pad; ++i) {
				if (SDL_IsGameController(i)) m_pad = SDL_GameControllerOpen(i);
			}
		}
		return m_pad;
	}

	void InputMap::ClosePad() {
		if (m_pad) SDL_GameControllerClose(static_cast<SDL_GameController*>(m_pad));
		m_pad = nullptr;
	}

	void InputMap::Sample(bool enabled) {
		if (m_decls.empty()) return;
		SDL_GameController* pad = static_cast<SDL_GameController*>(enabled ? OpenPad() : nullptr);
		const Uint8* keys = SDL_GetKeyboardState(nullptr);
		InputDevices devices;
		devices.key = [keys](int scancode) {
			return keys && scancode >= 0 && scancode < SDL_NUM_SCANCODES && keys[scancode] != 0;
		};
		devices.padButton = [pad](int button) {
			return pad && button >= 0 && SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(button)) != 0;
		};
		devices.padAxis = [pad](int axis) {
			if (!pad || axis < 0) return 0.0f;
			return static_cast<float>(SDL_GameControllerGetAxis(pad, static_cast<SDL_GameControllerAxis>(axis))) / 32767.0f;
		};
		SampleWith(devices, enabled);
	}

	bool InputMap::Down(const std::string& name, bool* known) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && m_state[static_cast<size_t>(i)].down;
	}

	bool InputMap::Pressed(const std::string& name, bool* known) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && m_state[static_cast<size_t>(i)].pressed;
	}

	bool InputMap::Released(const std::string& name, bool* known) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && m_state[static_cast<size_t>(i)].released;
	}

	float InputMap::Axis(const std::string& name, bool* known) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 ? m_state[static_cast<size_t>(i)].value : 0.0f;
	}
}
