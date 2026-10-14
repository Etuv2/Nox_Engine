#include "InspectorWidgets.h"

#include <IMGUI/imgui.h>
#include <IMGUI/imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../SceneNode.h"

namespace nox::editor {

using nox::reflect::PropertyDesc;
using nox::reflect::PropType;
using nox::reflect::PropValue;

namespace {

constexpr float kDefaultSpeed = 0.01f;

bool HasRange(const PropertyDesc& prop)
{
	return prop.minValue < prop.maxValue;
}

float Speed(const PropertyDesc& prop)
{
	return prop.step > 0.0 ? static_cast<float>(prop.step) : kDefaultSpeed;
}

// Extension match for AssetPath filters (case-insensitive, e.g. ".glb").
bool MatchesFilter(const std::string& path, const std::vector<std::string>& filter)
{
	if (filter.empty()) return true;
	const size_t dot = path.find_last_of('.');
	std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (std::string f : filter) {
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (f == ext) return true;
	}
	return false;
}

// One text buffer shared by all text fields: only the focused field keeps its typed text, every other
// field refreshes the buffer from its value each frame.
struct TextBuffer {
	char data[1024] = {};
};

TextBuffer& Buffer()
{
	static TextBuffer buffer;
	return buffer;
}

PropertyWidgetResult TextField(const char* id, std::string& value, const std::vector<std::string>* pickList,
	const std::vector<std::string>& pickFilter, bool allowAssetDrop, bool allowNodeDrop)
{
	PropertyWidgetResult result;
	TextBuffer& buf = Buffer();
	const ImGuiID fieldId = ImGui::GetID(id);
	if (GImGui->ActiveId != fieldId) {
		std::snprintf(buf.data, sizeof(buf.data), "%s", value.c_str());
	}
	const bool showPicker = pickList && !pickList->empty();
	ImGui::SetNextItemWidth(showPicker ? -40.0f : -1.0f);
	const bool entered = ImGui::InputText(id, buf.data, sizeof(buf.data), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
	result.active = ImGui::IsItemActive();
	if (allowAssetDrop && ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragType)) {
			const char* path = static_cast<const char*>(payload->Data);
			if (path && *path && MatchesFilter(path, pickFilter)) {
				std::snprintf(buf.data, sizeof(buf.data), "%s", path);
				result.changed = true;
				result.final = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (allowNodeDrop && ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodeDragType)) {
			SceneNode* node = nullptr;
			if (payload->DataSize == sizeof(SceneNode*)) node = *static_cast<SceneNode* const*>(payload->Data);
			if (node) {
				std::snprintf(buf.data, sizeof(buf.data), "%s", node->GetName().c_str());
				result.changed = true;
				result.final = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (entered || deactivated) {
		result.final = true;
		if (std::string(buf.data) != value) {
			value = buf.data;
			result.changed = true;
		}
	}
	else if (result.changed) {
		value = buf.data;
	}
	if (showPicker) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo((std::string(id) + "##pick").c_str(), "", ImGuiComboFlags_NoPreview)) {
			for (const std::string& candidate : *pickList) {
				if (!MatchesFilter(candidate, pickFilter)) continue;
				if (ImGui::Selectable(candidate.c_str(), candidate == value)) {
					value = candidate;
					std::snprintf(buf.data, sizeof(buf.data), "%s", candidate.c_str());
					result.changed = true;
					result.final = true;
				}
			}
			ImGui::EndCombo();
		}
	}
	return result;
}

// Keeps an ImGui-style result in sync for widgets that return "changed" only on release.
PropertyWidgetResult FromImGui(bool changed)
{
	PropertyWidgetResult r;
	r.changed = changed;
	r.active = ImGui::IsItemActive();
	r.final = changed && !r.active;
	return r;
}

} // namespace

PropertyWidgetResult DrawPropertyWidget(const PropertyDesc& prop, PropValue& value,
	const std::vector<std::string>& assetPaths, const std::vector<std::string>& nodeNames, const char* id)
{
	PropertyWidgetResult result;
	if (!nox::reflect::ValueMatchesType(prop.type, value)) {
		ImGui::TextDisabled("(value type mismatch)");
		return result;
	}
	const bool readOnly = (prop.flags & nox::reflect::PropReadOnly) != 0;
	const bool ranged = HasRange(prop);
	const bool slider = (prop.flags & nox::reflect::PropSlider) != 0 && ranged;
	ImGui::PushID(id);
	ImGui::BeginDisabled(readOnly);
	ImGui::SetNextItemWidth(-1.0f);

	switch (prop.type) {
	case PropType::Bool: {
		bool b = std::get<bool>(value);
		if (ImGui::Checkbox("##v", &b)) {
			value = b;
			result.changed = result.final = true;
		}
		break;
	}
	case PropType::Int:
	case PropType::Enum: {
		if (prop.type == PropType::Enum) {
			int64_t index = std::get<int64_t>(value);
			const bool valid = index >= 0 && static_cast<size_t>(index) < prop.enumNames.size();
			const char* preview = valid ? prop.enumNames[static_cast<size_t>(index)].c_str() : "";
			if (ImGui::BeginCombo("##v", preview)) {
				for (size_t i = 0; i < prop.enumNames.size(); ++i) {
					const bool selected = static_cast<int64_t>(i) == index;
					if (ImGui::Selectable(prop.enumNames[i].c_str(), selected) && !selected) {
						value = static_cast<int64_t>(i);
						result.changed = result.final = true;
					}
				}
				ImGui::EndCombo();
			}
			break;
		}
		int i = static_cast<int>(std::get<int64_t>(value));
		const int lo = ranged ? static_cast<int>(std::ceil(prop.minValue)) : 0;
		const int hi = ranged ? static_cast<int>(std::floor(prop.maxValue)) : 0;
		const float speed = prop.step > 0.0 ? static_cast<float>(prop.step) : 1.0f;
		const bool changed = slider ? ImGui::SliderInt("##v", &i, lo, hi) : ImGui::DragInt("##v", &i, speed, lo, hi);
		if (changed) {
			value = static_cast<int64_t>(i);
			result = FromImGui(true);
		}
		else {
			result.active = ImGui::IsItemActive();
			if (!result.active && ImGui::IsItemDeactivatedAfterEdit()) result.final = true;
		}
		break;
	}
	case PropType::Float: {
		float f = static_cast<float>(std::get<double>(value));
		const float lo = static_cast<float>(prop.minValue), hi = static_cast<float>(prop.maxValue);
		const bool changed = slider ? ImGui::SliderFloat("##v", &f, lo, hi, "%.3f")
			: ImGui::DragFloat("##v", &f, Speed(prop), ranged ? lo : 0.0f, ranged ? hi : 0.0f, "%.3f");
		if (changed) {
			value = static_cast<double>(f);
			result = FromImGui(true);
		}
		else {
			result.active = ImGui::IsItemActive();
			if (!result.active && ImGui::IsItemDeactivatedAfterEdit()) result.final = true;
		}
		break;
	}
	case PropType::Vec2:
	case PropType::Vec3:
	case PropType::Vec4:
	case PropType::Color3:
	case PropType::Color4: {
		const bool colour = prop.type == PropType::Color3 || prop.type == PropType::Color4;
		const int n = (prop.type == PropType::Vec2) ? 2 : (prop.type == PropType::Vec3 || prop.type == PropType::Color3) ? 3 : 4;
		glm::vec4 v(0.0f);
		if (const glm::vec2* a = std::get_if<glm::vec2>(&value)) v = glm::vec4(*a, 0.0f, 0.0f);
		else if (const glm::vec3* a = std::get_if<glm::vec3>(&value)) v = glm::vec4(*a, 0.0f);
		else if (const glm::vec4* a = std::get_if<glm::vec4>(&value)) v = *a;
		const float lo = static_cast<float>(prop.minValue), hi = static_cast<float>(prop.maxValue);
		bool changed = false;
		if (colour) {
			changed = n == 3 ? ImGui::ColorEdit3("##v", &v.x) : ImGui::ColorEdit4("##v", &v.x);
		}
		else if (slider) {
			changed = n == 2 ? ImGui::SliderFloat2("##v", &v.x, lo, hi, "%.3f")
				: n == 3 ? ImGui::SliderFloat3("##v", &v.x, lo, hi, "%.3f")
				: ImGui::SliderFloat4("##v", &v.x, lo, hi, "%.3f");
		}
		else {
			const float speed = Speed(prop);
			const float dlo = ranged ? lo : 0.0f, dhi = ranged ? hi : 0.0f;
			changed = n == 2 ? ImGui::DragFloat2("##v", &v.x, speed, dlo, dhi, "%.3f")
				: n == 3 ? ImGui::DragFloat3("##v", &v.x, speed, dlo, dhi, "%.3f")
				: ImGui::DragFloat4("##v", &v.x, speed, dlo, dhi, "%.3f");
		}
		if (changed) {
			if (prop.type == PropType::Vec2) value = glm::vec2(v);
			else if (n == 3) value = glm::vec3(v);
			else value = v;
			result = FromImGui(true);
		}
		else {
			result.active = ImGui::IsItemActive();
			if (!result.active && ImGui::IsItemDeactivatedAfterEdit()) result.final = true;
		}
		break;
	}
	case PropType::String: {
		std::string text = std::get<std::string>(value);
		result = TextField("##v", text, nullptr, {}, false, false);
		if (result.changed) value = text;
		break;
	}
	case PropType::AssetPath: {
		std::string text = std::get<std::string>(value);
		result = TextField("##v", text, &assetPaths, prop.assetFilter, true, false);
		if (result.changed) value = text;
		break;
	}
	case PropType::NodeRef: {
		std::string text = std::get<std::string>(value);
		result = TextField("##v", text, &nodeNames, {}, false, true);
		if (result.changed) value = text;
		break;
	}
	}

	ImGui::EndDisabled();
	ImGui::PopID();
	if (readOnly) result = PropertyWidgetResult{};
	return result;
}

} // namespace nox::editor
