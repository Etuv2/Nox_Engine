#include "InputActionsModel.h"

#include "scripting/LuaInput.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <utility>

namespace nox::editor {

namespace {

using json = nlohmann::ordered_json;

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

std::string Lower(std::string s) {
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// One-line JSON in the file's style: "{ \"k\": v, ... }", "[\"a\", \"b\"]", empty containers as "{}" and "[]".
std::string Inline(const json& v) {
	if (v.is_object()) {
		if (v.empty()) return "{}";
		std::string out = "{ ";
		bool first = true;
		for (const auto& item : v.items()) {
			if (!first) out += ", ";
			first = false;
			out += json(item.key()).dump() + ": " + Inline(item.value());
		}
		return out + " }";
	}
	if (v.is_array()) {
		if (v.empty()) return "[]";
		std::string out = "[";
		for (size_t i = 0; i < v.size(); ++i) {
			if (i) out += ", ";
			out += Inline(v[i]);
		}
		return out + "]";
	}
	return v.dump();
}

// Binding list fields by action kind.
bool IsButtonField(const std::string& field) {
	return field == "keys" || field == "pad_buttons" || field == "mouse_buttons";
}
bool IsAxisField(const std::string& field) {
	return field == "keys_negative" || field == "keys_positive" || field == "pad_axis";
}

// Checks a binding value against the engine's names. Returns false with `error` for an unknown name.
bool CheckBindingName(const std::string& field, const std::string& value, std::string& error) {
	bool known = false;
	if (field == "keys" || field == "keys_negative" || field == "keys_positive") {
		known = SDL_GetScancodeFromName(value.c_str()) != SDL_SCANCODE_UNKNOWN;
		if (!known) error = "unknown key name '" + value + "' (SDL key names, e.g. \"W\", \"Space\", \"Left Shift\")";
	}
	else if (field == "pad_buttons") {
		known = SDL_GameControllerGetButtonFromString(value.c_str()) != SDL_CONTROLLER_BUTTON_INVALID;
		if (!known) error = "unknown pad button '" + value + "' (SDL controller names, e.g. \"a\", \"dpup\")";
	}
	else if (field == "pad_axis") {
		known = SDL_GameControllerGetAxisFromString(value.c_str()) != SDL_CONTROLLER_AXIS_INVALID;
		if (!known) error = "unknown pad axis '" + value + "' (SDL controller names, e.g. \"leftx\", \"righttrigger\")";
	}
	else if (field == "mouse_buttons") {
		known = value == "left" || value == "middle" || value == "right" || value == "x1" || value == "x2";
		if (!known) error = "unknown mouse button '" + value + "' (left, middle, right, x1, x2)";
	}
	return known;
}

} // namespace

bool InputActionsModel::Load(const std::string& text, std::string& error) {
	m_duplicates.clear();
	std::vector<std::string> duplicates;
	std::set<std::string> seen;
	std::string rootKey;
	// Action names are the keys of the "actions" object (depth 2); a repeated one is reported, and the later one wins.
	const auto onEvent = [&](int depth, json::parse_event_t event, json& parsed) {
		if (event != json::parse_event_t::key || !parsed.is_string()) return true;
		if (depth == 1) {
			rootKey = parsed.get<std::string>();
		}
		else if (depth == 2 && rootKey == "actions") {
			const std::string name = parsed.get<std::string>();
			if (!seen.insert(name).second) duplicates.push_back(name);
		}
		return true;
	};
	const json doc = json::parse(text, onEvent, false);
	if (doc.is_discarded() || !doc.is_object()) {
		error = "the file is not a JSON object";
		return false;
	}
	if (!doc.contains("format") || !doc["format"].is_number_integer() || doc["format"].get<int>() != 1) {
		error = "format must be 1";
		return false;
	}
	if (doc.contains("actions") && !doc["actions"].is_object()) {
		error = "\"actions\" must be an object";
		return false;
	}
	m_root = doc;
	if (!m_root.contains("actions")) m_root["actions"] = json::object();
	m_duplicates = duplicates;
	return true;
}

void InputActionsModel::Reset() {
	m_root = json::object();
	m_root["format"] = 1;
	m_root["actions"] = json::object();
	m_duplicates.clear();
}

std::string InputActionsModel::Serialize() const {
	std::string out = "{\n";
	bool first = true;
	for (const auto& item : m_root.items()) {
		if (!first) out += ",\n";
		first = false;
		out += "  " + json(item.key()).dump() + ": ";
		const json& value = item.value();
		if (item.key() == "actions" && value.is_object() && !value.empty()) {
			out += "{\n";
			bool firstAction = true;
			for (const auto& action : value.items()) {
				if (!firstAction) out += ",\n";
				firstAction = false;
				out += "    " + json(action.key()).dump() + ": " + Inline(action.value());
			}
			out += "\n  }";
		}
		else {
			out += Inline(value);
		}
	}
	out += "\n}\n";
	return out;
}

bool InputActionsModel::Save(const std::string& physicalPath, std::string& error) const {
	std::ofstream file(physicalPath, std::ios::binary | std::ios::trunc);
	if (!file) {
		error = "cannot open " + physicalPath + " for writing";
		return false;
	}
	const std::string text = Serialize();
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!file) {
		error = "writing " + physicalPath + " failed";
		return false;
	}
	return true;
}

std::vector<std::string> InputActionsModel::ActionNames() const {
	std::vector<std::string> names;
	for (const auto& item : m_root["actions"].items()) names.push_back(item.key());
	return names;
}

bool InputActionsModel::HasAction(const std::string& name) const {
	return FindAction(name) != nullptr;
}

std::string InputActionsModel::ActionType(const std::string& name) const {
	const json* action = FindAction(name);
	if (!action) return std::string();
	return action->value("type", std::string("button"));
}

json InputActionsModel::Action(const std::string& name) const {
	const json* action = FindAction(name);
	return action ? *action : json();
}

json* InputActionsModel::FindAction(const std::string& name) {
	auto& actions = m_root["actions"];
	const auto it = actions.find(name);
	return it == actions.end() ? nullptr : &it.value();
}

const json* InputActionsModel::FindAction(const std::string& name) const {
	const auto& actions = m_root["actions"];
	const auto it = actions.find(name);
	return it == actions.end() ? nullptr : &it.value();
}

bool InputActionsModel::AddAction(const std::string& name, const std::string& type, std::string& error) {
	if (!IsIdentifier(name)) {
		error = "action name '" + name + "' must be letters, digits or underscores, and not start with a digit";
		return false;
	}
	if (type != "button" && type != "axis") {
		error = "action type must be \"button\" or \"axis\"";
		return false;
	}
	if (HasAction(name)) {
		error = "an action named '" + name + "' already exists";
		return false;
	}
	json action = json::object();
	action["type"] = type;
	m_root["actions"][name] = action;
	return true;
}

bool InputActionsModel::RemoveAction(const std::string& name, std::string& error) {
	if (!HasAction(name)) {
		error = "no action named '" + name + "'";
		return false;
	}
	m_root["actions"].erase(name);
	return true;
}

bool InputActionsModel::RenameAction(const std::string& from, const std::string& to, std::string& error) {
	if (!HasAction(from)) {
		error = "no action named '" + from + "'";
		return false;
	}
	if (from == to) return true;
	if (!IsIdentifier(to)) {
		error = "action name '" + to + "' must be letters, digits or underscores, and not start with a digit";
		return false;
	}
	if (HasAction(to)) {
		error = "an action named '" + to + "' already exists";
		return false;
	}
	// Rebuild the object so the renamed action keeps its place in the file.
	json renamed = json::object();
	for (const auto& item : m_root["actions"].items()) {
		renamed[item.key() == from ? to : item.key()] = item.value();
	}
	m_root["actions"] = renamed;
	return true;
}

bool InputActionsModel::AddBinding(const std::string& action, const std::string& field, const std::string& value,
	std::string& error) {
	json* a = FindAction(action);
	if (!a) {
		error = "no action named '" + action + "'";
		return false;
	}
	const bool isButton = a->value("type", std::string("button")) == "button";
	if (isButton ? !IsButtonField(field) : !IsAxisField(field)) {
		error = "'" + field + "' is not a binding of " + (isButton ? "a button" : "an axis") + " action";
		return false;
	}
	if (value.empty()) {
		error = "the binding name is empty";
		return false;
	}
	if (!CheckBindingName(field, value, error)) return false;
	if (field == "pad_axis") {
		(*a)["pad_axis"] = value;
		return true;
	}
	if (!a->contains(field)) (*a)[field] = json::array();
	json& list = (*a)[field];
	if (list.size() >= NoxLua::InputMap::kMaxBindingsPerAction) {
		error = "an action takes at most " + std::to_string(NoxLua::InputMap::kMaxBindingsPerAction) + " bindings in " + field;
		return false;
	}
	for (const auto& entry : list) {
		if (entry.is_string() && Lower(entry.get<std::string>()) == Lower(value)) {
			error = "'" + value + "' is already bound in " + field;
			return false;
		}
	}
	list.push_back(value);
	return true;
}

bool InputActionsModel::RemoveBinding(const std::string& action, const std::string& field, size_t index, std::string& error) {
	json* a = FindAction(action);
	if (!a) {
		error = "no action named '" + action + "'";
		return false;
	}
	if (field == "pad_axis") {
		if (index != 0 || !a->contains("pad_axis")) {
			error = "no pad_axis to remove";
			return false;
		}
		a->erase("pad_axis");
		return true;
	}
	if (!a->contains(field) || !(*a)[field].is_array() || index >= (*a)[field].size()) {
		error = "no binding " + std::to_string(index) + " in " + field;
		return false;
	}
	json& list = (*a)[field];
	list.erase(list.begin() + static_cast<json::difference_type>(index));
	return true;
}

bool InputActionsModel::SetDeadzone(const std::string& action, double value, std::string& error) {
	json* a = FindAction(action);
	if (!a) {
		error = "no action named '" + action + "'";
		return false;
	}
	if (a->value("type", std::string("button")) != "axis") {
		error = "only axis actions have a deadzone";
		return false;
	}
	if (!(value >= 0.0 && value < 0.9)) {
		error = "deadzone must be in [0, 0.9)";
		return false;
	}
	(*a)["deadzone"] = value;
	return true;
}

bool InputActionsModel::SetInvert(const std::string& action, bool value, std::string& error) {
	json* a = FindAction(action);
	if (!a) {
		error = "no action named '" + action + "'";
		return false;
	}
	if (a->value("type", std::string("button")) != "axis") {
		error = "only axis actions can be inverted";
		return false;
	}
	(*a)["invert"] = value;
	return true;
}

std::vector<InputIssue> InputActionsModel::Validate() const {
	std::vector<InputIssue> issues;
	for (const std::string& name : m_duplicates) {
		issues.push_back({ InputIssue::Severity::Error, name, "action '" + name + "' is defined more than once; the later definition is used" });
	}
	// The runtime parser is the authority on names and field rules.
	NoxLua::InputMap parsed;
	std::vector<std::string> errors;
	parsed.Parse(Serialize(), errors);
	for (const std::string& message : errors) {
		issues.push_back({ InputIssue::Severity::Error, std::string(), message });
	}
	// Shared inputs: each input (case-insensitive) with the actions that use it.
	struct Users {
		std::string label;
		std::vector<std::string> actions;
	};
	std::map<std::string, Users> users;
	const auto use = [&](const std::string& actionName, const std::string& kind, const std::string& value) {
		const std::string key = kind + ":" + Lower(value);
		Users& u = users[key];
		if (u.label.empty()) u.label = value;
		if (std::find(u.actions.begin(), u.actions.end(), actionName) == u.actions.end()) u.actions.push_back(actionName);
	};
	for (const auto& item : m_root["actions"].items()) {
		const json& a = item.value();
		if (!a.is_object()) continue;
		for (const char* field : { "keys", "keys_negative", "keys_positive" }) {
			if (!a.contains(field) || !a[field].is_array()) continue;
			for (const auto& v : a[field]) if (v.is_string()) use(item.key(), "key", v.get<std::string>());
		}
		if (a.contains("mouse_buttons") && a["mouse_buttons"].is_array()) {
			for (const auto& v : a["mouse_buttons"]) if (v.is_string()) use(item.key(), "mouse", v.get<std::string>());
		}
		if (a.contains("pad_buttons") && a["pad_buttons"].is_array()) {
			for (const auto& v : a["pad_buttons"]) if (v.is_string()) use(item.key(), "pad", v.get<std::string>());
		}
		if (a.contains("pad_axis") && a["pad_axis"].is_string()) use(item.key(), "axis", a["pad_axis"].get<std::string>());
	}
	for (const auto& entry : users) {
		const Users& u = entry.second;
		if (u.actions.size() < 2) continue;
		std::string names;
		for (const std::string& n : u.actions) names += (names.empty() ? "" : ", ") + ("'" + n + "'");
		issues.push_back({ InputIssue::Severity::Warning, u.actions.front(),
			"input " + u.label + " is used by " + names + " (all of them fire)" });
	}
	return issues;
}

} // namespace nox::editor
