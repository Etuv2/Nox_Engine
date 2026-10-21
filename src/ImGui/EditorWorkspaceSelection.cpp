// Editor selection commands: the selection set, click / Ctrl / Shift selection, marquee select in the Scene
// viewport, and the multi-node copy, cut, paste, duplicate, delete and select-all steps. Each command that changes
// the scene is one undo step (UndoBatch). The single-node paths in EditorWorkspace.cpp are unchanged.
#include "EditorWorkspace.h"

#include "../Core.h"
#include "../ImGuiInterface.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../editor/Clipboard.h"
#include "../editor/InspectorUndo.h"
#include "../editor/Selection.h"
#include <IMGUI/imgui.h>
#include <IMGUI/ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

using NodeList = std::vector<std::shared_ptr<SceneNode>>;

// Authored nodes below n (generated nodes are rebuilt on load and are not selectable).
void CollectAuthored(const std::shared_ptr<SceneNode>& n, NodeList& out)
{
	for (const auto& c : n->children) {
		if (!c || c->IsGenerated()) continue;
		out.push_back(c);
		CollectAuthored(c, out);
	}
}

const ImU32 kMarqueeFill = IM_COL32(255, 210, 90, 40);
const ImU32 kMarqueeLine = IM_COL32(255, 210, 90, 255);

} // namespace

void EditorWorkspace::ApplyMultiEdit(const NodeList& targets, const std::string& component, const std::string& property,
	const nox::reflect::PropValue& before, const nox::reflect::PropValue& after, bool discrete)
{
	if (targets.empty()) return;
	if (!discrete && !m_multiActive) {
		m_multiActive = true;
		m_multiComponent = component;
		m_multiProperty = property;
		m_multiLabel = component + "." + property;
		m_multiEdits.clear();
	}
	if (!discrete) m_multiEdits.resize(targets.size());
	// Each target gets the primary's edit merged into its own value: only the components the edit changed are written.
	nox::editor::UndoBatch batch;
	std::string error;
	bool allOk = true;
	for (size_t i = 0; i < targets.size(); ++i) {
		nox::reflect::PropValue current;
		if (!nox::reflect::GetProperty(*targets[i], component, property, current)) continue;
		const nox::reflect::PropValue merged = nox::editor::MergeEditedValue(current, before, after);
		const bool ok = discrete
			? nox::editor::SetPropertyUndoable(m_nodeTable, batch.Sink(), targets[i], component, property, merged, &error)
			: nox::editor::SetPropertyContinuous(m_multiEdits[i], m_nodeTable, targets[i], component, property, merged, &error);
		allOk = allOk && ok;
	}
	if (!allOk) ui::Toast(error.empty() ? "Value rejected by some nodes" : error, ui::ToastKind::Warning);
	if (discrete) batch.Commit(component + "." + property, UndoSink());
	NoteEdit();
}

void EditorWorkspace::EndMultiEdit()
{
	if (!m_multiActive) return;
	nox::editor::UndoBatch batch;
	for (nox::editor::PropertyEdit& edit : m_multiEdits) nox::editor::EndPropertyEdit(edit, m_nodeTable, batch.Sink());
	batch.Commit(m_multiLabel, UndoSink());
	m_multiEdits.clear();
	m_multiActive = false;
}

void EditorWorkspace::SyncSelection()
{
	// The window manager selection can change elsewhere (Lua, automation, undo, Core picking). A change that the set
	// does not know becomes the new selection; the primary of the set is what the window manager shows.
	const auto legacy = m_legacy.GetSelectedNode();
	if (legacy != m_selection.Primary()) {
		if (legacy && m_selection.Contains(legacy)) m_selection.Add(legacy);
		else m_selection.Set(legacy);
	}
	if (m_selection.Prune(RootNode())) m_legacy.SetSelectedNode(m_selection.Primary());
}

void EditorWorkspace::ClickSelect(const std::shared_ptr<SceneNode>& node, bool additive, bool range)
{
	if (range && node && m_selection.Primary()) {
		// Shift+click: the visible rows between the primary and node; node becomes primary.
		const NodeList span = nox::editor::RangeBetween(m_visibleRows, m_selection.Primary(), node);
		m_selection.SetAll(span);
		m_selection.Add(node);
	}
	else if (additive) {
		if (node) m_selection.Toggle(node);   // Ctrl+click on empty space keeps the selection
	}
	else {
		m_selection.Set(node);
	}
	m_legacy.SetSelectedNode(m_selection.Primary());
}

bool EditorWorkspace::NodeShortcutScope() const
{
	return m_hierarchyFocused || m_scene.focused;
}

void EditorWorkspace::CopySelection(bool cut)
{
	if (cut && m_core.IsPlaying()) return;   // cut deletes, which play mode does not do
	const auto root = RootNode();
	NodeList targets;
	for (const auto& n : m_selection.TopLevel()) if (n && n != root) targets.push_back(n);
	if (targets.empty()) return;

	std::vector<nlohmann::json> nodes;
	for (const auto& n : targets) {
		nlohmann::json j = m_core.SerializeNodeForEditor(n);   // prefab instances stay references
		if (j.is_object()) nodes.push_back(j);
	}
	if (nodes.empty()) return;
	m_clipboardNodes = nodes;
	// The OS clipboard carries the same document, so a copy can be pasted into another editor session.
	ImGui::SetClipboardText(nox::editor::MakeClipboardDocument(nodes).dump().c_str());
	ui::Toast((cut ? "Cut " : "Copied ") + std::to_string(nodes.size()) + (nodes.size() == 1 ? " node" : " nodes"), ui::ToastKind::Success);
	if (cut) DeleteSelectionWithUndo();
}

void EditorWorkspace::PasteFromClipboard()
{
	if (m_core.IsPlaying()) return;
	// The OS clipboard wins when it holds a document from the editor (another session or this one); other text
	// falls back to the copy made in this session.
	std::vector<nlohmann::json> nodes;
	const char* text = ImGui::GetClipboardText();
	if (!text || !nox::editor::ParseClipboardDocument(text, nodes)) nodes = m_clipboardNodes;
	if (nodes.empty()) return;

	// Copies go under the parent of the primary selection (the scene root when nothing is selected).
	const auto primary = m_selection.Primary();
	auto parent = primary ? primary->parentNode.lock() : nullptr;
	if (!parent) parent = RootNode();
	if (!parent) return;

	std::vector<std::string> siblings;
	for (const auto& c : parent->children) if (c) siblings.push_back(c->GetName());

	nox::editor::UndoBatch batch;
	const nox::editor::UndoPush sink = batch.Sink();
	NodeList created;
	std::string lastName;
	for (nlohmann::json j : nodes) {
		// Relative transforms are kept: each node's position is relative to its parent, and the copies share it.
		lastName = nox::editor::UniqueSiblingName(j.value("name", std::string("Node")), siblings);
		j["name"] = lastName;
		siblings.push_back(lastName);
		if (auto node = nox::editor::CreateNodeUndoable(m_nodeTable, CoreNodeOps(), sink, "Paste", j, parent)) created.push_back(node);
	}
	batch.Commit(created.size() == 1 ? "Paste " + lastName : "Paste " + std::to_string(created.size()) + " nodes", UndoSink());
	if (created.empty()) return;
	m_selection.SetAll(created);
	m_legacy.SetSelectedNode(m_selection.Primary());
	NoteEdit();
}

void EditorWorkspace::SelectAllTopLevel()
{
	const auto root = RootNode();
	if (!root) return;
	NodeList top;
	for (const auto& c : root->children) if (c && !c->IsGenerated()) top.push_back(c);
	m_selection.SetAll(top);
	m_legacy.SetSelectedNode(m_selection.Primary());
}

void EditorWorkspace::DeleteSelectionWithUndo()
{
	if (m_core.IsPlaying()) return;
	const auto root = RootNode();
	NodeList targets;
	for (const auto& n : m_selection.TopLevel()) if (n && n != root) targets.push_back(n);
	if (targets.empty()) return;

	nox::editor::UndoBatch batch;
	const nox::editor::UndoPush sink = batch.Sink();
	bool changed = false;
	for (const auto& n : targets) {
		// Each subtree is deleted with its own step; the batch makes them one undo entry.
		changed = nox::editor::DeleteNodeUndoable(m_nodeTable, CoreNodeOps(), sink, n) || changed;
	}
	batch.Commit(targets.size() == 1 ? "Delete " + targets[0]->GetName() : "Delete " + std::to_string(targets.size()) + " nodes", UndoSink());
	if (changed) NoteEdit();
	SelectNode(nullptr);
}

void EditorWorkspace::DuplicateSelectionWithUndo()
{
	if (m_core.IsPlaying()) return;
	const auto root = RootNode();
	NodeList targets;
	for (const auto& n : m_selection.TopLevel()) if (n && n != root) targets.push_back(n);
	if (targets.empty()) return;

	nox::editor::UndoBatch batch;
	const nox::editor::UndoPush sink = batch.Sink();
	NodeList copies;
	for (const auto& n : targets) {
		if (auto copy = m_core.DuplicateNode(n)) {
			nox::editor::RecordCreatedNode(m_nodeTable, CoreNodeOps(), sink, "Duplicate " + n->GetName(), copy);
			copies.push_back(copy);
		}
	}
	batch.Commit(targets.size() == 1 ? "Duplicate " + targets[0]->GetName() : "Duplicate " + std::to_string(targets.size()) + " nodes", UndoSink());
	if (copies.empty()) return;
	NoteEdit();
	m_selection.SetAll(copies);
	m_legacy.SetSelectedNode(m_selection.Primary());
}

// Click, Ctrl+click and marquee selection in the Scene viewport. A press on empty space (not on the gizmo or an
// authoring handle) starts a rectangle; released after moving a few pixels it selects the authored nodes whose origin
// lies inside it (Ctrl adds to the selection), released in place it is a click.
void EditorWorkspace::UpdateMarquee(const ImGuiIO& io, ImDrawList* dl)
{
	auto overHandle = [&] {
		for (const ViewportHandle& h : m_handles) {
			ImVec2 p;
			if (ProjectToViewport(m_scene, h.world, p) && std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 10.0f) return true;
		}
		return false;
	};
	if (!m_marquee && m_scene.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() &&
		!ImGuizmo::IsOver() && m_dragHandle < 0 && !m_core.IsEditorCameraActive() && !overHandle()) {
		m_marquee = true;
		m_marqueeStart = io.MousePos;
	}
	if (!m_marquee) return;

	const float dx = io.MousePos.x - m_marqueeStart.x;
	const float dy = io.MousePos.y - m_marqueeStart.y;
	const bool dragged = std::hypot(dx, dy) > 4.0f;
	if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (dragged) {
			dl->AddRectFilled(m_marqueeStart, io.MousePos, kMarqueeFill);
			dl->AddRect(m_marqueeStart, io.MousePos, kMarqueeLine, 0.0f, 0, 1.5f);
		}
		return;
	}
	m_marquee = false;

	if (!dragged) {
		// A click: pick the node under the mouse (Ctrl toggles it, a click on nothing clears the selection).
		if (!m_scene.hovered || m_core.IsPlaying()) return;
		const ImVec2 scale = io.DisplayFramebufferScale;
		const int x = static_cast<int>((io.MousePos.x - m_scene.pos.x) * scale.x);
		const int y = static_cast<int>((io.MousePos.y - m_scene.pos.y) * scale.y);
		ClickSelect(m_core.PickNode(x, y, m_scene.texWidth, m_scene.texHeight), io.KeyCtrl, false);
		return;
	}

	// Marquee: every authored node whose origin projects inside the rectangle.
	const ImVec2 lo(std::min(m_marqueeStart.x, io.MousePos.x), std::min(m_marqueeStart.y, io.MousePos.y));
	const ImVec2 hi(std::max(m_marqueeStart.x, io.MousePos.x), std::max(m_marqueeStart.y, io.MousePos.y));
	NodeList all;
	if (const auto root = RootNode()) CollectAuthored(root, all);
	// A node with a model is selected when its world bounding box, projected to the viewport, overlaps the rectangle.
	// Other nodes (lights, cameras, empty nodes) are selected when their origin lies inside it.
	auto inRect = [&](const std::shared_ptr<SceneNode>& n) {
		if (!n->GetModel()) {
			ImVec2 p;
			return ProjectToViewport(m_scene, n->GetWorldPosition(), p) && p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y;
		}
		const auto [bmin, bmax] = n->GetBoundingBox();
		const glm::mat4 world = n->GetWorldPosition4x4();
		const float big = std::numeric_limits<float>::max();
		float minX = big, minY = big, maxX = -big, maxY = -big;
		bool any = false;
		for (int i = 0; i < 8; ++i) {
			const glm::vec3 corner((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
			ImVec2 p;
			if (!ProjectToViewport(m_scene, glm::vec3(world * glm::vec4(corner, 1.0f)), p)) continue;
			any = true;
			minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
			minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
		}
		return any && maxX >= lo.x && minX <= hi.x && maxY >= lo.y && minY <= hi.y;
	};
	NodeList inside;
	for (const auto& n : all) if (inRect(n)) inside.push_back(n);
	if (io.KeyCtrl) {
		for (const auto& n : inside) m_selection.Add(n);
	}
	else {
		m_selection.SetAll(inside);
	}
	m_legacy.SetSelectedNode(m_selection.Primary());
}
