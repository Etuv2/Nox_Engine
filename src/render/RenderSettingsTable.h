#pragma once
// The scene-wide render settings as one table: every key the renderer reads from a scene's
// "render_settings" object (SceneRenderSettings.cpp Apply), with its type, range, label and accessors on
// RenderContext. Three consumers use it so they cannot drift apart:
//   - the WorldEnvironment component (src/reflect/components/EnvironmentComponents.cpp), whose property
//     names are these keys;
//   - SceneRenderSettings::ToJson, which the scene saver uses to write the live values back;
//   - tests (tests/native/test_render_components.cpp) that round-trip every key.
// Property name == scene key. Enum settings store their JSON name ("auto", "aces", ...) through enumNames.

#include "../reflect/Reflection.h"

#include <functional>
#include <string>
#include <vector>

struct RenderContext;

namespace nox::render {

struct RenderSetting {
	std::string key;        // scene render_settings key and property name
	std::string label;      // inspector label, prefixed with its group
	std::string group;
	std::string tooltip;
	nox::reflect::PropType type = nox::reflect::PropType::Float;
	double minValue = 0.0;  // used when minValue < maxValue
	double maxValue = 0.0;
	double step = 0.0;
	uint32_t flags = 0;     // reflect PropFlags (PropSlider, PropAngle ...)
	std::vector<std::string> enumNames;   // Enum settings: JSON names, index = PropValue int
	std::function<nox::reflect::PropValue(const RenderContext&)> get;
	std::function<bool(RenderContext&, const nox::reflect::PropValue&)> set;
};

// Every consumed key, in inspector order (groups are contiguous).
const std::vector<RenderSetting>& RenderSettingTable();
// Null when key is not a render setting.
const RenderSetting* FindRenderSetting(const std::string& key);

} // namespace nox::render
