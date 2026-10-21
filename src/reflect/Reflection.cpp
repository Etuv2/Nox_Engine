#include "Reflection.h"
#include "ReflectServices.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../SceneNode.h"

namespace nox::reflect {

const PropertyDesc* ComponentDesc::FindProperty(const std::string& property) const {
	for (const PropertyDesc& prop : properties) {
		if (prop.name == property) return &prop;
	}
	return nullptr;
}

const MethodDesc* ComponentDesc::FindMethod(const std::string& method) const {
	for (const MethodDesc& m : methods) {
		if (m.name == method) return &m;
	}
	return nullptr;
}

EngineServices& Services() {
	static EngineServices services;
	return services;
}

Registry& Registry::Get() {
	static Registry registry;
	return registry;
}

void Registry::Register(ComponentDesc desc) {
	for (PropertyDesc& prop : desc.properties) {
		if (prop.label.empty()) prop.label = prop.name;
		if (!prop.set) prop.flags |= PropReadOnly;
	}
	auto it = std::find_if(m_components.begin(), m_components.end(),
		[&](const ComponentDesc& c) { return c.name == desc.name; });
	if (it != m_components.end()) *it = std::move(desc);
	else m_components.push_back(std::move(desc));
	std::stable_sort(m_components.begin(), m_components.end(), [](const ComponentDesc& a, const ComponentDesc& b) {
		if (a.order != b.order) return a.order < b.order;
		return a.name < b.name;
	});
}

const ComponentDesc* Registry::Find(const std::string& name) const {
	for (const ComponentDesc& c : m_components) {
		if (c.name == name) return &c;
	}
	return nullptr;
}

std::vector<const ComponentDesc*> Registry::ComponentsOf(const SceneNode& node) const {
	std::vector<const ComponentDesc*> out;
	for (const ComponentDesc& c : m_components) {
		if (c.has && c.has(node)) out.push_back(&c);
	}
	return out;
}

// ---- Value helpers -------------------------------------------------------------------------------

bool ValueMatchesType(PropType type, const PropValue& value) {
	switch (type) {
	case PropType::Bool: return std::holds_alternative<bool>(value);
	case PropType::Int:
	case PropType::Enum: return std::holds_alternative<int64_t>(value);
	case PropType::Float: return std::holds_alternative<double>(value);
	case PropType::Vec2: return std::holds_alternative<glm::vec2>(value);
	case PropType::Vec3:
	case PropType::Color3: return std::holds_alternative<glm::vec3>(value);
	case PropType::Vec4:
	case PropType::Color4: return std::holds_alternative<glm::vec4>(value);
	case PropType::String:
	case PropType::AssetPath:
	case PropType::NodeRef: return std::holds_alternative<std::string>(value);
	}
	return false;
}

bool CoerceValue(PropType type, PropValue& value) {
	if (ValueMatchesType(type, value)) return true;
	switch (type) {
	case PropType::Int:
	case PropType::Enum:
		if (const double* d = std::get_if<double>(&value)) {
			if (!std::isfinite(*d) || std::floor(*d) != *d) return false;
			value = static_cast<int64_t>(*d);
			return true;
		}
		if (const bool* b = std::get_if<bool>(&value)) { value = static_cast<int64_t>(*b ? 1 : 0); return true; }
		return false;
	case PropType::Float:
		if (const int64_t* i = std::get_if<int64_t>(&value)) { value = static_cast<double>(*i); return true; }
		return false;
	case PropType::Vec3:
	case PropType::Color3:
		if (const glm::vec4* v = std::get_if<glm::vec4>(&value)) { value = glm::vec3(*v); return true; }
		return false;
	case PropType::Vec4:
	case PropType::Color4:
		if (const glm::vec3* v = std::get_if<glm::vec3>(&value)) { value = glm::vec4(*v, 1.0f); return true; }
		return false;
	default:
		return false;
	}
}

namespace {

bool ReadFloats(const nlohmann::json& json, float* out, size_t count) {
	if (!json.is_array() || json.size() != count) return false;
	for (size_t i = 0; i < count; ++i) {
		if (!json[i].is_number()) return false;
		out[i] = json[i].get<float>();
	}
	return true;
}

bool IsFiniteValue(const PropValue& value) {
	if (const double* d = std::get_if<double>(&value)) return std::isfinite(*d);
	if (const glm::vec2* v = std::get_if<glm::vec2>(&value)) return std::isfinite(v->x) && std::isfinite(v->y);
	if (const glm::vec3* v = std::get_if<glm::vec3>(&value)) return std::isfinite(v->x) && std::isfinite(v->y) && std::isfinite(v->z);
	if (const glm::vec4* v = std::get_if<glm::vec4>(&value)) {
		return std::isfinite(v->x) && std::isfinite(v->y) && std::isfinite(v->z) && std::isfinite(v->w);
	}
	return true;
}

} // namespace

nlohmann::json ValueToJson(const PropertyDesc& prop, const PropValue& value) {
	if (prop.type == PropType::Enum) {
		if (const int64_t* i = std::get_if<int64_t>(&value)) {
			if (*i >= 0 && static_cast<size_t>(*i) < prop.enumNames.size()) return prop.enumNames[static_cast<size_t>(*i)];
			return *i;
		}
	}
	return std::visit([](const auto& v) -> nlohmann::json {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, glm::vec2>) return nlohmann::json::array({ v.x, v.y });
		else if constexpr (std::is_same_v<T, glm::vec3>) return nlohmann::json::array({ v.x, v.y, v.z });
		else if constexpr (std::is_same_v<T, glm::vec4>) return nlohmann::json::array({ v.x, v.y, v.z, v.w });
		else return v;
	}, value);
}

bool ValueFromJson(const PropertyDesc& prop, const nlohmann::json& json, PropValue& out) {
	switch (prop.type) {
	case PropType::Bool:
		if (!json.is_boolean()) return false;
		out = json.get<bool>();
		return true;
	case PropType::Int:
		if (!json.is_number_integer()) return false;
		out = json.get<int64_t>();
		return true;
	case PropType::Enum:
		if (json.is_string()) {
			const std::string name = json.get<std::string>();
			for (size_t i = 0; i < prop.enumNames.size(); ++i) {
				if (prop.enumNames[i] == name) { out = static_cast<int64_t>(i); return true; }
			}
			return false;
		}
		if (!json.is_number_integer()) return false;
		out = json.get<int64_t>();
		return true;
	case PropType::Float:
		if (!json.is_number()) return false;
		out = json.get<double>();
		return true;
	case PropType::Vec2: { glm::vec2 v; if (!ReadFloats(json, &v.x, 2)) return false; out = v; return true; }
	case PropType::Vec3:
	case PropType::Color3: { glm::vec3 v; if (!ReadFloats(json, &v.x, 3)) return false; out = v; return true; }
	case PropType::Vec4:
	case PropType::Color4: { glm::vec4 v; if (!ReadFloats(json, &v.x, 4)) return false; out = v; return true; }
	case PropType::String:
	case PropType::AssetPath:
	case PropType::NodeRef:
		if (!json.is_string()) return false;
		out = json.get<std::string>();
		return true;
	}
	return false;
}

std::string ValueToString(const PropValue& value) {
	char buf[128];
	return std::visit([&](const auto& v) -> std::string {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
		else if constexpr (std::is_same_v<T, int64_t>) return std::to_string(v);
		else if constexpr (std::is_same_v<T, double>) { std::snprintf(buf, sizeof(buf), "%g", v); return buf; }
		else if constexpr (std::is_same_v<T, glm::vec2>) { std::snprintf(buf, sizeof(buf), "(%g, %g)", v.x, v.y); return buf; }
		else if constexpr (std::is_same_v<T, glm::vec3>) { std::snprintf(buf, sizeof(buf), "(%g, %g, %g)", v.x, v.y, v.z); return buf; }
		else if constexpr (std::is_same_v<T, glm::vec4>) { std::snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", v.x, v.y, v.z, v.w); return buf; }
		else return v;
	}, value);
}

bool ValuesEqual(const PropValue& a, const PropValue& b) {
	return a == b;
}

PropValue ClampToRange(const PropertyDesc& prop, PropValue value) {
	if (!(prop.minValue < prop.maxValue)) return value;
	const double lo = prop.minValue, hi = prop.maxValue;
	if (double* d = std::get_if<double>(&value)) *d = std::clamp(*d, lo, hi);
	else if (int64_t* i = std::get_if<int64_t>(&value)) {
		*i = std::clamp(*i, static_cast<int64_t>(std::ceil(lo)), static_cast<int64_t>(std::floor(hi)));
	}
	return value;
}

// ---- Node-level operations -----------------------------------------------------------------------

namespace {

const ComponentDesc* FindPresent(const SceneNode& node, const std::string& component, std::string* error) {
	const ComponentDesc* desc = Registry::Get().Find(component);
	if (!desc) {
		if (error) *error = "unknown component '" + component + "'";
		return nullptr;
	}
	if (!desc->has || !desc->has(node)) {
		if (error) *error = "node has no component '" + component + "'";
		return nullptr;
	}
	return desc;
}

} // namespace

bool GetProperty(const SceneNode& node, const std::string& component, const std::string& property, PropValue& out) {
	const ComponentDesc* desc = FindPresent(node, component, nullptr);
	if (!desc) return false;
	const PropertyDesc* prop = desc->FindProperty(property);
	if (!prop || !prop->get || !prop->AppliesTo(node)) return false;
	out = prop->get(node);
	return true;
}

bool SetProperty(SceneNode& node, const std::string& component, const std::string& property,
	const PropValue& value, std::string* error) {
	const ComponentDesc* desc = FindPresent(node, component, error);
	if (!desc) return false;
	const PropertyDesc* prop = desc->FindProperty(property);
	if (!prop || !prop->AppliesTo(node)) {
		if (error) *error = component + " has no property '" + property + "'";
		return false;
	}
	if (!prop->set || (prop->flags & PropReadOnly)) {
		if (error) *error = component + "." + property + " is read-only";
		return false;
	}
	PropValue coerced = value;
	if (!CoerceValue(prop->type, coerced)) {
		if (error) *error = component + "." + property + ": wrong value type";
		return false;
	}
	if (!IsFiniteValue(coerced)) {
		if (error) *error = component + "." + property + ": value must be finite";
		return false;
	}
	if (prop->type == PropType::Enum) {
		const int64_t index = std::get<int64_t>(coerced);
		if (index < 0 || static_cast<size_t>(index) >= prop->enumNames.size()) {
			if (error) *error = component + "." + property + ": enum index out of range";
			return false;
		}
	}
	coerced = ClampToRange(*prop, std::move(coerced));
	if (!prop->set(node, coerced)) {
		if (error) *error = component + "." + property + ": value rejected";
		return false;
	}
	return true;
}

bool InvokeMethod(SceneNode& node, const std::string& component, const std::string& method,
	const std::vector<PropValue>& args, PropValue& result, std::string* error) {
	const ComponentDesc* desc = FindPresent(node, component, error);
	if (!desc) return false;
	const MethodDesc* m = desc->FindMethod(method);
	if (!m || !m->invoke) {
		if (error) *error = component + " has no method '" + method + "'";
		return false;
	}
	if (args.size() != m->args.size()) {
		if (error) *error = component + ":" + method + " expects " + std::to_string(m->args.size()) + " argument(s)";
		return false;
	}
	std::vector<PropValue> coerced = args;
	for (size_t i = 0; i < coerced.size(); ++i) {
		if (!CoerceValue(m->args[i], coerced[i]) || !IsFiniteValue(coerced[i])) {
			if (error) *error = component + ":" + method + ": bad argument " + std::to_string(i + 1);
			return false;
		}
	}
	if (!m->invoke(node, coerced, result)) {
		if (error) *error = component + ":" + method + " failed";
		return false;
	}
	return true;
}

nlohmann::json SerializeComponents(const SceneNode& node, bool skipBuiltin) {
	nlohmann::json out = nlohmann::json::object();
	for (const ComponentDesc* desc : Registry::Get().ComponentsOf(node)) {
		if (skipBuiltin && desc->builtin) continue;
		nlohmann::json props = nlohmann::json::object();
		for (const PropertyDesc& prop : desc->properties) {
			if ((prop.flags & (PropNoSave | PropReadOnly)) || !prop.get || !prop.AppliesTo(node)) continue;
			props[prop.name] = ValueToJson(prop, prop.get(node));
		}
		out[desc->name] = std::move(props);
	}
	return out;
}

namespace {

// Adds the component when the node lacks it, then applies the authored properties. False when it could not be added.
bool DeserializeOneComponent(SceneNode& node, const std::string& name, const ComponentDesc& desc,
	const nlohmann::json& value, std::vector<std::string>* warnings, bool reportAddFailure) {
	if (!desc.has(node)) {
		if (!desc.add || !desc.add(node)) {
			if (reportAddFailure && warnings) warnings->push_back("cannot add component '" + name + "'");
			return false;
		}
	}
	if (!value.is_object()) return true;
	for (auto p = value.begin(); p != value.end(); ++p) {
		const PropertyDesc* prop = desc.FindProperty(p.key());
		if (!prop) {
			if (warnings) warnings->push_back(name + ": unknown property '" + p.key() + "'");
			continue;
		}
		if (prop->flags & PropReadOnly) continue;
		PropValue propValue;
		std::string error;
		if (!ValueFromJson(*prop, p.value(), propValue) || !SetProperty(node, name, p.key(), propValue, &error)) {
			if (warnings) warnings->push_back(name + "." + p.key() + ": " + (error.empty() ? "bad value" : error));
		}
	}
	return true;
}

} // namespace

void DeserializeComponents(SceneNode& node, const nlohmann::json& components, std::vector<std::string>* warnings,
	std::vector<std::string>* deferred) {
	if (!components.is_object()) {
		if (warnings) warnings->push_back("'components' must be an object");
		return;
	}
	for (auto it = components.begin(); it != components.end(); ++it) {
		const ComponentDesc* desc = Registry::Get().Find(it.key());
		if (!desc) {
			if (warnings) warnings->push_back("unknown component '" + it.key() + "'");
			continue;
		}
		// With `deferred` the failure is not reported here: the caller retries the name once the node has its entity.
		if (!DeserializeOneComponent(node, it.key(), *desc, it.value(), warnings, deferred == nullptr) && deferred) {
			deferred->push_back(it.key());
		}
	}
}

void DeserializeComponentsNamed(SceneNode& node, const nlohmann::json& components, const std::vector<std::string>& names,
	std::vector<std::string>* warnings) {
	if (!components.is_object()) return;
	for (const std::string& name : names) {
		const ComponentDesc* desc = Registry::Get().Find(name);
		if (!desc || !components.contains(name)) continue;
		DeserializeOneComponent(node, name, *desc, components.at(name), warnings, true);
	}
}

} // namespace nox::reflect
