#pragma once
// Editor "Input Actions" window body (EditorWorkspace owns the dockable window and its Window menu entry).
//
// Two tabs. Project edits the active project's action map (projects/<name>/input.json, found through its descriptor).
// Editor edits the engine's fly-camera map (config/editor_input.json, packaged with the editor). Each tab works on one
// InputActionsModel file: add, rename and remove actions; add and remove bindings; Listen captures the next key, mouse
// button or pad input; axes get a deadzone and invert; the issue list shows errors and shared inputs. Every edit is one
// undo step on the editor's stack. Save writes the file (stable formatting); saving the Editor tab reloads the live fly
// map. A file that fails to parse is shown read-only: Save refuses to overwrite it.
//
// The live test view samples the same actions the game samples (player 0), so a binding can be tried without play.
#include "../editor/InputActionsModel.h"
#include "../editor/InspectorUndo.h"
#include "../scripting/LuaInput.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class InputActionsWindow {
public:
	struct Host {
		std::string projectDescriptor;                 // "projects/<name>/project.json" of the active project; "" for none
		nox::editor::UndoPush undo;                    // null while playing: edits apply without an undo step
		std::function<void()> reloadEditorInput;       // called after the Editor tab's file is saved
	};

	// Draws the window contents (the caller owns ImGui::Begin / End).
	void Draw(const Host& host);

private:
	struct Doc {
		std::string logicalPath;                       // "projects/<name>/input.json" or "config/editor_input.json"
		std::string physicalPath;                      // where Save writes; empty when the file has no source path
		bool loadFailed = false;                       // the file exists but does not parse: read-only
		bool dirty = false;
		nox::editor::InputActionsModel model;
		NoxLua::InputMap live;                         // parsed from model.Serialize(); sampled for the test view and Listen
		std::vector<nox::editor::InputIssue> issues;
		std::string selected;                          // action shown on the right
		std::string status;                            // last edit / save message
		std::string newName;                           // add-action name
		std::string renameDraft;                       // rename field, reset when the selection changes
		std::string renameFor;
		std::string bindingDraft;                      // add-binding text, shared by the binding lists
		bool deadzoneActive = false;                   // the deadzone slider is being dragged
		float deadzoneDraft = 0.0f;
		std::string listenAction;                      // action the capture is for
		std::string listenField;                       // keys_positive / keys_negative: the side a key capture goes to
	};

	static std::shared_ptr<Doc> OpenDoc(const std::string& logicalPath);
	// Re-reads the file (also used by Revert). Sets loadFailed when the text does not parse.
	static void ReadFromDisk(Doc& doc);
	static void Refresh(Doc& doc);
	// Applies one edit: snapshot, op, validation, undo step. False (reason in doc.status) when op fails.
	bool Edit(const std::shared_ptr<Doc>& doc, const nox::editor::UndoPush& undo, const std::string& label,
		const std::function<bool(nox::editor::InputActionsModel&, std::string&)>& op);
	bool Save(Doc& doc, const Host& host);

	void DrawDoc(const std::shared_ptr<Doc>& doc, const Host& host);
	void DrawActionList(const std::shared_ptr<Doc>& doc, const Host& host);
	void DrawActionDetails(const std::shared_ptr<Doc>& doc, const Host& host);
	void DrawBindingList(const std::shared_ptr<Doc>& doc, const Host& host, const char* field, const char* label,
		const char* listenSide);
	void DrawLiveTest(const std::shared_ptr<Doc>& doc);
	void DrawIssues(const std::shared_ptr<Doc>& doc);
	void ProcessListen(const std::shared_ptr<Doc>& doc, const nox::editor::UndoPush& undo);

	std::string m_projectDescriptor;
	std::string m_projectError;
	std::shared_ptr<Doc> m_project;
	std::shared_ptr<Doc> m_editor;
};
