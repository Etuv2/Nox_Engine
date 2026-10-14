#pragma once
// Generic inspector widgets: one editor per reflection PropType (see reflect/Reflection.h). The inspector
// draws every registered component through these, so a new descriptor needs no editor code.

#include <string>
#include <vector>
#include "../reflect/Reflection.h"

namespace nox::editor {

struct PropertyWidgetResult {
	bool changed = false;   // value was edited this frame (the new value is written into value)
	bool active = false;    // the widget is still being edited (drag, slider, text focus)
	bool final = false;     // the edit ends this frame (discrete change, Enter, text deactivated)
};

// Draws the value editor for one property in the current table cell. The id must be unique per property.
// assetPaths feeds the AssetPath picker; nodeNames feeds the NodeRef picker. Drag-and-drop accepts assets
// dragged from the Assets panel ("NOX_ASSET") and nodes dragged from the Hierarchy ("NOX_NODE").
PropertyWidgetResult DrawPropertyWidget(const nox::reflect::PropertyDesc& prop, nox::reflect::PropValue& value,
	const std::vector<std::string>& assetPaths, const std::vector<std::string>& nodeNames, const char* id);

// Drag-and-drop payload types shared with the Assets and Hierarchy panels.
inline constexpr const char* kAssetDragType = "NOX_ASSET";   // payload: NUL-terminated logical path
inline constexpr const char* kNodeDragType = "NOX_NODE";     // payload: SceneNode* (pointer value)

} // namespace nox::editor
