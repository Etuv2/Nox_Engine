#include "InputActionsWindow.h"

#include "../assets/AssetIO.h"
#include "../scripting/LuaProject.h"

#include <IMGUI/imgui.h>
#include <algorithm>
#include <map>

namespace {

constexpr const char* kEditorInputPath = "config/editor_input.json";
const ImVec4 kErrorColor(0.95f, 0.35f, 0.35f, 1.0f);
const ImVec4 kWarningColor(0.95f, 0.8f, 0.3f, 1.0f);
const ImVec4 kOkColor(0.45f, 0.85f, 0.5f, 1.0f);
const ImVec4 kDimColor(0.6f, 0.6f, 0.6f, 1.0f);

// Single-line text field backed by a std::string. ImGui edits a char buffer that persists per field (keyed by id), so
// typing survives frames; the string is written on every change, and re-read when it changed elsewhere (Revert,
// a cleared add field).
bool TextField(const char* id, std::string& value, size_t capacity) {
	struct Field {
		std::vector<char> buf;
		std::string synced;
	};
	static std::map<std::string, Field> fields;
	Field& f = fields[id];
	if (f.buf.size() != capacity || f.synced != value) {
		f.buf.assign(capacity, '\0');
		value.copy(f.buf.data(), std::min(value.size(), capacity - 1));
		f.synced = value;
	}
	const bool changed = ImGui::InputText(id, f.buf.data(), f.buf.size());
	if (changed) {
		value = f.buf.data();
		f.synced = value;
	}
	return changed;
}

} // namespace

std::shared_ptr<InputActionsWindow::Doc> InputActionsWindow::OpenDoc(const std::string& logicalPath) {
	auto doc = std::make_shared<Doc>();
	doc->logicalPath = logicalPath;
	if (const auto physical = AssetIO::SourcePath(logicalPath)) doc->physicalPath = physical->string();
	ReadFromDisk(*doc);
	return doc;
}

void InputActionsWindow::ReadFromDisk(Doc& doc) {
	doc.loadFailed = false;
	doc.dirty = false;
	doc.model.Reset();
	const auto text = AssetIO::ReadText(doc.logicalPath, nullptr);
	if (!text) {
		doc.status = "No file yet; Save creates it.";
	}
	else {
		std::string error;
		if (!doc.model.Load(*text, error)) {
			doc.loadFailed = true;
			doc.status = "The file does not parse (" + error + "). Fix it in a text editor, then Revert.";
			doc.model.Reset();
		}
		else {
			doc.status.clear();
		}
	}
	Refresh(doc);
}

void InputActionsWindow::Refresh(Doc& doc) {
	doc.issues = doc.model.Validate();
	std::vector<std::string> errors;
	doc.live.Parse(doc.model.Serialize(), errors);
}

bool InputActionsWindow::Edit(const std::shared_ptr<Doc>& doc, const nox::editor::UndoPush& undo, const std::string& label,
	const std::function<bool(nox::editor::InputActionsModel&, std::string&)>& op) {
	if (doc->loadFailed) {
		doc->status = "The file does not parse; Revert or fix it before editing.";
		return false;
	}
	const auto before = doc->model.Snapshot();
	std::string error;
	if (!op(doc->model, error)) {
		doc->status = error;
		return false;
	}
	const auto after = doc->model.Snapshot();
	doc->status.clear();
	if (before == after) return true;
	doc->dirty = true;
	Refresh(*doc);
	if (undo) {
		// Weak references: a step left on the stack does nothing once its document was replaced (project switch).
		const std::weak_ptr<Doc> weak = doc;
		undo(label,
			[weak, before] {
				if (auto d = weak.lock()) { d->model.Restore(before); d->dirty = true; Refresh(*d); }
			},
			[weak, after] {
				if (auto d = weak.lock()) { d->model.Restore(after); d->dirty = true; Refresh(*d); }
			});
	}
	return true;
}

bool InputActionsWindow::Save(Doc& doc, const Host& host) {
	if (doc.loadFailed) {
		doc.status = "Not saved: the file does not parse. Fix it in a text editor, then Revert.";
		return false;
	}
	if (doc.physicalPath.empty()) {
		doc.status = "Not saved: " + doc.logicalPath + " has no writable source file.";
		return false;
	}
	std::string error;
	if (!doc.model.Save(doc.physicalPath, error)) {
		doc.status = "Not saved: " + error;
		return false;
	}
	doc.dirty = false;
	doc.status = "Saved " + doc.logicalPath;
	if (doc.logicalPath == kEditorInputPath && host.reloadEditorInput) host.reloadEditorInput();
	return true;
}

void InputActionsWindow::Draw(const Host& host) {
	if (host.projectDescriptor != m_projectDescriptor) {
		m_projectDescriptor = host.projectDescriptor;
		m_project.reset();
		m_projectError.clear();
		if (!m_projectDescriptor.empty()) {
			NoxLua::ProjectDescriptor descriptor;
			std::string error;
			if (NoxLua::ReadProjectDescriptor(m_projectDescriptor, descriptor, error)) {
				m_project = OpenDoc(descriptor.inputPath);
			}
			else {
				m_projectError = error;
			}
		}
	}
	if (!m_editor) m_editor = OpenDoc(kEditorInputPath);

	if (ImGui::BeginTabBar("##input_actions_tabs")) {
		if (ImGui::BeginTabItem("Project")) {
			if (m_project) {
				DrawDoc(m_project, host);
			}
			else if (!m_projectError.empty()) {
				ImGui::TextColored(kErrorColor, "The project descriptor could not be read: %s", m_projectError.c_str());
			}
			else {
				ImGui::TextDisabled("Open a project to edit its input.json.");
			}
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Editor")) {
			DrawDoc(m_editor, host);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void InputActionsWindow::DrawDoc(const std::shared_ptr<Doc>& doc, const Host& host) {
	// Listen capture reads the devices once per frame (the same sampling the game uses).
	doc->live.Sample(true);
	ProcessListen(doc, host.undo);

	ImGui::TextDisabled("%s", doc->logicalPath.c_str());
	if (doc->dirty) {
		ImGui::SameLine();
		ImGui::TextColored(kWarningColor, "unsaved");
	}
	if (ImGui::Button("Save")) Save(*doc, host);
	ImGui::SameLine();
	if (ImGui::Button("Revert")) {
		ReadFromDisk(*doc);
		doc->selected.clear();
	}
	if (!doc->status.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", doc->status.c_str());
	}
	ImGui::Separator();

	const float issuesHeight = 110.0f;
	ImGui::BeginChild("##ia_main", ImVec2(0.0f, -issuesHeight), false);
	DrawActionList(doc, host);
	ImGui::SameLine();
	ImGui::BeginChild("##ia_details", ImVec2(0.0f, 0.0f), false);
	DrawActionDetails(doc, host);
	ImGui::EndChild();
	ImGui::EndChild();
	DrawIssues(doc);
}

void InputActionsWindow::DrawActionList(const std::shared_ptr<Doc>& doc, const Host& host) {
	ImGui::BeginChild("##ia_list", ImVec2(170.0f, 0.0f), true);
	for (const std::string& name : doc->model.ActionNames()) {
		const bool axis = doc->model.ActionType(name) == "axis";
		const std::string label = name + (axis ? "  (axis)" : "");
		if (ImGui::Selectable(label.c_str(), doc->selected == name)) doc->selected = name;
	}
	ImGui::Separator();
	ImGui::SetNextItemWidth(-1.0f);
	TextField("##ia_new_name", doc->newName, 64);
	if (ImGui::Button("+ Button", ImVec2(-1.0f, 0.0f))) {
		const std::string name = doc->newName;
		if (Edit(doc, host.undo, "Add action " + name, [name](auto& m, auto& e) { return m.AddAction(name, "button", e); })) {
			doc->selected = name;
			doc->newName.clear();
		}
	}
	if (ImGui::Button("+ Axis", ImVec2(-1.0f, 0.0f))) {
		const std::string name = doc->newName;
		if (Edit(doc, host.undo, "Add action " + name, [name](auto& m, auto& e) { return m.AddAction(name, "axis", e); })) {
			doc->selected = name;
			doc->newName.clear();
		}
	}
	ImGui::EndChild();
}

void InputActionsWindow::DrawActionDetails(const std::shared_ptr<Doc>& doc, const Host& host) {
	if (doc->selected.empty() || !doc->model.HasAction(doc->selected)) {
		ImGui::TextDisabled("Select an action, or add one with the name field.");
		return;
	}
	const std::string name = doc->selected;
	const bool axis = doc->model.ActionType(name) == "axis";
	if (doc->renameFor != name) {
		doc->renameFor = name;
		doc->renameDraft = name;
	}

	ImGui::Text("Name");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	TextField("##ia_rename", doc->renameDraft, 64);
	// Renames when the field is confirmed (Enter or focus loss), so a half-typed name is not applied.
	if (ImGui::IsItemDeactivatedAfterEdit() && doc->renameDraft != name) {
		const std::string to = doc->renameDraft;
		if (Edit(doc, host.undo, "Rename " + name, [name, to](auto& m, auto& e) { return m.RenameAction(name, to, e); })) {
			doc->selected = to;
			doc->renameFor = to;
			return;
		}
		doc->renameDraft = name;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%s", axis ? "axis" : "button");
	ImGui::SameLine(ImGui::GetWindowWidth() - 130.0f);
	if (ImGui::Button("Remove action")) {
		if (Edit(doc, host.undo, "Remove action " + name, [name](auto& m, auto& e) { return m.RemoveAction(name, e); })) {
			doc->selected.clear();
			return;
		}
	}
	ImGui::Separator();

	if (!axis) {
		DrawBindingList(doc, host, "keys", "Keys", "");
		DrawBindingList(doc, host, "pad_buttons", "Pad buttons", "");
		DrawBindingList(doc, host, "mouse_buttons", "Mouse buttons", "");
	}
	else {
		DrawBindingList(doc, host, "keys_negative", "Keys (negative)", "keys_negative");
		DrawBindingList(doc, host, "keys_positive", "Keys (positive)", "keys_positive");
		const nox::editor::InputActionsModel& model = doc->model;
		const nlohmann::ordered_json action = model.Action(name);
		ImGui::Text("Pad axis");
		ImGui::SameLine(110.0f);
		if (action.contains("pad_axis")) {
			ImGui::Text("%s", action["pad_axis"].get<std::string>().c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("x##pad_axis")) {
				Edit(doc, host.undo, "Clear pad axis of " + name, [name](auto& m, auto& e) { return m.RemoveBinding(name, "pad_axis", 0, e); });
			}
		}
		else {
			ImGui::TextDisabled("(none)");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Listen##pad_axis")) {
			doc->listenAction = name;
			doc->listenField = "pad_axis";
			doc->live.BeginListen();
		}

		// Deadzone: one undo step when the slider is released.
		const float stored = static_cast<float>(action.value("deadzone", 0.15));
		float value = doc->deadzoneActive ? doc->deadzoneDraft : stored;
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderFloat("Deadzone", &value, 0.0f, 0.89f, "%.2f")) {
			doc->deadzoneActive = true;
			doc->deadzoneDraft = value;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			const double dz = static_cast<double>(doc->deadzoneDraft);
			Edit(doc, host.undo, "Deadzone of " + name, [name, dz](auto& m, auto& e) { return m.SetDeadzone(name, dz, e); });
			doc->deadzoneActive = false;
		}

		const bool invert = action.value("invert", false);
		bool invertDraft = invert;
		if (ImGui::Checkbox("Invert pad axis", &invertDraft) && invertDraft != invert) {
			Edit(doc, host.undo, "Invert " + name, [name, invertDraft](auto& m, auto& e) { return m.SetInvert(name, invertDraft, e); });
		}
	}

	if (doc->live.Listening()) {
		ImGui::Separator();
		ImGui::TextColored(kWarningColor, "%s",
			doc->model.ActionType(doc->listenAction) == "axis" ? "Press a key, pad button or pad axis (Cancel stops listening)"
				: "Press a key, mouse button or pad button (Cancel stops listening)");
		ImGui::SameLine();
		if (ImGui::SmallButton("Cancel")) {
			doc->live.CancelListen();
			doc->listenAction.clear();
			doc->listenField.clear();
		}
	}
	ImGui::Separator();
	DrawLiveTest(doc);
}

void InputActionsWindow::DrawBindingList(const std::shared_ptr<Doc>& doc, const Host& host, const char* field,
	const char* label, const char* listenSide) {
	const std::string name = doc->selected;
	const nlohmann::ordered_json action = doc->model.Action(name);
	ImGui::Text("%s", label);
	const std::string id = std::string("##") + field;
	const bool haveList = action.contains(field) && action[field].is_array();
	if (haveList) {
		const auto& list = action[field];
		for (size_t i = 0; i < list.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			ImGui::Text("   %s", list[i].get<std::string>().c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				Edit(doc, host.undo, std::string("Remove ") + field + " of " + name,
					[name, field, i](auto& m, auto& e) { return m.RemoveBinding(name, field, i, e); });
				ImGui::PopID();
				break;   // the list changed; draw the rest next frame
			}
			ImGui::PopID();
		}
	}
	ImGui::SetNextItemWidth(150.0f);
	ImGui::Indent(12.0f);
	TextField((id + "_text").c_str(), doc->bindingDraft, 128);
	ImGui::SameLine();
	if (ImGui::SmallButton(("Add" + id).c_str()) && !doc->bindingDraft.empty()) {
		const std::string value = doc->bindingDraft;
		if (Edit(doc, host.undo, std::string("Add ") + field + " to " + name,
			[name, field, value](auto& m, auto& e) { return m.AddBinding(name, field, value, e); })) {
			doc->bindingDraft.clear();
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(("Listen" + id).c_str())) {
		doc->listenAction = name;
		doc->listenField = listenSide;
		doc->live.BeginListen();
	}
	ImGui::Unindent(12.0f);
}

void InputActionsWindow::DrawLiveTest(const std::shared_ptr<Doc>& doc) {
	if (!ImGui::CollapsingHeader("Live test (player 0)", ImGuiTreeNodeFlags_DefaultOpen)) return;
	ImGui::TextDisabled("Shows what the game sees while this window is open.");
	for (const std::string& name : doc->model.ActionNames()) {
		ImGui::Text("%s", name.c_str());
		ImGui::SameLine(170.0f);
		if (doc->model.ActionType(name) == "axis") {
			const float v = doc->live.Axis(name);
			ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(180.0f, 0.0f), "");
			ImGui::SameLine();
			ImGui::Text("%+.2f", v);
		}
		else {
			const bool down = doc->live.Down(name);
			ImGui::TextColored(down ? kOkColor : kDimColor, "%s", down ? "pressed" : "-");
		}
	}
}

void InputActionsWindow::DrawIssues(const std::shared_ptr<Doc>& doc) {
	ImGui::BeginChild("##ia_issues", ImVec2(0.0f, 0.0f), true);
	if (doc->issues.empty()) {
		ImGui::TextColored(kOkColor, "No problems found.");
	}
	for (const nox::editor::InputIssue& issue : doc->issues) {
		const bool error = issue.severity == nox::editor::InputIssue::Severity::Error;
		ImGui::TextColored(error ? kErrorColor : kWarningColor, "%s %s", error ? "Error:" : "Warning:", issue.message.c_str());
	}
	ImGui::EndChild();
}

void InputActionsWindow::ProcessListen(const std::shared_ptr<Doc>& doc, const nox::editor::UndoPush& undo) {
	if (!doc->live.Listening()) return;
	NoxLua::InputMap::Captured captured;
	if (!doc->live.TakeCapture(captured)) return;
	const std::string action = doc->listenAction;
	doc->listenAction.clear();
	const std::string side = doc->listenField;
	doc->listenField.clear();
	if (!doc->model.HasAction(action)) return;
	const bool axis = doc->model.ActionType(action) == "axis";

	std::string field;
	switch (captured.binding.type) {
	case NoxLua::InputBinding::Type::Key:
		field = axis ? (side == "keys_negative" ? "keys_negative" : "keys_positive") : "keys";
		break;
	case NoxLua::InputBinding::Type::Mouse:
		field = axis ? std::string() : "mouse_buttons";
		break;
	case NoxLua::InputBinding::Type::PadButton:
		field = axis ? std::string() : "pad_buttons";
		break;
	case NoxLua::InputBinding::Type::PadAxis:
		field = axis ? "pad_axis" : std::string();
		break;
	}
	if (field.empty()) {
		doc->status = "That input does not fit " + action + " (" + (axis ? "an axis" : "a button") + ")";
		return;
	}
	const std::string value = captured.binding.name;
	if (Edit(doc, undo, "Bind " + value + " to " + action,
		[action, field, value](auto& m, auto& e) { return m.AddBinding(action, field, value, e); })) {
		doc->status = "Bound " + value + " to " + action;
	}
}
