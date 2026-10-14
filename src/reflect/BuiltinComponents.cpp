// Built-in reflection descriptors. Each subsystem registers its descriptors from its own file
// (src/reflect/components/*.cpp); this file owns the always-present facets (Node, Transform), the
// Light descriptor that serves as the reference example, and the registration list.

#include "Reflection.h"
#include "ComponentBuilders.h"

#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include "../BaseLight.h"
#include "../LightNode.h"
#include "../SceneNode.h"
#include "../SpotLight.h"

#include "../render/RenderComponents.h"

namespace nox::reflect {

// Implemented in src/reflect/components/*.cpp.
void RegisterModelComponents();
void RegisterCameraComponents();
void RegisterAudioComponents();
void RegisterPhysicsComponents();
void RegisterAnimationComponents();
void RegisterGuiComponents();
void RegisterEnvironmentComponents();

namespace {

std::shared_ptr<BaseLight> LightOf(const SceneNode& node) {
	if (const auto* light = dynamic_cast<const LightNode*>(&node)) return light->GetLight();
	return nullptr;
}

LightNode::PhotometricUnit PhotometricUnitOf(const SceneNode& node) {
	const auto* light = dynamic_cast<const LightNode*>(&node);
	return light ? light->GetPhotometricUnit() : LightNode::PhotometricUnit::None;
}

void RegisterNode() {
	ComponentDesc c;
	c.name = "Node";
	c.category = "Core";
	c.builtin = true;
	c.order = 0;
	c.has = [](const SceneNode&) { return true; };
	c.properties = {
		Prop("name", PropType::String,
			[](const SceneNode& n) { return PropValue(const_cast<SceneNode&>(n).GetName()); },
			[](SceneNode& n, const PropValue& v) {
				const std::string& name = std::get<std::string>(v);
				if (name.empty()) return false;
				n.SetName(name);
				return true;
			}),
		[] {
			PropertyDesc p = Prop("node_type", PropType::Enum,
				[](const SceneNode& n) { return PropValue(static_cast<int64_t>(n.GetNodeType())); });
			p.enumNames = { "node", "audio", "model", "light", "camera", "gui", "lpv_volume", "skeletal", "folder" };
			p.flags |= PropNoSave;
			return p;
		}(),
		Prop("generated", PropType::Bool,
			[](const SceneNode& n) { return PropValue(n.IsGenerated()); }, nullptr, PropNoSave),
		Prop("child_count", PropType::Int,
			[](const SceneNode& n) { return PropValue(static_cast<int64_t>(n.GetChildCount())); }, nullptr, PropNoSave),
	};
	Registry::Get().Register(std::move(c));
}

void RegisterTransform() {
	ComponentDesc c;
	c.name = "Transform";
	c.category = "Core";
	c.builtin = true;
	c.order = 1;
	c.has = [](const SceneNode& n) { return !n.IsFolder(); };   // folders have no transform
	auto setTRS = [](SceneNode& n, const glm::vec3* t, const glm::quat* r, const glm::vec3* s) {
		n.SetLocalTRS(t ? *t : n.GetPosition(), r ? *r : n.GetOrientation(), s ? *s : n.GetScale());
		return true;
	};
	PropertyDesc rotation = Prop("rotation", PropType::Vec3,
		[](const SceneNode& n) { return PropValue(glm::degrees(glm::eulerAngles(n.GetOrientation()))); },
		[setTRS](SceneNode& n, const PropValue& v) {
			const glm::quat q(glm::radians(std::get<glm::vec3>(v)));
			return setTRS(n, nullptr, &q, nullptr);
		}, PropAngle);
	rotation.tooltip = "Euler angles in degrees (XYZ)";
	c.properties = {
		Prop("position", PropType::Vec3,
			[](const SceneNode& n) { return PropValue(n.GetPosition()); },
			[setTRS](SceneNode& n, const PropValue& v) { const glm::vec3 t = std::get<glm::vec3>(v); return setTRS(n, &t, nullptr, nullptr); }),
		rotation,
		Prop("scale", PropType::Vec3,
			[](const SceneNode& n) { return PropValue(n.GetScale()); },
			[setTRS](SceneNode& n, const PropValue& v) { const glm::vec3 s = std::get<glm::vec3>(v); return setTRS(n, nullptr, nullptr, &s); }),
		Prop("world_position", PropType::Vec3,
			[](const SceneNode& n) { return PropValue(n.GetWorldPosition()); }, nullptr, PropNoSave),
	};
	Registry::Get().Register(std::move(c));
}

void RegisterLight() {
	ComponentDesc c;
	c.name = "Light";
	c.category = "Rendering";
	c.tooltip = "Light source owned by a light node. The light kind is fixed when the node is created.";
	c.order = 10;
	c.has = [](const SceneNode& n) { return LightOf(n) != nullptr; };
	// Lights are node types (LightNode); add/remove happens through the add-node menu.

	auto light = [](auto fn) {
		return [fn](SceneNode& n, const PropValue& v) {
			auto l = LightOf(n);
			if (!l) return false;
			fn(*l, v);
			return true;
		};
	};
	PropertyDesc kind = Prop("kind", PropType::Enum,
		[](const SceneNode& n) { auto l = LightOf(n); return PropValue(static_cast<int64_t>(l ? static_cast<int>(l->GetLightType()) : 0)); },
		nullptr, PropNoSave);
	kind.enumNames = { "directional", "point", "spot", "area", "studio_key" };

	c.properties = {
		kind,
		Prop("enabled", PropType::Bool,
			[](const SceneNode& n) { auto l = LightOf(n); return PropValue(l && l->IsEnabled()); },
			light([](BaseLight& l, const PropValue& v) { l.SetEnabled(std::get<bool>(v)); })),
		Prop("color", PropType::Color3,
			[](const SceneNode& n) { auto l = LightOf(n); return PropValue(l ? l->GetColor() : glm::vec3(1.0f)); },
			light([](BaseLight& l, const PropValue& v) { l.SetColor(glm::max(std::get<glm::vec3>(v), glm::vec3(0.0f))); })),
		// Intensity is candela (punctual) or lux (directional). A light authored in lumens or lux shows that unit below
		// instead, so the file keeps its unit (see LightNode::GetPhotometricUnit).
		[&] {
			PropertyDesc p = Ranged(Prop("intensity", PropType::Float,
				[](const SceneNode& n) { auto l = LightOf(n); return PropValue(static_cast<double>(l ? l->GetIntensity() : 0.0f)); },
				light([](BaseLight& l, const PropValue& v) { l.SetIntensity(static_cast<float>(std::get<double>(v))); })), 0.0, 100000.0, 0.01);
			p.applies = [](const SceneNode& n) { return PhotometricUnitOf(n) == LightNode::PhotometricUnit::None; };
			return p;
		}(),
		[&] {
			PropertyDesc p = Ranged(Prop("luminous_power", PropType::Float,
				[](const SceneNode& n) {
					auto l = LightOf(n);
					return PropValue(static_cast<double>(l ? LightNode::LuminousPowerFromIntensity(l->GetLightType(), l->GetIntensity()) : 0.0f));
				},
				[](SceneNode& n, const PropValue& v) {
					auto l = LightOf(n);
					if (!l || PhotometricUnitOf(n) != LightNode::PhotometricUnit::Lumens) return false;
					const float lumens = static_cast<float>(std::max(std::get<double>(v), 0.0));
					l->SetIntensity(LightNode::IntensityFromLuminousPower(l->GetLightType(), lumens));
					return true;
				}), 0.0, 10000000.0, 1.0);
			p.label = "Luminous power (lm)";
			p.applies = [](const SceneNode& n) { return PhotometricUnitOf(n) == LightNode::PhotometricUnit::Lumens; };
			return p;
		}(),
		[&] {
			PropertyDesc p = Ranged(Prop("illuminance", PropType::Float,
				[](const SceneNode& n) { auto l = LightOf(n); return PropValue(static_cast<double>(l ? l->GetIntensity() : 0.0f)); },
				[](SceneNode& n, const PropValue& v) {
					auto l = LightOf(n);
					if (!l || PhotometricUnitOf(n) != LightNode::PhotometricUnit::Lux) return false;
					l->SetIntensity(static_cast<float>(std::max(std::get<double>(v), 0.0)));
					return true;
				}), 0.0, 1000000.0, 1.0);
			p.label = "Illuminance (lx)";
			p.applies = [](const SceneNode& n) { return PhotometricUnitOf(n) == LightNode::PhotometricUnit::Lux; };
			return p;
		}(),
		Ranged(Prop("range", PropType::Float,
			[](const SceneNode& n) { auto l = LightOf(n); return PropValue(static_cast<double>(l ? l->GetRange() : 0.0f)); },
			light([](BaseLight& l, const PropValue& v) { l.SetRange(static_cast<float>(std::get<double>(v))); })), 0.01, 100000.0, 0.1),
		Ranged(Prop("source_radius", PropType::Float,
			[](const SceneNode& n) { auto l = LightOf(n); return PropValue(static_cast<double>(l ? l->GetSourceRadius() : 0.0f)); },
			light([](BaseLight& l, const PropValue& v) { l.SetSourceRadius(static_cast<float>(std::get<double>(v))); })), 0.0, 100.0, 0.01),
		Prop("cast_shadows", PropType::Bool,
			[](const SceneNode& n) { auto l = LightOf(n); return PropValue(l && l->CastsShadows()); },
			light([](BaseLight& l, const PropValue& v) { l.SetCastsShadows(std::get<bool>(v)); })),
	};

	// Spot cone angles only exist on spot lights; the setter rejects other kinds.
	auto spotAngle = [](const char* name, bool inner) {
		PropertyDesc p = Ranged(Prop(name, PropType::Float,
			[inner](const SceneNode& n) {
				auto spot = std::dynamic_pointer_cast<SpotLight>(LightOf(n));
				return PropValue(static_cast<double>(spot ? (inner ? spot->GetCutOff() : spot->GetOuterCutOff()) : 0.0f));
			},
			[inner](SceneNode& n, const PropValue& v) {
				auto spot = std::dynamic_pointer_cast<SpotLight>(LightOf(n));
				if (!spot) return false;
				const float deg = static_cast<float>(std::get<double>(v));
				if (inner) spot->SetCutOff(deg); else spot->SetOuterCutOff(deg);
				return true;
			}, PropAngle), 0.0, 89.0, 0.1);
		p.applies = [](const SceneNode& n) { return std::dynamic_pointer_cast<SpotLight>(LightOf(n)) != nullptr; };
		return p;
	};
	c.properties.push_back(spotAngle("spot_inner_angle", true));
	c.properties.push_back(spotAngle("spot_outer_angle", false));

	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterBuiltinComponents() {
	static bool registered = false;
	if (registered) return;
	registered = true;
	RegisterNode();
	RegisterTransform();
	RegisterLight();
	RegisterModelComponents();
	RegisterCameraComponents();
	RegisterAudioComponents();
	RegisterPhysicsComponents();
	RegisterAnimationComponents();
	RegisterGuiComponents();
	RegisterEnvironmentComponents();
	nox::render::RegisterRenderComponents();   // ParticleEmitter, Decal (src/render/RenderComponents.cpp)
}

} // namespace nox::reflect
