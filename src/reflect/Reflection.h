#pragma once
// Engine reflection: one property registry for every native node component.
//
// A ComponentDesc describes one inspectable facet of a SceneNode ("Transform", "Light", "Audio", ...):
// whether a node has it, how to add/remove it, its typed properties (get/set closures) and callable
// methods. The editor inspector, the add-component menu, scene save/load, undo and the Lua
// node:component(...) API all go through this registry, so a component registered once is
// inspectable, editable, serialized, undoable and scriptable everywhere.
//
// Rules for descriptors:
//  - get() must be side-effect free; set() returns false when the value is rejected (type or range).
//  - set() must leave the node in a renderable state and call whatever sync the subsystem needs.
//  - Property names are snake_case and stable: they are the scene JSON keys and the Lua field names.
//  - Engine descriptors stay generic (see docs/engine_boundary.md); game data belongs to projects.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "../json.hpp"

class SceneNode;

namespace nox::reflect {

enum class PropType : uint8_t {
	Bool,
	Int,
	Float,
	Vec2,
	Vec3,
	Vec4,
	Color3,     // vec3, edited as a colour
	Color4,     // vec4, edited as a colour with alpha
	String,
	Enum,       // int index into PropertyDesc::enumNames; serialized as the name
	AssetPath,  // logical asset path (string); assetFilter lists accepted extensions
	NodeRef,    // name of another node in the scene (string)
};

using PropValue = std::variant<bool, int64_t, double, glm::vec2, glm::vec3, glm::vec4, std::string>;

enum PropFlags : uint32_t {
	PropNone = 0,
	PropReadOnly = 1u << 0,   // shown, not editable, not loaded from JSON
	PropHidden = 1u << 1,     // not shown in the inspector
	PropNoSave = 1u << 2,     // runtime state; not written to scene JSON
	PropAngle = 1u << 3,      // Float/Vec3 stored in degrees; inspector shows degrees
	PropSlider = 1u << 4,     // draw as a slider between min and max
	PropRestartOnChange = 1u << 5,  // takes effect after the subsystem rebuilds (inspector hint)
};

struct PropertyDesc {
	std::string name;       // snake_case; scene JSON key and Lua field
	std::string label;      // inspector label; defaults to name
	std::string tooltip;
	PropType type = PropType::Float;
	uint32_t flags = PropNone;
	double minValue = 0.0;  // used when minValue < maxValue
	double maxValue = 0.0;
	double step = 0.0;      // drag speed / slider step; 0 = default
	std::vector<std::string> enumNames;   // PropType::Enum
	std::vector<std::string> assetFilter; // PropType::AssetPath, e.g. {".glb", ".gltf"}
	std::function<PropValue(const SceneNode&)> get;
	std::function<bool(SceneNode&, const PropValue&)> set;   // null for read-only properties
	// Optional: false hides the property for this node (not drawn, not saved, get/set fail), e.g. spot
	// cone angles on a point light.
	std::function<bool(const SceneNode&)> applies;

	bool AppliesTo(const SceneNode& node) const { return !applies || applies(node); }
};

// A callable action (Lua: comp:method(args...), inspector: button when it takes no arguments).
struct MethodDesc {
	std::string name;
	std::string tooltip;
	std::vector<PropType> args;
	// Fills result when returnsValue; returns false on bad input or failure.
	std::function<bool(SceneNode&, const std::vector<PropValue>& args, PropValue& result)> invoke;
	bool returnsValue = false;
	bool editorButton = false;   // show as an inspector button (args must be empty)
};

struct ComponentDesc {
	std::string name;        // PascalCase, e.g. "Light"; scene JSON key under "components"
	std::string category;    // add-component menu group, e.g. "Rendering", "Physics", "Audio"
	std::string tooltip;
	bool builtin = false;    // true for always-present facets (Node, Transform): no add/remove
	std::function<bool(const SceneNode&)> has;
	std::function<bool(SceneNode&)> add;      // null when it cannot be added from the editor/scripts
	std::function<bool(SceneNode&)> remove;   // null when it cannot be removed
	std::vector<PropertyDesc> properties;
	std::vector<MethodDesc> methods;
	int order = 100;         // inspector order (lower first)

	const PropertyDesc* FindProperty(const std::string& property) const;
	const MethodDesc* FindMethod(const std::string& method) const;
};

class Registry {
public:
	static Registry& Get();

	// Replaces an existing descriptor with the same name (lets tests and projects override).
	void Register(ComponentDesc desc);
	const ComponentDesc* Find(const std::string& name) const;
	const std::vector<ComponentDesc>& All() const { return m_components; }   // sorted by order, name

	// Descriptors whose has() is true for the node, in inspector order.
	std::vector<const ComponentDesc*> ComponentsOf(const SceneNode& node) const;

private:
	Registry() = default;
	std::vector<ComponentDesc> m_components;
};

// Registers the engine's built-in descriptors once (idempotent). Called by Core at startup and by tests.
void RegisterBuiltinComponents();

// ---- Value helpers -------------------------------------------------------------------------------

// True when the value's variant alternative matches the property type.
bool ValueMatchesType(PropType type, const PropValue& value);
// Coerces a value toward the property type (int<->double, vec widths); false when impossible.
bool CoerceValue(PropType type, PropValue& value);

nlohmann::json ValueToJson(const PropertyDesc& prop, const PropValue& value);
bool ValueFromJson(const PropertyDesc& prop, const nlohmann::json& json, PropValue& out);
std::string ValueToString(const PropValue& value);
bool ValuesEqual(const PropValue& a, const PropValue& b);

// Clamps numeric values into [minValue, maxValue] when a range is set. Returns the clamped value.
PropValue ClampToRange(const PropertyDesc& prop, PropValue value);

// ---- Node-level operations (all callers use these, never get/set directly) -----------------------

bool GetProperty(const SceneNode& node, const std::string& component, const std::string& property, PropValue& out);
// Validates type and range, then calls set(). error is filled on failure.
bool SetProperty(SceneNode& node, const std::string& component, const std::string& property,
	const PropValue& value, std::string* error = nullptr);
bool InvokeMethod(SceneNode& node, const std::string& component, const std::string& method,
	const std::vector<PropValue>& args, PropValue& result, std::string* error = nullptr);

// Serializes every present component's savable properties: { "Light": { "intensity": 2.0, ... }, ... }.
// Builtin facets whose data the scene format already stores elsewhere are skipped when skipBuiltin.
nlohmann::json SerializeComponents(const SceneNode& node, bool skipBuiltin = true);
// Applies a "components" object: adds missing components, then sets properties. Unknown components
// or properties are reported through warnings and otherwise ignored (forward compatibility).
void DeserializeComponents(SceneNode& node, const nlohmann::json& components, std::vector<std::string>* warnings = nullptr);

} // namespace nox::reflect
