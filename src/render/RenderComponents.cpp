// Render component descriptors: ParticleEmitter and Decal (Rendering category). Runtime state lives on the
// node (SceneNode::GetParticleEmitter / GetDecal); ParticlePass and DecalPass read it every frame.
// See docs/rendering_components.md for the property reference.

#include "RenderComponents.h"

#include "DecalComponent.h"
#include "ParticleSystem.h"
#include "../SceneNode.h"
#include "../reflect/ComponentBuilders.h"
#include "../reflect/ReflectServices.h"

#include <algorithm>
#include <atomic>

namespace nox::render {

using namespace nox::reflect;

namespace {

std::atomic<uint32_t> g_emitterSeed{ 1u };   // each emitter gets its own random stream

ParticleEmitterComponent* EmitterOf(SceneNode& n) { return n.GetParticleEmitter().get(); }
const ParticleEmitterComponent* EmitterOf(const SceneNode& n) { return n.GetParticleEmitter().get(); }
DecalComponent* DecalOf(SceneNode& n) { return n.GetDecal().get(); }
const DecalComponent* DecalOf(const SceneNode& n) { return n.GetDecal().get(); }

// Float property bound to one member of the emitter settings.
PropertyDesc EmitterFloat(const char* name, float ParticleEmitterSettings::*member, double lo, double hi,
	double step, uint32_t flags = PropNone) {
	return Ranged(Prop(name, PropType::Float,
		[member](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(static_cast<double>(e ? e->settings.*member : 0.0f));
		},
		[member](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.*member = static_cast<float>(std::get<double>(v));
			NotifySceneChanged();
			return true;
		}, flags), lo, hi, step);
}

PropertyDesc EmitterVec3(const char* name, glm::vec3 ParticleEmitterSettings::*member) {
	return Prop(name, PropType::Vec3,
		[member](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(e ? e->settings.*member : glm::vec3(0.0f));
		},
		[member](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.*member = std::get<glm::vec3>(v);
			NotifySceneChanged();
			return true;
		});
}

PropertyDesc EmitterColor(const char* name, glm::vec4 ParticleEmitterSettings::*member) {
	return Prop(name, PropType::Color4,
		[member](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(e ? e->settings.*member : glm::vec4(1.0f));
		},
		[member](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.*member = std::get<glm::vec4>(v);
			NotifySceneChanged();
			return true;
		});
}

void RegisterParticleEmitter() {
	ComponentDesc c;
	c.name = "ParticleEmitter";
	c.category = "Rendering";
	c.tooltip = "CPU particle emitter (camera-facing billboards) at the node's transform. Emits while 'emitting' is set.";
	c.order = 60;
	c.has = [](const SceneNode& n) { return n.GetParticleEmitter() != nullptr; };
	c.add = [](SceneNode& n) {
		if (n.GetParticleEmitter()) return false;
		auto emitter = std::make_shared<ParticleEmitterComponent>();
		emitter->system = ParticleSystem(g_emitterSeed.fetch_add(1u));
		n.SetParticleEmitter(emitter);
		NotifySceneChanged();
		return true;
	};
	c.remove = [](SceneNode& n) {
		if (!n.GetParticleEmitter()) return false;
		n.SetParticleEmitter(nullptr);
		NotifySceneChanged();
		return true;
	};

	PropertyDesc emitting = Prop("emitting", PropType::Bool,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(e ? e->settings.emitting : false);
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.emitting = std::get<bool>(v);
			NotifySceneChanged();
			return true;
		});

	// lifetime_min and lifetime_max keep their order: each setter clamps against the other.
	PropertyDesc lifetimeMin = Ranged(Prop("lifetime_min", PropType::Float,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(static_cast<double>(e ? e->settings.lifetimeMin : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.lifetimeMin = std::min(static_cast<float>(std::get<double>(v)), e->settings.lifetimeMax);
			NotifySceneChanged();
			return true;
		}), 0.01, 60.0, 0.05);
	PropertyDesc lifetimeMax = Ranged(Prop("lifetime_max", PropType::Float,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(static_cast<double>(e ? e->settings.lifetimeMax : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.lifetimeMax = std::max(static_cast<float>(std::get<double>(v)), e->settings.lifetimeMin);
			NotifySceneChanged();
			return true;
		}), 0.01, 60.0, 0.05);

	PropertyDesc spread = EmitterFloat("spread_angle", &ParticleEmitterSettings::spreadAngle, 0.0, 180.0, 1.0, PropAngle);
	spread.tooltip = "Full cone angle in degrees around the node's up axis";

	PropertyDesc blend = Prop("blend", PropType::Enum,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(static_cast<int64_t>(e ? e->settings.blend : ParticleBlend::Alpha));
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.blend = static_cast<ParticleBlend>(std::get<int64_t>(v));
			NotifySceneChanged();
			return true;
		});
	blend.enumNames = { "alpha", "additive" };

	PropertyDesc texture = Prop("texture", PropType::AssetPath,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(e ? e->settings.texture : std::string());
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.texture = std::get<std::string>(v);
			NotifySceneChanged();
			return true;
		});
	texture.assetFilter = { ".png", ".jpg", ".jpeg" };
	texture.tooltip = "Optional particle image; empty draws a soft disc";

	PropertyDesc maxParticles = Ranged(Prop("max_particles", PropType::Int,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(static_cast<int64_t>(e ? e->settings.maxParticles : 0));
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.maxParticles = static_cast<int>(std::get<int64_t>(v));
			NotifySceneChanged();
			return true;
		}), 1.0, 100000.0, 1.0);

	PropertyDesc worldSpace = Prop("world_space", PropType::Bool,
		[](const SceneNode& n) {
			const ParticleEmitterComponent* e = EmitterOf(n);
			return PropValue(e ? e->settings.worldSpace : false);
		},
		[](SceneNode& n, const PropValue& v) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->settings.worldSpace = std::get<bool>(v);
			NotifySceneChanged();
			return true;
		});
	worldSpace.tooltip = "Particles stay where they were spawned instead of following the emitter";

	c.properties = {
		emitting,
		EmitterFloat("rate", &ParticleEmitterSettings::rate, 0.0, 1000.0, 1.0),
		lifetimeMin,
		lifetimeMax,
		EmitterFloat("start_speed", &ParticleEmitterSettings::startSpeed, 0.0, 100.0, 0.1),
		spread,
		EmitterVec3("gravity", &ParticleEmitterSettings::gravity),
		EmitterFloat("start_size", &ParticleEmitterSettings::startSize, 0.0, 100.0, 0.01),
		EmitterFloat("end_size", &ParticleEmitterSettings::endSize, 0.0, 100.0, 0.01),
		EmitterColor("start_color", &ParticleEmitterSettings::startColor),
		EmitterColor("end_color", &ParticleEmitterSettings::endColor),
		texture,
		maxParticles,
		worldSpace,
		blend,
		RuntimeProp("particle_count", PropType::Int,
			[](const SceneNode& n) {
				const ParticleEmitterComponent* e = EmitterOf(n);
				return PropValue(static_cast<int64_t>(e ? e->system.Count() : 0));
			}),
	};

	c.methods.push_back(Method("burst", { PropType::Int },
		[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e || args.empty()) return false;
			const int count = static_cast<int>(std::clamp<int64_t>(std::get<int64_t>(args[0]), 0, 100000));
			e->system.Burst(count, n.GetWorldPosition4x4(), e->settings);
			return true;
		}));
	c.methods.back().tooltip = "Spawns count particles now (respects max_particles)";
	c.methods.push_back(VoidMethod("restart", "Removes all particles and restarts emission",
		[](SceneNode& n) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->system.Restart();
			return true;
		}));
	c.methods.push_back(VoidMethod("clear", "Removes all live particles",
		[](SceneNode& n) {
			ParticleEmitterComponent* e = EmitterOf(n);
			if (!e) return false;
			e->system.Clear();
			return true;
		}));

	Registry::Get().Register(std::move(c));
}

void RegisterDecal() {
	ComponentDesc c;
	c.name = "Decal";
	c.category = "Rendering";
	c.tooltip = "Textured decal quad on the surface at the node (planar: size.x by size.z, facing the node's up axis).";
	c.order = 61;
	c.has = [](const SceneNode& n) { return n.GetDecal() != nullptr; };
	c.add = [](SceneNode& n) {
		if (n.GetDecal()) return false;
		n.SetDecal(std::make_shared<DecalComponent>());
		NotifySceneChanged();
		return true;
	};
	c.remove = [](SceneNode& n) {
		if (!n.GetDecal()) return false;
		n.SetDecal(nullptr);
		NotifySceneChanged();
		return true;
	};

	PropertyDesc texture = Prop("texture", PropType::AssetPath,
		[](const SceneNode& n) {
			const DecalComponent* d = DecalOf(n);
			return PropValue(d ? d->texture : std::string());
		},
		[](SceneNode& n, const PropValue& v) {
			DecalComponent* d = DecalOf(n);
			if (!d) return false;
			d->texture = std::get<std::string>(v);
			NotifySceneChanged();
			return true;
		});
	texture.assetFilter = { ".png", ".jpg", ".jpeg" };

	PropertyDesc size = Prop("size", PropType::Vec3,
		[](const SceneNode& n) {
			const DecalComponent* d = DecalOf(n);
			return PropValue(d ? d->size : glm::vec3(1.0f, 0.1f, 1.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			DecalComponent* d = DecalOf(n);
			const glm::vec3 s = std::get<glm::vec3>(v);
			if (!d || !(s.x > 0.0f && s.y > 0.0f && s.z > 0.0f)) return false;
			d->size = s;
			NotifySceneChanged();
			return true;
		});
	size.tooltip = "Metres: x across and z along the node's local axes; y is reserved for projected decals";

	PropertyDesc color = Prop("color", PropType::Color4,
		[](const SceneNode& n) {
			const DecalComponent* d = DecalOf(n);
			return PropValue(d ? d->color : glm::vec4(1.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			DecalComponent* d = DecalOf(n);
			if (!d) return false;
			d->color = std::get<glm::vec4>(v);
			NotifySceneChanged();
			return true;
		});

	PropertyDesc opacity = Ranged(Prop("opacity", PropType::Float,
		[](const SceneNode& n) {
			const DecalComponent* d = DecalOf(n);
			return PropValue(static_cast<double>(d ? d->opacity : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			DecalComponent* d = DecalOf(n);
			if (!d) return false;
			d->opacity = static_cast<float>(std::get<double>(v));
			NotifySceneChanged();
			return true;
		}, PropSlider), 0.0, 1.0, 0.01);

	c.properties = { texture, size, color, opacity };
	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterRenderComponents() {
	RegisterParticleEmitter();
	RegisterDecal();
}

} // namespace nox::render
