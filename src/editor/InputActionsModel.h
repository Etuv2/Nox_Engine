#pragma once
// Editor model of an action map file (projects/<name>/input.json, config/editor_input.json; format in
// scripting/LuaInput.h). The file is held as an ordered JSON document and edited one action at a time. No ImGui
// here: the Input Actions window draws it, and tests drive the same code.
//
// Formatting is stable: actions keep their file order, every action is written on one line, arrays stay inline,
// and the top level has one key per line. Saving twice without edits writes the same bytes.
//
// Validation runs the runtime parser (InputMap::Parse), so anything the engine would reject (unknown key or pad names,
// bad deadzones, buttons with axis fields) is an error here too. Two actions that use the same input are a warning:
// both fire, which is usually a mistake.
#include "json.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace nox::editor {

struct InputIssue {
	enum class Severity { Error, Warning };
	Severity severity = Severity::Error;
	std::string action;      // the action the issue is about; empty for file-level issues
	std::string message;
};

class InputActionsModel {
public:
	InputActionsModel() { Reset(); }
	// Parses the file text. False with `error` for invalid JSON, a non-object, or a format other than 1.
	// Duplicate action names are kept (the later one wins) and reported by Validate().
	bool Load(const std::string& text, std::string& error);
	// An empty map (format 1, no actions).
	void Reset();
	// The file text in the stable format described above (ends with a newline).
	std::string Serialize() const;
	// Writes Serialize() to a physical path. False with `error` when the file cannot be written.
	bool Save(const std::string& physicalPath, std::string& error) const;

	std::vector<std::string> ActionNames() const;
	bool HasAction(const std::string& name) const;
	// "button", "axis", or "" when the action does not exist.
	std::string ActionType(const std::string& name) const;
	// A copy of one action's object (null when it does not exist).
	nlohmann::ordered_json Action(const std::string& name) const;

	// Edits. Each returns false with `error` set and leaves the document unchanged.
	// AddAction: name is an identifier (letters, digits, underscore; not starting with a digit), type is button or axis.
	bool AddAction(const std::string& name, const std::string& type, std::string& error);
	bool RemoveAction(const std::string& name, std::string& error);
	bool RenameAction(const std::string& from, const std::string& to, std::string& error);
	// Adds one binding. field: keys, pad_buttons, mouse_buttons (button actions); keys_negative, keys_positive,
	// pad_axis (axis actions; pad_axis is a single value and replaces the old one). The value is checked against the
	// engine's names (SDL key names, SDL controller names, left/middle/right/x1/x2).
	bool AddBinding(const std::string& action, const std::string& field, const std::string& value, std::string& error);
	// Removes the binding at index from a list field (pad_axis: index 0 removes the field).
	bool RemoveBinding(const std::string& action, const std::string& field, size_t index, std::string& error);
	// Axis actions only. Deadzone in [0, 0.9); invert is the pad axis sign.
	bool SetDeadzone(const std::string& action, double value, std::string& error);
	bool SetInvert(const std::string& action, bool value, std::string& error);

	// Errors (the runtime parser's messages, duplicate names) and warnings (shared inputs). Empty when clean.
	std::vector<InputIssue> Validate() const;

	// The whole document, for undo steps: Restore(snapshot taken before an edit) undoes it.
	nlohmann::ordered_json Snapshot() const { return m_root; }
	void Restore(const nlohmann::ordered_json& doc) { m_root = doc; }

	// Action names that appeared more than once in the last loaded text.
	const std::vector<std::string>& DuplicateNames() const { return m_duplicates; }

private:
	nlohmann::ordered_json* FindAction(const std::string& name);
	const nlohmann::ordered_json* FindAction(const std::string& name) const;

	nlohmann::ordered_json m_root;
	std::vector<std::string> m_duplicates;
};

} // namespace nox::editor
