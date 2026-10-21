#include "LuaInput.h"

#include "../assets/AssetIO.h"
#include "../app/UserSettings.h"

#include "json.hpp"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace NoxLua {

	namespace {
		constexpr int kButtonSlots = 32;   // SDL_CONTROLLER_BUTTON_MAX is 21; spare slots keep the arrays simple
		constexpr int kAxisSlots = 8;      // SDL_CONTROLLER_AXIS_MAX is 6
		constexpr int kMouseSlots = 6;     // SDL_BUTTON_X2 is 5
		constexpr float kListenThreshold = 0.6f;

		// ---- gamepad slots: the first kMaxPads controllers SDL reports, kept in place while they stay attached ----
		SDL_GameController* g_pads[kMaxPads] = {};

		SDL_GameController* PadAt(int slot) {
			return slot >= 0 && slot < kMaxPads ? g_pads[slot] : nullptr;
		}

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
		int ResolveMouse(const std::string& name) {
			if (name == "left") return SDL_BUTTON_LEFT;
			if (name == "middle") return SDL_BUTTON_MIDDLE;
			if (name == "right") return SDL_BUTTON_RIGHT;
			if (name == "x1") return SDL_BUTTON_X1;
			if (name == "x2") return SDL_BUTTON_X2;
			return -1;
		}

		std::string Capitalized(std::string s) {
			if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
			return s;
		}

		// Reads the binding fields of an action object into decl/res. Fields that do not fit the action kind are
		// errors. Lists are appended to, so callers clear the binding fields first when replacing them.
		bool ReadBindingFields(const nlohmann::json& action, InputActionDecl& decl, InputMap::Resolved& res,
			std::vector<std::string>& errors) {
			const std::string& name = decl.name;
			bool ok = true;
			if (decl.kind == InputActionDecl::Kind::Button) {
				ok = ReadNames(action, "keys", name, decl.keys, ResolveKey, res.keys, errors) && ok;
				ok = ReadNames(action, "pad_buttons", name, decl.padButtons, ResolvePadButton, res.padButtons, errors) && ok;
				ok = ReadNames(action, "mouse_buttons", name, decl.mouseButtons, ResolveMouse, res.mouseButtons, errors) && ok;
				if (action.contains("keys_negative") || action.contains("keys_positive") || action.contains("pad_axis") || action.contains("invert")) {
					errors.push_back("action '" + name + "': button actions take keys, pad_buttons and mouse_buttons only");
					ok = false;
				}
			}
			else {
				ok = ReadNames(action, "keys_negative", name, decl.keysNegative, ResolveKey, res.keysNegative, errors) && ok;
				ok = ReadNames(action, "keys_positive", name, decl.keysPositive, ResolveKey, res.keysPositive, errors) && ok;
				if (action.contains("pad_buttons") || action.contains("mouse_buttons")) {
					errors.push_back("action '" + name + "': axis actions take pad_axis, not pad_buttons or mouse_buttons");
					ok = false;
				}
				if (action.contains("keys")) {
					errors.push_back("action '" + name + "': axis actions take keys_negative and keys_positive, not keys");
					ok = false;
				}
				if (action.contains("pad_axis")) {
					const std::string axis = action.value("pad_axis", std::string());
					res.padAxis = ResolvePadAxis(axis);
					if (res.padAxis < 0) {
						errors.push_back("action '" + name + "': unknown pad_axis '" + axis + "'");
						ok = false;
					}
					else {
						decl.padAxis = axis;
					}
				}
				if (action.contains("invert")) {
					if (!action["invert"].is_boolean()) {
						errors.push_back("action '" + name + "': invert must be true or false");
						ok = false;
					}
					else {
						decl.invert = action["invert"].get<bool>();
					}
				}
			}
			if (action.contains("deadzone")) {
				const float dz = action["deadzone"].is_number() ? action["deadzone"].get<float>() : -1.0f;
				if (!(dz >= 0.0f && dz < 0.9f)) {
					errors.push_back("action '" + name + "': deadzone must be in [0, 0.9)");
					ok = false;
				}
				else {
					decl.deadzone = dz;
				}
			}
			return ok;
		}

		// Checks that a parsed action has something to bind.
		bool ValidateAction(const InputActionDecl& decl, const InputMap::Resolved& res, std::vector<std::string>& errors) {
			if (decl.kind == InputActionDecl::Kind::Button) {
				if (decl.keys.empty() && decl.padButtons.empty() && decl.mouseButtons.empty()) {
					errors.push_back("action '" + decl.name + "': button action needs keys, pad_buttons or mouse_buttons");
					return false;
				}
			}
			else if (decl.keysNegative.empty() && decl.keysPositive.empty() && res.padAxis < 0) {
				errors.push_back("action '" + decl.name + "': axis action needs pad_axis or keys_negative/keys_positive");
				return false;
			}
			return true;
		}

		void ClearBindings(InputActionDecl& decl, InputMap::Resolved& res) {
			decl.keys.clear();
			decl.keysNegative.clear();
			decl.keysPositive.clear();
			decl.padButtons.clear();
			decl.mouseButtons.clear();
			decl.padAxis.clear();
			decl.invert = false;
			res = InputMap::Resolved{};
		}

		// The binding fields of one action, as written to the user's override file.
		nlohmann::json BindingFields(const InputActionDecl& decl) {
			nlohmann::json out = nlohmann::json::object();
			if (decl.kind == InputActionDecl::Kind::Button) {
				out["keys"] = decl.keys;
				out["pad_buttons"] = decl.padButtons;
				out["mouse_buttons"] = decl.mouseButtons;
			}
			else {
				out["keys_negative"] = decl.keysNegative;
				out["keys_positive"] = decl.keysPositive;
				if (!decl.padAxis.empty()) out["pad_axis"] = decl.padAxis;
				out["invert"] = decl.invert;
				out["deadzone"] = decl.deadzone;
			}
			return out;
		}

		// Overrides are per action map: one file in the user directory, named from the logical path.
		std::string OverrideFileName(const std::string& logicalPath) {
			std::string userName = "input_bindings_";
			for (char c : logicalPath) {
				const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
				userName.push_back(alnum ? c : '_');
			}
			return userName + ".json";
		}

		std::string Join(const std::vector<std::string>& names) {
			std::string out;
			for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
			return out;
		}

		// The inputs an action uses, as tags. Two actions with a tag in common conflict.
		std::vector<std::string> InputTags(const InputActionDecl& decl) {
			std::vector<std::string> tags;
			for (const auto& k : decl.keys) tags.push_back("key:" + k);
			for (const auto& k : decl.keysNegative) tags.push_back("key:" + k);
			for (const auto& k : decl.keysPositive) tags.push_back("key:" + k);
			for (const auto& m : decl.mouseButtons) tags.push_back("mouse:" + m);
			for (const auto& b : decl.padButtons) tags.push_back("pad:" + b);
			if (!decl.padAxis.empty()) tags.push_back("axis:" + decl.padAxis);
			return tags;
		}

		// Listen capture: the first press since the capture started, in the order keys, mouse, pad buttons, axes.
		// (The baseline is taken on the first sample after BeginListen; inputs held at that moment do not count.)
		const char* KeyLabel(int scancode) {
			const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
			return name && *name ? name : nullptr;
		}
	}

	// ---- public helpers ---------------------------------------------------------------------------------------------

	void RefreshGamepadSlots() {
		for (auto& slot : g_pads) {
			if (slot && !SDL_GameControllerGetAttached(slot)) {
				SDL_GameControllerClose(slot);
				slot = nullptr;
			}
		}
		const int count = SDL_NumJoysticks();
		for (int i = 0; i < count; ++i) {
			if (!SDL_IsGameController(i)) continue;
			const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
			bool open = false;
			for (SDL_GameController* slot : g_pads) {
				if (slot && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot)) == id) open = true;
			}
			if (open) continue;
			for (auto& slot : g_pads) {
				if (!slot) {
					slot = SDL_GameControllerOpen(i);
					break;
				}
			}
		}
	}

	bool GamepadConnected(int slot) {
		RefreshGamepadSlots();
		return PadAt(slot) != nullptr;
	}

	std::string GamepadName(int slot) {
		RefreshGamepadSlots();
		SDL_GameController* pad = PadAt(slot);
		const char* name = pad ? SDL_GameControllerName(pad) : nullptr;
		return name ? name : "";
	}

	std::string BindingLabel(const InputBinding& b) {
		switch (b.type) {
		case InputBinding::Type::Key:
			if (b.role == InputBinding::Role::Negative) return b.name + " (negative)";
			if (b.role == InputBinding::Role::Positive) return b.name + " (positive)";
			return b.name;
		case InputBinding::Type::Mouse:
			return "Mouse " + Capitalized(b.name);
		case InputBinding::Type::PadButton: {
			static const std::pair<const char*, const char*> kNames[] = {
				{ "dpup", "D-Pad Up" }, { "dpdown", "D-Pad Down" }, { "dpleft", "D-Pad Left" }, { "dpright", "D-Pad Right" },
				{ "leftshoulder", "Left Bumper" }, { "rightshoulder", "Right Bumper" }, { "start", "Start" },
				{ "back", "Back" }, { "guide", "Guide" }, { "leftstick", "Left Stick Click" }, { "rightstick", "Right Stick Click" },
			};
			for (const auto& entry : kNames) {
				if (b.name == entry.first) return std::string("Pad ") + entry.second;
			}
			return "Pad " + Capitalized(b.name);
		}
		case InputBinding::Type::PadAxis: {
			static const std::pair<const char*, const char*> kNames[] = {
				{ "leftx", "Left Stick X" }, { "lefty", "Left Stick Y" }, { "rightx", "Right Stick X" },
				{ "righty", "Right Stick Y" }, { "lefttrigger", "Left Trigger" }, { "righttrigger", "Right Trigger" },
			};
			std::string label = b.name;
			for (const auto& entry : kNames) {
				if (b.name == entry.first) label = entry.second;
			}
			return "Pad " + label + (b.invert ? " (inverted)" : "");
		}
		}
		return b.name;
	}

	// ---- InputMap ---------------------------------------------------------------------------------------------------

	bool InputMap::Parse(const std::string& jsonText, std::vector<std::string>& errors) {
		m_decls.clear();
		m_resolved.clear();
		m_authoredDecls.clear();
		m_authoredResolved.clear();
		m_overridden.clear();
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
			Resolved res;
			if (!ReadBindingFields(action, decl, res, errors)) continue;
			if (!ValidateAction(decl, res, errors)) continue;
			m_decls.push_back(std::move(decl));
			m_resolved.push_back(std::move(res));
		}
		m_authoredDecls = m_decls;
		m_authoredResolved = m_resolved;
		m_state.assign(m_decls.size() * kMaxPads, State{});
		return errors.size() == before;
	}

	bool InputMap::LoadFile(const std::string& logicalPath, std::vector<std::string>& errors) {
		m_logicalPath = logicalPath;
		m_overridden.clear();
		std::string error;
		const auto text = AssetIO::ReadText(logicalPath, &error);
		if (!text) {
			m_decls.clear();
			m_resolved.clear();
			m_authoredDecls.clear();
			m_authoredResolved.clear();
			m_state.clear();
			return true;   // no input map: no actions
		}
		const bool ok = Parse(*text, errors);
		// Player rebinds from an earlier session (user directory) win over the authored bindings.
		if (const auto saved = UserSettings::LoadUserJson(OverrideFileName(logicalPath))) {
			ApplyOverrides(saved->dump(), errors);
		}
		return ok;
	}

	void InputMap::ApplyOverrides(const std::string& jsonText, std::vector<std::string>& errors) {
		const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false);
		if (root.is_discarded() || !root.is_object() || !root.contains("actions") || !root["actions"].is_object()) {
			errors.push_back("input bindings file is not a valid override document");
			return;
		}
		for (const auto& item : root["actions"].items()) {
			const int idx = Find(item.key());
			if (idx < 0) {
				errors.push_back("input binding: unknown input action '" + item.key() + "'");
				continue;
			}
			// Each override holds the complete binding fields of its action: read them onto a clean copy.
			InputActionDecl decl = m_decls[static_cast<size_t>(idx)];
			Resolved res = m_resolved[static_cast<size_t>(idx)];
			ClearBindings(decl, res);
			std::vector<std::string> local;
			if (!ReadBindingFields(item.value(), decl, res, local) || !ValidateAction(decl, res, local)) {
				for (auto& e : local) errors.push_back("input binding: " + e);
				continue;
			}
			m_decls[static_cast<size_t>(idx)] = std::move(decl);
			m_resolved[static_cast<size_t>(idx)] = std::move(res);
			m_overridden.push_back(item.key());
		}
	}

	void InputMap::MarkOverridden(const std::string& action) {
		if (std::find(m_overridden.begin(), m_overridden.end(), action) == m_overridden.end()) m_overridden.push_back(action);
	}

	bool InputMap::Rebind(const std::string& action, const InputBinding& binding, std::vector<std::string>& conflicts,
		std::string& error) {
		conflicts.clear();
		const int idx = Find(action);
		if (idx < 0) {
			error = "unknown input action '" + action + "'";
			return false;
		}
		InputActionDecl& decl = m_decls[static_cast<size_t>(idx)];
		Resolved& res = m_resolved[static_cast<size_t>(idx)];
		const bool isButton = decl.kind == InputActionDecl::Kind::Button;
		const bool sideOnly = binding.role != InputBinding::Role::Single;
		switch (binding.type) {
		case InputBinding::Type::Key: {
			const int code = ResolveKey(binding.name);
			if (code < 0) {
				error = "unknown key name '" + binding.name + "'";
				return false;
			}
			if (isButton) {
				if (sideOnly) {
					error = "button action '" + action + "' takes a key, not a negative or positive side";
					return false;
				}
				decl.keys = { binding.name };
				res.keys = { code };
			}
			else {
				if (!sideOnly) {
					error = "axis action '" + action + "' takes a key as role \"negative\" or \"positive\"";
					return false;
				}
				if (binding.role == InputBinding::Role::Negative) {
					decl.keysNegative = { binding.name };
					res.keysNegative = { code };
				}
				else {
					decl.keysPositive = { binding.name };
					res.keysPositive = { code };
				}
			}
			break;
		}
		case InputBinding::Type::Mouse: {
			const int code = ResolveMouse(binding.name);
			if (code < 0) {
				error = "unknown mouse button '" + binding.name + "'";
				return false;
			}
			if (!isButton) {
				error = "axis action '" + action + "' cannot take a mouse button";
				return false;
			}
			decl.mouseButtons = { binding.name };
			res.mouseButtons = { code };
			break;
		}
		case InputBinding::Type::PadButton: {
			const int code = ResolvePadButton(binding.name);
			if (code < 0) {
				error = "unknown pad button '" + binding.name + "'";
				return false;
			}
			if (!isButton) {
				error = "axis action '" + action + "' takes a pad_axis, not a pad button";
				return false;
			}
			decl.padButtons = { binding.name };
			res.padButtons = { code };
			break;
		}
		case InputBinding::Type::PadAxis: {
			const int code = ResolvePadAxis(binding.name);
			if (code < 0) {
				error = "unknown pad axis '" + binding.name + "'";
				return false;
			}
			if (isButton) {
				error = "button action '" + action + "' cannot take a pad axis";
				return false;
			}
			if (binding.deadzone >= 0.9f) {
				error = "deadzone must be in [0, 0.9)";
				return false;
			}
			decl.padAxis = binding.name;
			res.padAxis = code;
			decl.invert = binding.invert;
			if (binding.deadzone >= 0.0f) decl.deadzone = binding.deadzone;
			break;
		}
		}
		MarkOverridden(action);
		conflicts = Conflicts(action);
		return true;
	}

	bool InputMap::ResetBindings(const std::string& action, std::string& error) {
		if (action.empty()) {
			m_decls = m_authoredDecls;
			m_resolved = m_authoredResolved;
			m_overridden.clear();
			return true;
		}
		const int idx = Find(action);
		if (idx < 0) {
			error = "unknown input action '" + action + "'";
			return false;
		}
		m_decls[static_cast<size_t>(idx)] = m_authoredDecls[static_cast<size_t>(idx)];
		m_resolved[static_cast<size_t>(idx)] = m_authoredResolved[static_cast<size_t>(idx)];
		m_overridden.erase(std::remove(m_overridden.begin(), m_overridden.end(), action), m_overridden.end());
		return true;
	}

	std::vector<std::string> InputMap::Conflicts(const std::string& action) const {
		std::vector<std::string> out;
		const int idx = Find(action);
		if (idx < 0) return out;
		const std::vector<std::string> mine = InputTags(m_decls[static_cast<size_t>(idx)]);
		for (size_t j = 0; j < m_decls.size(); ++j) {
			if (static_cast<int>(j) == idx) continue;
			const std::vector<std::string> theirs = InputTags(m_decls[j]);
			for (const auto& tag : mine) {
				if (std::find(theirs.begin(), theirs.end(), tag) != theirs.end()) {
					out.push_back(m_decls[j].name);
					break;
				}
			}
		}
		return out;
	}

	bool InputMap::Bindings(const std::string& action, InputActionDecl& out) const {
		const int idx = Find(action);
		if (idx < 0) return false;
		out = m_decls[static_cast<size_t>(idx)];
		return true;
	}

	bool InputMap::SaveOverrides() const {
		nlohmann::json actions = nlohmann::json::object();
		for (const auto& name : m_overridden) {
			const int idx = Find(name);
			if (idx >= 0) actions[name] = BindingFields(m_decls[static_cast<size_t>(idx)]);
		}
		const nlohmann::json doc = { { "format", 1 }, { "actions", actions } };
		return UserSettings::SaveUserJson(OverrideFileName(m_logicalPath), doc);
	}

	void InputMap::BeginListen() {
		m_listening = true;
		m_listenArmed = false;
		m_capturedReady = false;
	}

	void InputMap::CancelListen() {
		m_listening = false;
		m_listenArmed = false;
		m_capturedReady = false;
	}

	bool InputMap::TakeCapture(Captured& out) {
		if (!m_capturedReady) return false;
		out = m_captured;
		CancelListen();
		return true;
	}

	void InputMap::SampleListen(const InputDevices& devices) {
		if (!m_listenArmed) {
			m_baseKeys.assign(SDL_NUM_SCANCODES, false);
			for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) m_baseKeys[sc] = devices.key && devices.key(sc);
			m_baseMouse.assign(kMouseSlots, false);
			for (int b = 1; b < kMouseSlots; ++b) m_baseMouse[b] = devices.mouse && devices.mouse(b);
			m_basePad.assign(kMaxPads * kButtonSlots, false);
			m_baseAxis.assign(kMaxPads * kAxisSlots, 0.0f);
			for (int p = 0; p < kMaxPads; ++p) {
				for (int b = 0; b < kButtonSlots; ++b) m_basePad[p * kButtonSlots + b] = devices.padButton && devices.padButton(p, b);
				for (int a = 0; a < kAxisSlots; ++a) m_baseAxis[p * kAxisSlots + a] = devices.padAxis ? devices.padAxis(p, a) : 0.0f;
			}
			m_listenArmed = true;
			return;
		}
		const auto capture = [this](InputBinding::Type type, const std::string& name, int pad, int sign) {
			m_captured = Captured{};
			m_captured.binding.type = type;
			m_captured.binding.name = name;
			m_captured.pad = pad;
			m_captured.sign = sign;
			m_capturedReady = true;
		};
		for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
			if (m_baseKeys[sc] || !devices.key || !devices.key(sc)) continue;
			if (const char* name = KeyLabel(sc)) capture(InputBinding::Type::Key, name, -1, 0);
			if (m_capturedReady) return;
		}
		for (int b = 1; b < kMouseSlots; ++b) {
			if (m_baseMouse[b] || !devices.mouse || !devices.mouse(b)) continue;
			static const char* kMouseNames[kMouseSlots] = { "", "left", "middle", "right", "x1", "x2" };
			capture(InputBinding::Type::Mouse, kMouseNames[b], -1, 0);
			return;
		}
		for (int p = 0; p < kMaxPads; ++p) {
			for (int b = 0; b < kButtonSlots; ++b) {
				if (m_basePad[p * kButtonSlots + b] || !devices.padButton || !devices.padButton(p, b)) continue;
				const char* name = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b));
				if (name && *name) {
					capture(InputBinding::Type::PadButton, name, p, 0);
					return;
				}
			}
		}
		for (int p = 0; p < kMaxPads; ++p) {
			for (int a = 0; a < kAxisSlots; ++a) {
				if (!devices.padAxis) return;
				const float raw = devices.padAxis(p, a);
				const float base = m_baseAxis[p * kAxisSlots + a];
				if (std::fabs(raw) <= kListenThreshold || std::fabs(base) > kListenThreshold * 0.5f) continue;
				const char* name = SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(a));
				if (name && *name) {
					capture(InputBinding::Type::PadAxis, name, p, raw < 0.0f ? -1 : 1);
					return;
				}
			}
		}
	}

	bool InputMap::Rumble(float low, float high, int durationMs, int pad) {
		RefreshGamepadSlots();
		SDL_GameController* controller = PadAt(pad);
		if (!controller || !std::isfinite(low) || !std::isfinite(high)) return false;
		const auto motor = [](float v) { return static_cast<Uint16>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
		const int ms = std::clamp(durationMs, 0, 5000);
		return SDL_GameControllerRumble(controller, motor(low), motor(high), static_cast<Uint32>(ms)) == 0;
	}

	std::vector<InputMap::DeviceEvent> InputMap::TakeDeviceEvents() {
		std::vector<DeviceEvent> out;
		out.swap(m_deviceEvents);
		return out;
	}

	int InputMap::Find(const std::string& name) const {
		for (size_t i = 0; i < m_decls.size(); ++i) {
			if (m_decls[i].name == name) return static_cast<int>(i);
		}
		return -1;
	}

	void InputMap::SampleWith(const InputDevices& devices, bool enabled) {
		if (m_listening && enabled && !m_capturedReady) SampleListen(devices);
		for (size_t i = 0; i < m_decls.size(); ++i) {
			const InputActionDecl& decl = m_decls[i];
			const Resolved& res = m_resolved[i];
			for (int p = 0; p < kMaxPads; ++p) {
				State& s = m_state[i * kMaxPads + p];
				const bool keyboard = p == 0;   // keyboard and mouse belong to player 0
				float value = 0.0f;
				if (enabled) {
					if (decl.kind == InputActionDecl::Kind::Button) {
						bool down = false;
						if (keyboard) {
							for (int k : res.keys) down = down || (devices.key && devices.key(k));
							for (int m : res.mouseButtons) down = down || (devices.mouse && devices.mouse(m));
						}
						for (int b : res.padButtons) down = down || (devices.padButton && devices.padButton(p, b));
						value = down ? 1.0f : 0.0f;
					}
					else {
						if (res.padAxis >= 0 && devices.padAxis) {
							float raw = devices.padAxis(p, res.padAxis);
							if (decl.invert) raw = -raw;
							const float mag = std::fabs(raw);
							if (mag > decl.deadzone) {
								const float scaled = (mag - decl.deadzone) / (1.0f - decl.deadzone);
								value += raw < 0.0f ? -scaled : scaled;
							}
						}
						if (keyboard) {
							bool positive = false, negative = false;
							for (int k : res.keysPositive) positive = positive || (devices.key && devices.key(k));
							for (int k : res.keysNegative) negative = negative || (devices.key && devices.key(k));
							value += (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
						}
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
	}

	InputDevices SdlInputDevices(bool enabled) {
		RefreshGamepadSlots();
		const Uint8* keys = enabled ? SDL_GetKeyboardState(nullptr) : nullptr;
		const Uint32 mouse = enabled ? SDL_GetMouseState(nullptr, nullptr) : 0u;
		InputDevices devices;
		devices.key = [keys](int scancode) {
			return keys && scancode >= 0 && scancode < SDL_NUM_SCANCODES && keys[scancode] != 0;
		};
		devices.mouse = [mouse, enabled](int button) {
			return enabled && button >= 1 && button <= 5 && (mouse & SDL_BUTTON(button)) != 0;
		};
		devices.padButton = [enabled](int pad, int button) {
			SDL_GameController* c = enabled ? PadAt(pad) : nullptr;
			return c && button >= 0 && SDL_GameControllerGetButton(c, static_cast<SDL_GameControllerButton>(button)) != 0;
		};
		devices.padAxis = [enabled](int pad, int axis) {
			SDL_GameController* c = enabled ? PadAt(pad) : nullptr;
			if (!c || axis < 0) return 0.0f;
			return static_cast<float>(SDL_GameControllerGetAxis(c, static_cast<SDL_GameControllerAxis>(axis))) / 32767.0f;
		};
		return devices;
	}

	void InputMap::Sample(bool enabled) {
		// Hot-plug: compare the gamepad slots with what this map saw last frame.
		RefreshGamepadSlots();
		for (int p = 0; p < kMaxPads; ++p) {
			const bool now = PadAt(p) != nullptr;
			if (now == m_padSeen[p]) continue;
			m_padSeen[p] = now;
			DeviceEvent event;
			event.connected = now;
			event.player = p;
			event.name = now ? GamepadName(p) : std::string();
			m_deviceEvents.push_back(std::move(event));
		}
		SampleWith(SdlInputDevices(enabled), enabled);
	}

	bool InputMap::Down(const std::string& name, bool* known, int player) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && player >= 0 && player < kMaxPads && m_state[static_cast<size_t>(i * kMaxPads + player)].down;
	}

	bool InputMap::Pressed(const std::string& name, bool* known, int player) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && player >= 0 && player < kMaxPads && m_state[static_cast<size_t>(i * kMaxPads + player)].pressed;
	}

	bool InputMap::Released(const std::string& name, bool* known, int player) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && player >= 0 && player < kMaxPads && m_state[static_cast<size_t>(i * kMaxPads + player)].released;
	}

	float InputMap::Axis(const std::string& name, bool* known, int player) const {
		const int i = Find(name);
		if (known) *known = i >= 0;
		return i >= 0 && player >= 0 && player < kMaxPads ? m_state[static_cast<size_t>(i * kMaxPads + player)].value : 0.0f;
	}
}
