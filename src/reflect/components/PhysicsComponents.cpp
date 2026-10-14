// Physics descriptors: "RigidBody" (dynamics of a node's body), "Collider" (its shape and layers), "Trigger"
// (overlap volume) and "CharacterController" (capsule moved by move()). A node has at most one body; the two
// body descriptors act on it. Trigger and CharacterController state lives in the PhysicsEngine.
#include "../ComponentBuilders.h"
#include "../ReflectServices.h"
#include "../../PhysicsEngine.h"
#include "../../RigidBody.h"
#include "../../SceneNode.h"
#include "../../physics/CharacterSystem.h"
#include "../../physics/TriggerSystem.h"

#include <cmath>

namespace nox::reflect {

namespace {

std::shared_ptr<RigidBody> BodyOf(const SceneNode& n) { return n.GetRigidBody(); }

// Creates the default body for a node: a dynamic unit-mass sphere with the same defaults the scene
// loader uses for a sphere collider entry. Fails when the physics world is not available.
bool AddBodyToNode(SceneNode& node) {
	PhysicsEngine* physics = Services().physics;
	if (!physics || node.weak_from_this().expired()) return false;
	if (node.GetRigidBody()) return true;

	auto rb = std::make_shared<RigidBody>();
	rb->setShape(RigidBody::ShapeType::SPHERE);
	rb->AttachNode(node.shared_from_this());
	rb->setPosition(node.GetPosition());
	rb->setOrientation(node.GetOrientation());
	rb->setLinearDamping(0.1f);
	rb->setAngularDamping(0.1f);
	rb->setFriction(0.5f);
	rb->setBoundingRadius(0.5f);
	rb->setMass(1.0f);
	rb->setGravityScale(1.0f);
	rb->setVelocity(glm::vec3(0.0f));
	rb->setAcceleration(glm::vec3(0.0f));
	rb->computeInertiaTensor();
	rb->computeAABB();
	node.AttachRigidBody(rb);
	physics->AddBody(rb);
	NotifySceneChanged();
	return true;
}

// Unregisters the node's body from the physics world and detaches it. Fails when physics is absent.
bool RemoveBodyFromNode(SceneNode& node) {
	PhysicsEngine* physics = Services().physics;
	auto rb = node.GetRigidBody();
	if (!physics || !rb) return false;
	physics->RemoveBody(rb);
	rb->AttachNode(nullptr);
	node.AttachRigidBody(nullptr);
	NotifySceneChanged();
	return true;
}

// Shape type as the enum index used by the "shape" property (SPHERE, BOX, PLANE).
int64_t ShapeIndex(const RigidBody& rb) {
	return static_cast<int64_t>(rb.getShapeType());
}

bool FiniteAndPositive(const glm::vec3& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.x > 0.0f && v.y > 0.0f && v.z > 0.0f;
}

// Shared descriptor pieces: both descriptors refer to the same body, so the add/remove and has
// functions are common.
void AddRemoveAndHas(ComponentDesc& c) {
	c.has = [](const SceneNode& n) { return BodyOf(n) != nullptr; };
	c.add = [](SceneNode& n) { return AddBodyToNode(n); };
	c.remove = [](SceneNode& n) { return RemoveBodyFromNode(n); };
}

// Vec3 method with one argument that forwards to a RigidBody call.
MethodDesc VecMethod(const char* name, const char* tooltip, std::function<void(RigidBody&, const glm::vec3&)> fn) {
	MethodDesc m;
	m.name = name;
	m.tooltip = tooltip;
	m.args = { PropType::Vec3 };
	m.invoke = [fn = std::move(fn)](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
		auto rb = BodyOf(n);
		if (!rb) return false;
		fn(*rb, std::get<glm::vec3>(args[0]));
		return true;
	};
	return m;
}

void RegisterRigidBody() {
	ComponentDesc c;
	c.name = "RigidBody";
	c.category = "Physics";
	c.tooltip = "Simulated dynamics of the node's physics body";
	c.order = 30;
	AddRemoveAndHas(c);

	PropertyDesc bodyType = Prop("body_type", PropType::Enum,
		[](const SceneNode& n) {
			auto rb = BodyOf(n);
			return PropValue(static_cast<int64_t>(rb ? static_cast<int>(rb->GetBodyType()) : 0));
		},
		[](SceneNode& n, const PropValue& v) {
			auto rb = BodyOf(n);
			if (!rb) return false;
			rb->SetBodyType(static_cast<RigidBody::BodyType>(std::get<int64_t>(v)));
			return true;
		});
	bodyType.enumNames = { "static", "dynamic", "kinematic" };

	c.properties = {
		bodyType,
		Ranged(Prop("mass", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getMass() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setMass(static_cast<float>(std::get<double>(v)));
				return true;
			}), 0.0, 100000.0, 0.1),
		Prop("linear_velocity", PropType::Vec3,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb ? rb->getVelocity() : glm::vec3(0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setVelocity(std::get<glm::vec3>(v));
				return true;
			}, PropNoSave),
		Prop("angular_velocity", PropType::Vec3,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb ? rb->getAngularVelocity() : glm::vec3(0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setAngularVelocity(std::get<glm::vec3>(v));
				return true;
			}, PropNoSave),
		Ranged(Prop("gravity_scale", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getGravityScale() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setGravityScale(static_cast<float>(std::get<double>(v)));
				return true;
			}), -10.0, 10.0, 0.01),
		Ranged(Prop("linear_damping", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getLinearDamping() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setLinearDamping(static_cast<float>(std::get<double>(v)));
				return true;
			}), 0.0, 10.0, 0.01),
		Ranged(Prop("angular_damping", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getAngularDamping() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setAngularDamping(static_cast<float>(std::get<double>(v)));
				return true;
			}), 0.0, 10.0, 0.01),
		Ranged(Prop("friction", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getFriction() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setFriction(static_cast<float>(std::get<double>(v)));
				return true;
			}), 0.0, 1.0, 0.01),
		Ranged(Prop("restitution", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getRestitution() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setRestitution(static_cast<float>(std::get<double>(v)));
				return true;
			}), 0.0, 1.0, 0.01),
		Prop("sleeping", PropType::Bool,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb && rb->isSleeping()); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setSleeping(std::get<bool>(v));
				return true;
			}, PropNoSave),
	};

	c.methods.push_back(VecMethod("apply_force", "Apply a force at the centre of mass (dynamic bodies)",
		[](RigidBody& rb, const glm::vec3& f) { rb.applyForce(f); }));
	c.methods.push_back(VecMethod("apply_impulse", "Apply an instantaneous impulse (dynamic bodies)",
		[](RigidBody& rb, const glm::vec3& i) { rb.applyImpulse(i); }));
	c.methods.push_back(VecMethod("apply_torque", "Apply a torque (dynamic bodies)",
		[](RigidBody& rb, const glm::vec3& t) { rb.applyTorque(t); }));
	c.methods.push_back(VoidMethod("wake", "Wake a sleeping body",
		[](SceneNode& n) {
			auto rb = BodyOf(n);
			if (!rb) return false;
			rb->wakeUp();
			return true;
		}));
	c.methods.push_back(VoidMethod("sleep", "Put the body to sleep",
		[](SceneNode& n) {
			auto rb = BodyOf(n);
			if (!rb) return false;
			rb->setSleeping(true);
			return true;
		}));

	Registry::Get().Register(std::move(c));
}

// The shape setters below keep the shape type: RigidBody::setBox/setPlane also switch the type, so
// the previous type is restored after the parameter change.
void RegisterCollider() {
	ComponentDesc c;
	c.name = "Collider";
	c.category = "Physics";
	c.tooltip = "Collision shape and layers of the node's physics body";
	c.order = 31;
	AddRemoveAndHas(c);

	PropertyDesc shape = Prop("shape", PropType::Enum,
		[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb ? ShapeIndex(*rb) : int64_t(0)); },
		[](SceneNode& n, const PropValue& v) {
			auto rb = BodyOf(n);
			if (!rb) return false;
			switch (std::get<int64_t>(v)) {
			case 0: rb->setSphere(rb->getBoundingRadius()); break;
			case 1: rb->setBox(rb->getHalfExtents()); break;
			case 2: rb->setPlane(rb->getPlaneNormal(), rb->getPlaneDistance()); break;
			default: return false;
			}
			rb->computeAABB();
			NotifySceneChanged();
			return true;
		});
	shape.enumNames = { "sphere", "box", "plane" };

	c.properties = {
		shape,
		// Only sphere colliders have an authored radius; box and plane bodies carry a derived bounding radius that is
		// not saved (it would be clamped to the range on load).
		[] {
			PropertyDesc p = Ranged(Prop("radius", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getBoundingRadius() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setBoundingRadius(static_cast<float>(std::get<double>(v)));
				rb->computeInertiaTensor();
				rb->computeAABB();
				NotifySceneChanged();
				return true;
			}), 0.01, 10000.0, 0.01);
			p.applies = [](const SceneNode& n) { auto rb = BodyOf(n); return rb && rb->getShapeType() == RigidBody::ShapeType::SPHERE; };
			return p;
		}(),
		Prop("half_extents", PropType::Vec3,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb ? rb->getHalfExtents() : glm::vec3(0.5f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				const glm::vec3 half = std::get<glm::vec3>(v);
				if (!rb || !FiniteAndPositive(half)) return false;
				const auto shapeType = rb->getShapeType();
				rb->setBox(half);
				rb->setShape(shapeType);
				rb->computeAABB();
				NotifySceneChanged();
				return true;
			}),
		Prop("plane_normal", PropType::Vec3,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(rb ? rb->getPlaneNormal() : glm::vec3(0.0f, 1.0f, 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				const glm::vec3 normal = std::get<glm::vec3>(v);
				if (!rb || !std::isfinite(glm::length(normal)) || glm::length(normal) < 1e-6f) return false;
				const auto shapeType = rb->getShapeType();
				rb->setPlane(normal, rb->getPlaneDistance());
				rb->setShape(shapeType);
				rb->computeAABB();
				NotifySceneChanged();
				return true;
			}),
		Ranged(Prop("plane_distance", PropType::Float,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<double>(rb ? rb->getPlaneDistance() : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				const auto shapeType = rb->getShapeType();
				rb->setPlane(rb->getPlaneNormal(), static_cast<float>(std::get<double>(v)));
				rb->setShape(shapeType);
				rb->computeAABB();
				NotifySceneChanged();
				return true;
			}), -100000.0, 100000.0, 0.1),
		Ranged(Prop("layer", PropType::Int,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<int64_t>(rb ? rb->getCollisionLayer() : 0u)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setCollisionLayer(static_cast<uint32_t>(std::get<int64_t>(v)));
				return true;
			}), 0.0, 2147483647.0, 1.0),
		Ranged(Prop("mask", PropType::Int,
			[](const SceneNode& n) { auto rb = BodyOf(n); return PropValue(static_cast<int64_t>(rb ? rb->getCollisionMask() : 0u)); },
			[](SceneNode& n, const PropValue& v) {
				auto rb = BodyOf(n);
				if (!rb) return false;
				rb->setCollisionMask(static_cast<uint32_t>(std::get<int64_t>(v)));
				return true;
			}), 0.0, 4294967295.0, 1.0),   // full 32-bit mask (the default is all bits): the range must hold it
	};

	Registry::Get().Register(std::move(c));
}

// Trigger: a volume on any node, with or without a body. State lives in PhysicsEngine::Triggers().
TriggerSettings* TriggerOf(const SceneNode& n) {
	PhysicsEngine* physics = Services().physics;
	return physics ? physics->Triggers().Find(&n) : nullptr;
}

CharacterSettings* CharacterOf(const SceneNode& n) {
	PhysicsEngine* physics = Services().physics;
	return physics ? physics->Characters().Find(&n) : nullptr;
}

void RegisterTrigger() {
	ComponentDesc c;
	c.name = "Trigger";
	c.category = "Physics";
	c.tooltip = "Volume that reports bodies entering and leaving it (does not collide)";
	c.order = 32;
	c.has = [](const SceneNode& n) { return TriggerOf(n) != nullptr; };
	c.add = [](SceneNode& n) {
		PhysicsEngine* physics = Services().physics;
		if (!physics || n.weak_from_this().expired() || !physics->Triggers().Add(n.shared_from_this())) return false;
		NotifySceneChanged();
		return true;
	};
	c.remove = [](SceneNode& n) {
		PhysicsEngine* physics = Services().physics;
		if (!physics || !TriggerOf(n)) return false;
		physics->Triggers().Remove(&n);
		NotifySceneChanged();
		return true;
	};

	PropertyDesc shape = Prop("shape", PropType::Enum,
		[](const SceneNode& n) {
			const TriggerSettings* t = TriggerOf(n);
			return PropValue(static_cast<int64_t>(t ? static_cast<int>(t->shape) : 0));
		},
		[](SceneNode& n, const PropValue& v) {
			TriggerSettings* t = TriggerOf(n);
			const int64_t index = std::get<int64_t>(v);
			if (!t || index < 0 || index > 1) return false;
			t->shape = static_cast<TriggerSettings::Shape>(index);
			NotifySceneChanged();
			return true;
		});
	shape.enumNames = { "sphere", "box" };

	c.properties = {
		shape,
		Ranged(Prop("radius", PropType::Float,
			[](const SceneNode& n) { const TriggerSettings* t = TriggerOf(n); return PropValue(static_cast<double>(t ? t->radius : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				TriggerSettings* t = TriggerOf(n);
				if (!t) return false;
				t->radius = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}), 0.01, 10000.0, 0.01),
		Prop("half_extents", PropType::Vec3,
			[](const SceneNode& n) { const TriggerSettings* t = TriggerOf(n); return PropValue(t ? t->halfExtents : glm::vec3(0.5f)); },
			[](SceneNode& n, const PropValue& v) {
				TriggerSettings* t = TriggerOf(n);
				const glm::vec3 half = std::get<glm::vec3>(v);
				if (!t || !FiniteAndPositive(half)) return false;
				t->halfExtents = half;
				NotifySceneChanged();
				return true;
			}),
		Ranged(Prop("layer", PropType::Int,
			[](const SceneNode& n) { const TriggerSettings* t = TriggerOf(n); return PropValue(static_cast<int64_t>(t ? t->layer : 0u)); },
			[](SceneNode& n, const PropValue& v) {
				TriggerSettings* t = TriggerOf(n);
				if (!t) return false;
				t->layer = static_cast<uint32_t>(std::get<int64_t>(v));
				NotifySceneChanged();
				return true;
			}), 0.0, 2147483647.0, 1.0),
		Ranged(Prop("mask", PropType::Int,
			[](const SceneNode& n) { const TriggerSettings* t = TriggerOf(n); return PropValue(static_cast<int64_t>(t ? t->mask : 0u)); },
			[](SceneNode& n, const PropValue& v) {
				TriggerSettings* t = TriggerOf(n);
				if (!t) return false;
				t->mask = static_cast<uint32_t>(std::get<int64_t>(v));
				NotifySceneChanged();
				return true;
			}), 0.0, 2147483647.0, 1.0),
		Prop("enabled", PropType::Bool,
			[](const SceneNode& n) { const TriggerSettings* t = TriggerOf(n); return PropValue(t != nullptr && t->enabled); },
			[](SceneNode& n, const PropValue& v) {
				TriggerSettings* t = TriggerOf(n);
				if (!t) return false;
				t->enabled = std::get<bool>(v);
				NotifySceneChanged();
				return true;
			}),
	};

	Registry::Get().Register(std::move(c));
}

// CharacterController: a capsule moved by move(displacement) (see physics/CharacterSystem.h).
void RegisterCharacterController() {
	ComponentDesc c;
	c.name = "CharacterController";
	c.category = "Physics";
	c.tooltip = "Capsule character moved by move(), slides along walls and floors";
	c.order = 33;
	c.has = [](const SceneNode& n) { return CharacterOf(n) != nullptr; };
	c.add = [](SceneNode& n) {
		PhysicsEngine* physics = Services().physics;
		if (!physics || n.weak_from_this().expired() || !physics->Characters().Add(n.shared_from_this())) return false;
		NotifySceneChanged();
		return true;
	};
	c.remove = [](SceneNode& n) {
		PhysicsEngine* physics = Services().physics;
		if (!physics || !CharacterOf(n)) return false;
		physics->Characters().Remove(&n);
		NotifySceneChanged();
		return true;
	};

	c.properties = {
		Ranged(Prop("height", PropType::Float,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(static_cast<double>(s ? s->height : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				if (!s) return false;
				s->height = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}), 0.1, 100.0, 0.01),
		Ranged(Prop("radius", PropType::Float,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(static_cast<double>(s ? s->radius : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				if (!s) return false;
				s->radius = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}), 0.01, 10.0, 0.01),
		Ranged(Prop("step_offset", PropType::Float,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(static_cast<double>(s ? s->stepOffset : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				if (!s) return false;
				s->stepOffset = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}), 0.0, 10.0, 0.01),
		Ranged(Prop("slope_limit", PropType::Float,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(static_cast<double>(s ? s->slopeLimitDegrees : 0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				if (!s) return false;
				s->slopeLimitDegrees = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}, PropAngle), 0.0, 89.0, 1.0),
		Prop("gravity", PropType::Bool,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(s != nullptr && s->gravity); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				if (!s) return false;
				s->gravity = std::get<bool>(v);
				NotifySceneChanged();
				return true;
			}),
		Prop("velocity", PropType::Vec3,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(s ? s->velocity : glm::vec3(0.0f)); },
			[](SceneNode& n, const PropValue& v) {
				CharacterSettings* s = CharacterOf(n);
				const glm::vec3 velocity = std::get<glm::vec3>(v);
				if (!s || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) return false;
				s->velocity = velocity;
				return true;
			}, PropNoSave),
		RuntimeProp("is_on_floor", PropType::Bool,
			[](const SceneNode& n) { const CharacterSettings* s = CharacterOf(n); return PropValue(s != nullptr && s->onFloor); }),
	};

	MethodDesc move;
	move.name = "move";
	move.tooltip = "Move by a displacement, sliding along what is hit; returns true when something was touched";
	move.args = { PropType::Vec3 };
	move.returnsValue = true;
	move.invoke = [](SceneNode& n, const std::vector<PropValue>& args, PropValue& result) {
		PhysicsEngine* physics = Services().physics;
		if (!physics || !CharacterOf(n)) return false;
		result = PropValue(physics->Characters().Move(*physics, n, std::get<glm::vec3>(args[0])));
		return true;
	};
	c.methods.push_back(move);

	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterPhysicsComponents() {
	RegisterRigidBody();
	RegisterCollider();
	RegisterTrigger();
	RegisterCharacterController();
}

} // namespace nox::reflect
