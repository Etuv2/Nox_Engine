#include "EditorExtensions.h"

#include "UiIconAtlas.h"
#include <map>

namespace {
	std::vector<EditorPanel>& PanelList()
	{
		static std::vector<EditorPanel> panels;
		return panels;
	}
	std::vector<std::pair<std::string, EditorExtensions::ViewportOverlay>> g_viewportOverlays;
	EditorExtensions::ViewportOverlay g_viewportOverlay;   // draws all of g_viewportOverlays
	EditorExtensions::ToolbarIconResolver g_toolbarIconResolver;
}

namespace EditorExtensions {
	void RegisterPanel(EditorPanel panel)
	{
		for (EditorPanel& p : PanelList()) {
			if (p.id == panel.id) { p = std::move(panel); return; }
		}
		PanelList().push_back(std::move(panel));
	}

	const std::vector<EditorPanel>& Panels() { return PanelList(); }

	void AddViewportOverlay(const std::string& id, ViewportOverlay overlay)
	{
		bool replaced = false;
		for (auto& entry : g_viewportOverlays) {
			if (entry.first == id) { entry.second = std::move(overlay); replaced = true; break; }
		}
		if (!replaced) g_viewportOverlays.emplace_back(id, std::move(overlay));
		g_viewportOverlay = [](EditorHost& host, ImDrawList* drawList) {
			for (const auto& entry : g_viewportOverlays) {
				if (entry.second) entry.second(host, drawList);
			}
		};
	}
	void SetViewportOverlay(ViewportOverlay overlay) { AddViewportOverlay("default", std::move(overlay)); }
	const ViewportOverlay& GetViewportOverlay() { return g_viewportOverlay; }

	void SetToolbarIconResolver(ToolbarIconResolver resolver) { g_toolbarIconResolver = std::move(resolver); }

	std::string ResolveToolbarIcon(const char* role)
	{
		if (!g_toolbarIconResolver || !role) return std::string();
		return g_toolbarIconResolver(role);
	}
}

// Thin adapters over the Solace kit (src/editor/solace/SolaceUI.h).
namespace ui {
	namespace {
		bool g_gridOpen = false;
	}
	void Grid(const char* id) { g_gridOpen = solace::BeginPropertyGrid(id); }
	void Label(const char* label) {
		if (g_gridOpen) { solace::PropertyLabel(label); return; }
		ImGui::TextUnformatted(label);
		ImGui::SameLine(140.0f);
		ImGui::SetNextItemWidth(-1);
	}
	void EndGrid() { if (g_gridOpen) solace::EndPropertyGrid(); g_gridOpen = false; }
	bool Btn(const char* label, bool primary) { return solace::Button(label, primary ? solace::ButtonKind::Primary : solace::ButtonKind::Secondary); }
	bool Tool(const char* id, solace::Icon icon, const char* label, bool active, bool enabled, const char* tip) {
		return solace::ToolbarButton(id, icon, label, active, enabled, tip);
	}
	bool ToolIcon(const char* id, const std::string& atlasId, solace::Icon fallback, const char* label, bool active,
		bool enabled, const char* tip) {
		if (atlasId.empty() || !UiIconAtlas::Get().Has(atlasId)) return Tool(id, fallback, label, active, enabled, tip);
		ImGui::PushID(id);
		const ImGuiStyle& st = ImGui::GetStyle();
		const float h = ImGui::GetFrameHeight();
		const float icon = h * 0.5f;
		const float pad = st.FramePadding.x + 2.0f;
		const float gap = 6.0f;
		const bool hasText = label && *label;
		const ImVec2 text = hasText ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
		const float w = pad + icon + (hasText ? gap + text.x : 0.0f) + pad;
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + w, min.y + h);
		ImGui::BeginDisabled(!enabled);
		const bool clicked = ImGui::InvisibleButton("##tool_icon", ImVec2(w, h));
		ImGui::EndDisabled();
		const bool hovered = enabled && ImGui::IsItemHovered();
		if (tip && hovered) ImGui::SetTooltip("%s", tip);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (active || hovered) {
			dl->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), st.FrameRounding);
		}
		ImVec4 fg = st.Colors[ImGuiCol_Text];
		if (!(active || hovered)) fg.w *= 0.7f;
		fg.w *= enabled ? 1.0f : 0.4f;
		const ImU32 fgCol = ImGui::GetColorU32(fg);
		UiIconAtlas::Get().DrawCentered(dl, atlasId, ImVec2(min.x + pad + icon * 0.5f, min.y + h * 0.5f), icon, fgCol);
		if (hasText) dl->AddText(ImVec2(min.x + pad + icon + gap, min.y + (h - text.y) * 0.5f), fgCol, label);
		ImGui::PopID();
		return clicked && enabled;
	}
	bool Section(const char* title, bool defaultOpen) {
		static std::map<std::string, bool> open;
		auto it = open.find(title);
		if (it == open.end()) it = open.emplace(title, defaultOpen).first;
		return solace::SectionHeader((std::string("##sec_") + title).c_str(), title, &it->second);
	}
	bool Seg(const char* id, int* value, std::initializer_list<std::pair<const char*, int>> items) {
		std::vector<const char*> labels;
		int current = 0, i = 0;
		for (const auto& it : items) { labels.push_back(it.first); if (it.second == *value) current = i; ++i; }
		const int next = solace::SegmentedControl(id, labels.data(), static_cast<int>(labels.size()), current);
		if (next != current && next >= 0 && next < static_cast<int>(labels.size())) { *value = (items.begin() + next)->second; return true; }
		return false;
	}
	void Toast(const std::string& text, ToastKind kind) {
		const solace::Status st = kind == ToastKind::Success ? solace::Status::Success : kind == ToastKind::Warning ? solace::Status::Warning :
			kind == ToastKind::Error ? solace::Status::Error : solace::Status::Info;
		solace::PushToast(text.c_str(), nullptr, st);
	}
	void Progress(float p, float width) { if (p < 0.0f) solace::IndeterminateProgressBar(width); else solace::ProgressBar(p, width); }
}
