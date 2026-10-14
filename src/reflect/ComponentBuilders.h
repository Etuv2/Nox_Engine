#pragma once
// Small builders shared by every descriptor file (BuiltinComponents.cpp, src/reflect/components/*.cpp).

#include "Reflection.h"

#include <utility>

namespace nox::reflect {

inline PropertyDesc Prop(const char* name, PropType type,
	std::function<PropValue(const SceneNode&)> get,
	std::function<bool(SceneNode&, const PropValue&)> set = nullptr, uint32_t flags = PropNone) {
	PropertyDesc p;
	p.name = name;
	p.type = type;
	p.flags = flags;
	p.get = std::move(get);
	p.set = std::move(set);
	return p;
}

inline PropertyDesc Ranged(PropertyDesc p, double lo, double hi, double step = 0.0) {
	p.minValue = lo;
	p.maxValue = hi;
	p.step = step;
	return p;
}

// Read-only runtime value (shown in the inspector, never saved, never settable).
inline PropertyDesc RuntimeProp(const char* name, PropType type, std::function<PropValue(const SceneNode&)> get) {
	return Prop(name, type, std::move(get), nullptr, PropNoSave | PropReadOnly);
}

inline MethodDesc Method(const char* name, std::vector<PropType> args,
	std::function<bool(SceneNode&, const std::vector<PropValue>&, PropValue&)> invoke, bool editorButton = false) {
	MethodDesc m;
	m.name = name;
	m.args = std::move(args);
	m.invoke = std::move(invoke);
	m.editorButton = editorButton;
	return m;
}

// A method without arguments; shown as an inspector button when editorButton is set.
inline MethodDesc VoidMethod(const char* name, const char* tooltip,
	std::function<bool(SceneNode&)> fn, bool editorButton = true) {
	MethodDesc m;
	m.name = name;
	m.tooltip = tooltip;
	m.editorButton = editorButton;
	m.invoke = [fn = std::move(fn)](SceneNode& n, const std::vector<PropValue>&, PropValue&) { return fn(n); };
	return m;
}

} // namespace nox::reflect
