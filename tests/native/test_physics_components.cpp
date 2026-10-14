// Physics through nodes, components and scripts: overlap and ray queries, trigger enter/exit with a moving body,
// body contact begin/end, the character controller (slide along a wall, stop on a floor, gravity), the
// Trigger and CharacterController descriptors, and the Lua queries and event delivery. Headless: no GPU.
#include "TestFramework.h"

#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "physics/CharacterSystem.h"
#include "physics/TriggerSystem.h"
#include "reflect/ReflectServices.h"
#include "reflect/Reflection.h"
#include "scripting/LuaWorld.h"

#include "json.hpp"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using nox::reflect::PropValue;
using nox::reflect::Registry;
using nox::reflect::Services;
using nox::reflect::GetProperty;
using nox::reflect::SetProperty;

namespace {

	const float kDt = 1.0f / 60.0f;

	// Restores the engine service pointer on scope exit so tests cannot leak a physics engine.
	struct ServicesGuard {
		PhysicsEngine* physics;
		ServicesGuard() : physics(Services().physics) {}
		~ServicesGuard() { Services().physics = physics; }
	};

	std::shared_ptr<RigidBody> MakeSphereBody(PhysicsEngine& physics, const glm::vec3& pos, float radius,
		RigidBody::BodyType type, const glm::vec3& velocity = glm::vec3(0.0f)) {
		auto rb = std::make_shared<RigidBody>();
		rb->SetBodyType(type);
		rb->setSphere(radius);
		rb->setPosition(pos);
		rb->setMass(1.0f);
		rb->setVelocity(velocity);
		rb->setLinearDamping(0.0f);
		rb->setAngularDamping(0.0f);
		rb->computeAABB();
		physics.AddBody(rb);
		return rb;
	}

	std::shared_ptr<RigidBody> MakeBoxBody(PhysicsEngine& physics, const glm::vec3& pos, const glm::vec3& half,
		RigidBody::BodyType type = RigidBody::BodyType::STATIC, const glm::quat& orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
		auto rb = std::make_shared<RigidBody>();
		rb->SetBodyType(type);
		rb->setBox(half);
		rb->setPosition(pos);
		rb->setOrientation(orientation);
		rb->setMass(1.0f);
		rb->computeAABB();
		physics.AddBody(rb);
		return rb;
	}

	std::shared_ptr<RigidBody> MakePlaneBody(PhysicsEngine& physics, const glm::vec3& normal, float distance) {
		auto rb = std::make_shared<RigidBody>();
		rb->SetBodyType(RigidBody::BodyType::STATIC);
		rb->setPlane(normal, distance);
		physics.AddBody(rb);
		return rb;
	}

	bool Contains(const std::vector<PhysicsEngine::BodyID>& ids, PhysicsEngine::BodyID id) {
		return std::find(ids.begin(), ids.end(), id) != ids.end();
	}

	// Scene graph helper: nodes under the root with ECS entities (headless, as in test_lua_components).
	struct TestScene {
		SceneGraph graph;

		std::shared_ptr<SceneNode> Make(const char* name, const glm::vec3& pos) {
			auto node = std::make_shared<SceneNode>(graph.GetComponentManager(), graph.GetTransformSystem());
			node->SetName(name);
			graph.GetRoot()->AddChild(node);
			node->SetECSContext(graph.GetComponentManager(), graph.GetTransformSystem());
			node->CreateECSEntity(name);
			node->SetPosition(pos);
			return node;
		}
	};

	// Gives a node a body (as the RigidBody descriptor does) and registers it with the engine.
	void Attach(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<RigidBody>& body) {
		body->AttachNode(node);
		node->AttachRigidBody(body);
	}

	bool NearPoint(const glm::vec3& a, const glm::vec3& b, float eps) {
		return glm::length(a - b) <= eps;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsComponents, OverlapQueriesReportBodiesByShapeAndLayer)
{
	PhysicsEngine physics;
	auto sphere = MakeSphereBody(physics, glm::vec3(0.0f), 1.0f, RigidBody::BodyType::STATIC);
	auto box = MakeBoxBody(physics, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f));

	auto near = physics.OverlapSphere(glm::vec3(0.0f), 0.5f);
	NOX_CHECK(Contains(near, sphere->GetBodyID()));
	NOX_CHECK(!Contains(near, box->GetBodyID()));

	NOX_CHECK(Contains(physics.OverlapSphere(glm::vec3(4.5f, 0.0f, 0.0f), 0.6f), box->GetBodyID()));
	NOX_CHECK(physics.OverlapSphere(glm::vec3(2.5f, 0.0f, 0.0f), 0.5f).empty());
	NOX_CHECK(physics.OverlapSphere(glm::vec3(0.0f), 0.0f).empty());

	auto onBox = physics.OverlapBox(glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.1f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	NOX_CHECK(Contains(onBox, box->GetBodyID()));
	NOX_CHECK(!Contains(onBox, sphere->GetBodyID()));

	// Layer filter: a body is reported when its layer intersects the mask.
	box->setCollisionLayer(2);
	NOX_CHECK(!Contains(physics.OverlapSphere(glm::vec3(5.0f, 0.0f, 0.0f), 0.5f, 1u), box->GetBodyID()));
	NOX_CHECK(Contains(physics.OverlapSphere(glm::vec3(5.0f, 0.0f, 0.0f), 0.5f, 2u), box->GetBodyID()));
	NOX_CHECK(Contains(physics.OverlapSphere(glm::vec3(5.0f, 0.0f, 0.0f), 0.5f), box->GetBodyID()));

	// Rotation: a thin bar along world Y (rotated 90 degrees about Z) is not hit at x = 0.5, but is at x = 0.2.
	auto bar = MakeBoxBody(physics, glm::vec3(-3.0f, 3.0f, 0.0f), glm::vec3(3.0f, 0.1f, 0.1f), RigidBody::BodyType::STATIC,
		glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
	NOX_CHECK(!Contains(physics.OverlapSphere(glm::vec3(-2.5f, 4.0f, 0.0f), 0.3f), bar->GetBodyID()));
	NOX_CHECK(Contains(physics.OverlapSphere(glm::vec3(-3.2f, 4.0f, 0.0f), 0.3f), bar->GetBodyID()));

	// Body <-> entity lookup goes through the attached node.
	NOX_CHECK_EQ(physics.GetEntityForBody(sphere->GetBodyID()), static_cast<EntityID>(INVALID_ENTITY));
	NOX_CHECK_EQ(physics.GetBodyForEntity(INVALID_ENTITY), RigidBody::INVALID_BODY_ID);
}

NOX_TEST(PhysicsComponents, RayCastClosestHitsExactShapesWithNormals)
{
	PhysicsEngine physics;
	auto sphere = MakeSphereBody(physics, glm::vec3(0.0f), 1.0f, RigidBody::BodyType::STATIC);
	auto box = MakeBoxBody(physics, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f));
	auto plane = std::make_shared<RigidBody>();
	plane->SetBodyType(RigidBody::BodyType::STATIC);
	plane->setPlane(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
	physics.AddBody(plane);

	const uint32_t all = 0xFFFFFFFFu;
	PhysicsRayHit hit;
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, all, hit));
	NOX_CHECK(hit.body == sphere);
	NOX_CHECK(std::fabs(hit.distance - 9.0f) < 1e-3f);
	NOX_CHECK(NearPoint(hit.normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f));

	// The direction does not need to be normalized.
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(4.0f, 0.0f, 0.0f), 100.0f, all, hit));
	NOX_CHECK(std::fabs(hit.distance - 9.0f) < 1e-3f);

	// Nothing within 5 units.
	NOX_CHECK(!physics.RayCastClosest(glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 5.0f, all, hit));
	NOX_CHECK(!hit.hit);

	// Layer mask: with the sphere on layer 2, a ray over mask 1 passes it and hits the box.
	sphere->setCollisionLayer(2);
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 100.0f, 1u, hit));
	NOX_CHECK(hit.body == box);
	NOX_CHECK(std::fabs(hit.distance - 14.0f) < 1e-3f);
	NOX_CHECK(NearPoint(hit.point, glm::vec3(4.0f, 0.0f, 0.0f), 1e-3f));
	sphere->setCollisionLayer(1);

	// Planes are hit by downward rays, with the plane normal.
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(3.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, all, hit));
	NOX_CHECK(hit.body == plane);
	NOX_CHECK(std::fabs(hit.distance - 10.0f) < 1e-3f);
	NOX_CHECK(NearPoint(hit.normal, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f));

	// A ray parallel to the plane never hits it.
	NOX_CHECK(!physics.RayCastClosest(glm::vec3(3.0f, 3.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 2.0f, all, hit));
}

// ---------------------------------------------------------------------------------------------------------------
// Trigger volumes and contact events
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsComponents, TriggerReportsEnterAndExitForMovingBody)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	physics.SetGravity(glm::vec3(0.0f));
	TestScene scene;
	auto zone = scene.Make("zone", glm::vec3(0.0f));
	auto mover = scene.Make("mover", glm::vec3(-5.0f, 0.0f, 0.0f));

	NOX_REQUIRE(physics.Triggers().Add(zone));
	TriggerSettings* trigger = physics.Triggers().Find(zone.get());
	NOX_REQUIRE(trigger != nullptr);
	trigger->radius = 1.0f;
	NOX_CHECK(trigger->enabled);

	auto body = MakeSphereBody(physics, glm::vec3(-5.0f, 0.0f, 0.0f), 0.3f, RigidBody::BodyType::DYNAMIC, glm::vec3(3.0f, 0.0f, 0.0f));
	Attach(mover, body);

	std::vector<TriggerEvent> events;
	for (int frame = 0; frame < 200; ++frame) {
		physics.Update(kDt);
		for (const TriggerEvent& event : physics.Triggers().TakeEvents()) events.push_back(event);
	}

	NOX_REQUIRE(events.size() == 2);
	NOX_CHECK(events[0].enter);
	NOX_CHECK(!events[1].enter);
	NOX_CHECK(events[0].trigger == zone->GetEntityID());
	NOX_CHECK(events[0].other == mover->GetEntityID());
	NOX_CHECK(events[1].other == mover->GetEntityID());

	// Disabling a trigger that holds an overlap reports the exit; re-enabling reports nothing until a body is inside.
	auto other = scene.Make("other", glm::vec3(0.0f, 0.0f, 0.0f));
	auto parked = MakeSphereBody(physics, glm::vec3(0.0f), 0.2f, RigidBody::BodyType::STATIC);
	Attach(other, parked);
	physics.Update(kDt);
	auto first = physics.Triggers().TakeEvents();
	NOX_REQUIRE(first.size() == 1);
	NOX_CHECK(first[0].enter);
	NOX_CHECK(SetProperty(*zone, "Trigger", "enabled", PropValue(false)));
	physics.Update(kDt);
	auto second = physics.Triggers().TakeEvents();
	NOX_REQUIRE(second.size() == 1);
	NOX_CHECK(!second[0].enter);
	physics.Update(kDt);
	NOX_CHECK(physics.Triggers().TakeEvents().empty());

	// Removing the trigger while overlapping reports the exit as well.
	NOX_CHECK(SetProperty(*zone, "Trigger", "enabled", PropValue(true)));
	physics.Update(kDt);
	NOX_CHECK(physics.Triggers().TakeEvents().size() == 1);
	physics.Triggers().Remove(zone.get());
	auto leftover = physics.Triggers().TakeEvents();
	NOX_REQUIRE(leftover.size() == 1);
	NOX_CHECK(!leftover[0].enter);
	NOX_CHECK(!physics.Triggers().Has(zone.get()));
}

NOX_TEST(PhysicsComponents, ContactEventsBeginAndEnd)
{
	PhysicsEngine physics;
	auto floor = MakePlaneBody(physics, glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
	auto ball = MakeSphereBody(physics, glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, RigidBody::BodyType::DYNAMIC);

	std::vector<PhysicsContactEvent> events;
	for (int frame = 0; frame < 150; ++frame) {
		physics.Update(kDt);
		for (const PhysicsContactEvent& event : physics.TakeContactEvents()) events.push_back(event);
	}
	NOX_REQUIRE(!events.empty());
	NOX_CHECK(events[0].type == PhysicsContactEvent::Type::BEGIN);
	const bool sameBodies = (events[0].bodyA == floor && events[0].bodyB == ball) ||
		(events[0].bodyA == ball && events[0].bodyB == floor);
	NOX_CHECK(sameBodies);

	// Bounce away: the pair stops touching, which is reported once.
	ball->wakeUp();
	ball->setVelocity(glm::vec3(0.0f, 8.0f, 0.0f));
	events.clear();
	for (int frame = 0; frame < 60; ++frame) {
		physics.Update(kDt);
		for (const PhysicsContactEvent& event : physics.TakeContactEvents()) events.push_back(event);
	}
	NOX_REQUIRE(events.size() == 1);
	NOX_CHECK(events[0].type == PhysicsContactEvent::Type::END);
}

// ---------------------------------------------------------------------------------------------------------------
// Character controller
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsComponents, CharacterSlidesAlongWallAndStopsOnFloor)
{
	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	MakeBoxBody(physics, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f));     // floor, top at y = 0
	MakeBoxBody(physics, glm::vec3(2.5f, 2.0f, 0.0f), glm::vec3(0.5f, 2.0f, 10.0f));      // wall, x in [2, 3]

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(0.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	CharacterSettings* settings = physics.Characters().Find(hero.get());
	NOX_REQUIRE(settings != nullptr);
	settings->gravity = false;

	// Feet on the floor, moving down: stops on it and reports the floor.
	NOX_CHECK(physics.Characters().Move(physics, *hero, glm::vec3(0.0f, -0.5f, 0.0f)));
	NOX_CHECK(settings->onFloor);
	NOX_CHECK(std::fabs(hero->GetWorldPosition().y) < 0.02f);

	// Walk diagonally into the wall: x stops at wall minus radius, z keeps moving (slides).
	const float z0 = hero->GetWorldPosition().z;
	physics.Characters().Move(physics, *hero, glm::vec3(3.0f, 0.0f, 1.0f));
	const glm::vec3 p = hero->GetWorldPosition();
	NOX_CHECK(p.x > 1.5f && p.x < 1.65f);
	NOX_CHECK(p.z - z0 > 0.8f);
	NOX_CHECK(std::fabs(p.y) < 0.02f);
	NOX_CHECK(settings->onFloor);

	// In the air: no floor, nothing touched.
	auto air = scene.Make("air", glm::vec3(-5.0f, 5.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(air));
	physics.Characters().Find(air.get())->gravity = false;
	NOX_CHECK(!physics.Characters().Move(physics, *air, glm::vec3(0.0f, 0.1f, 0.0f)));
	NOX_CHECK(!physics.Characters().Find(air.get())->onFloor);
}

NOX_TEST(PhysicsComponents, CharacterGravityLandsAndSettlesOnFloor)
{
	PhysicsEngine physics;   // default gravity (-9.81 on y)
	MakeBoxBody(physics, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f));

	TestScene scene;
	auto faller = scene.Make("faller", glm::vec3(-4.0f, 2.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(faller));
	for (int frame = 0; frame < 120; ++frame) physics.Update(kDt);

	const CharacterSettings* settings = physics.Characters().Find(faller.get());
	NOX_REQUIRE(settings != nullptr);
	NOX_CHECK(settings->onFloor);
	NOX_CHECK(std::fabs(faller->GetWorldPosition().y) < 0.02f);
	NOX_CHECK(settings->velocity.y == 0.0f);

	// A jump: upward velocity lifts the character off the floor.
	physics.Characters().Find(faller.get())->velocity.y = 4.0f;
	physics.Update(kDt);
	NOX_CHECK(!physics.Characters().Find(faller.get())->onFloor);
	NOX_CHECK(faller->GetWorldPosition().y > 0.02f);
}

// ---------------------------------------------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsComponents, TriggerAndCharacterDescriptorsRoundTrip)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;

	TestScene scene;
	auto node = scene.Make("sensor", glm::vec3(0.0f));
	NOX_REQUIRE(Registry::Get().Find("Trigger")->add(*node));
	NOX_CHECK(physics.Triggers().Has(node.get()));
	NOX_CHECK(Registry::Get().Find("Trigger")->has(*node));
	NOX_CHECK(SetProperty(*node, "Trigger", "shape", PropValue(int64_t(1))));
	NOX_CHECK(SetProperty(*node, "Trigger", "half_extents", PropValue(glm::vec3(1.0f, 2.0f, 3.0f))));
	NOX_CHECK(!SetProperty(*node, "Trigger", "half_extents", PropValue(glm::vec3(0.0f, 1.0f, 1.0f))));
	NOX_CHECK(SetProperty(*node, "Trigger", "mask", PropValue(int64_t(6))));
	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Trigger", "half_extents", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(1.0f, 2.0f, 3.0f));
	const TriggerSettings* trigger = physics.Triggers().Find(node.get());
	NOX_REQUIRE(trigger != nullptr);
	NOX_CHECK(trigger->shape == TriggerSettings::Shape::BOX);
	NOX_CHECK(trigger->mask == 6u);

	NOX_REQUIRE(Registry::Get().Find("CharacterController")->add(*node));
	NOX_CHECK(SetProperty(*node, "CharacterController", "height", PropValue(2.0)));
	NOX_CHECK(SetProperty(*node, "CharacterController", "gravity", PropValue(false)));
	NOX_CHECK(!SetProperty(*node, "CharacterController", "is_on_floor", PropValue(true)));   // runtime, read-only
	NOX_CHECK(GetProperty(*node, "CharacterController", "height", value));
	NOX_CHECK(std::get<double>(value) == 2.0);
	const CharacterSettings* character = physics.Characters().Find(node.get());
	NOX_REQUIRE(character != nullptr);
	NOX_CHECK(character->height == 2.0f);
	NOX_CHECK(!character->gravity);

	// The move method returns the collided flag through the registry.
	const nox::reflect::MethodDesc* move = nullptr;
	for (const auto& method : Registry::Get().Find("CharacterController")->methods) {
		if (method.name == "move") move = &method;
	}
	NOX_REQUIRE(move != nullptr);
	PropValue result;
	NOX_CHECK(move->invoke(*node, { PropValue(glm::vec3(0.0f, 0.0f, 0.0f)) }, result));
	NOX_CHECK(std::holds_alternative<bool>(result));

	NOX_REQUIRE(Registry::Get().Find("Trigger")->remove(*node));
	NOX_CHECK(!physics.Triggers().Has(node.get()));
	NOX_REQUIRE(Registry::Get().Find("CharacterController")->remove(*node));
	NOX_CHECK(!physics.Characters().Has(node.get()));
}

// ---------------------------------------------------------------------------------------------------------------
// Lua: queries and events
// ---------------------------------------------------------------------------------------------------------------

namespace {
	class MemorySource : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const auto it = files.find(logicalPath);
			if (it == files.end()) {
				error = "no such file";
				return false;
			}
			text = it->second;
			return true;
		}
	};

	const char* kPhysicsEntry = "projects/t/scripts/main.lua";

	// The script records what it sees in globals; the test reads them back.
	const char* kPhysicsScript =
		"enters = 0\n"
		"exits = 0\n"
		"enter_ok = false\n"
		"ray_hit = false\n"
		"ray_on_post = false\n"
		"ray_distance = -1\n"
		"overlap_count = -1\n"
		"overlap_on_post = false\n"
		"contact_events = 0\n"
		"nox.on('trigger_enter', function(ctx, ev)\n"
		"  enters = enters + 1\n"
		"  enter_ok = ev.trigger == ctx:find('zone') and ev.other == ctx:find('mover')\n"
		"end)\n"
		"nox.on('trigger_exit', function(ctx, ev) exits = exits + 1 end)\n"
		"nox.on('contact_begin', function(ctx, ev) contact_events = contact_events + 1 end)\n"
		"nox.system{name = 'probe', update = function(ctx, dt)\n"
		"  if ctx:frame() ~= 1 then return end\n"
		"  local hit = ctx:physics_raycast({x = -10, y = 0, z = 0}, {x = 1, y = 0, z = 0}, 100)\n"
		"  if hit then\n"
		"    ray_hit = true\n"
		"    ray_on_post = hit.entity == ctx:find('post')\n"
		"    ray_distance = hit.distance\n"
		"  end\n"
		"  local found = ctx:overlap_sphere({x = -6, y = 0, z = 0}, 0.2)\n"
		"  overlap_count = #found\n"
		"  overlap_on_post = found[1] == ctx:find('post')\n"
		"end}\n";

	struct PhysicsLuaHarness {
		ServicesGuard guard;
		MemorySource source;
		PhysicsEngine physics;
		TestScene scene;
		NoxLua::LuaWorld world;

		PhysicsLuaHarness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
			Services().physics = &physics;
			nox::reflect::RegisterBuiltinComponents();
		}

		bool Load(const std::string& body) {
			source.files[kPhysicsEntry] = body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kPhysicsEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene.graph, nlohmann::json::object());
		}

		// One frame: physics first (as Core does), then the Lua world.
		void Frame() {
			physics.Update(kDt);
			world.Update(kDt, true);
		}

		lua_Integer GlobalInt(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const lua_Integer value = lua_tointeger(L, -1);
			lua_pop(L, 1);
			return value;
		}

		double GlobalNumber(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const double value = lua_tonumber(L, -1);
			lua_pop(L, 1);
			return value;
		}

		bool GlobalBool(const char* name) {
			lua_State* L = world.GetRuntime().State();
			lua_getglobal(L, name);
			const bool value = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		}
	};
}

NOX_TEST(PhysicsComponents, LuaRaycastOverlapAndTriggerEventsAreDelivered)
{
	PhysicsLuaHarness h;
	auto post = h.scene.Make("post", glm::vec3(-6.0f, 0.0f, 0.0f));
	Attach(post, MakeSphereBody(h.physics, glm::vec3(-6.0f, 0.0f, 0.0f), 0.5f, RigidBody::BodyType::STATIC));
	auto zone = h.scene.Make("zone", glm::vec3(0.0f));
	NOX_REQUIRE(h.physics.Triggers().Add(zone));
	auto mover = h.scene.Make("mover", glm::vec3(-5.0f, 0.0f, 0.0f));
	h.physics.SetGravity(glm::vec3(0.0f));
	Attach(mover, MakeSphereBody(h.physics, glm::vec3(-5.0f, 0.0f, 0.0f), 0.3f, RigidBody::BodyType::DYNAMIC,
		glm::vec3(3.0f, 0.0f, 0.0f)));
	NOX_REQUIRE(h.Load(kPhysicsScript));
	h.world.BeginPlay();

	for (int frame = 0; frame < 200; ++frame) h.Frame();

	NOX_CHECK(h.GlobalBool("ray_hit"));
	NOX_CHECK(h.GlobalBool("ray_on_post"));
	NOX_CHECK(std::fabs(h.GlobalNumber("ray_distance") - 3.5) < 1e-2);
	NOX_CHECK(h.GlobalInt("overlap_count") == 1);
	NOX_CHECK(h.GlobalBool("overlap_on_post"));
	NOX_CHECK(h.GlobalInt("enters") == 1);
	NOX_CHECK(h.GlobalInt("exits") == 1);
	NOX_CHECK(h.GlobalBool("enter_ok"));
	NOX_CHECK(h.world.Diagnostics().empty());
}
