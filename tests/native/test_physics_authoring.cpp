// Physics authoring follow-ups: StaticCollision components (generated mesh colliders for the nodes under a node),
// static meshes that follow their node, box contacts and sweeps against meshes, characters carried by a moving
// platform, character triggers and controller hits. Headless: no GPU.
#include "TestFramework.h"

#include "CollisionWorld.h"
#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "physics/CharacterSystem.h"
#include "physics/MeshCollider.h"
#include "physics/ShapeSweep.h"
#include "physics/StaticCollision.h"
#include "physics/TriggerSystem.h"
#include "reflect/ReflectServices.h"
#include "reflect/Reflection.h"

#include <cmath>
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

// A glTF file with triangles in the repository (the character demo level: a few dozen triangles).
const char* kLevelPath = "projects/character-demo/models/level.glb";

struct ServicesGuard {
	PhysicsEngine* physics;
	ServicesGuard() : physics(Services().physics) {}
	~ServicesGuard() { Services().physics = physics; }
};

struct TestScene {
	SceneGraph graph;

	// A node under parent (the scene root when null).
	std::shared_ptr<SceneNode> Make(const char* name, const glm::vec3& pos, const std::shared_ptr<SceneNode>& parent = nullptr) {
		auto node = std::make_shared<SceneNode>(graph.GetComponentManager(), graph.GetTransformSystem());
		node->SetName(name);
		(parent ? parent : graph.GetRoot())->AddChild(node);
		node->SetECSContext(graph.GetComponentManager(), graph.GetTransformSystem());
		node->CreateECSEntity(name);
		node->SetPosition(pos);
		return node;
	}
};

// Mesh body with a square floor at y = 0 (half size `half`), placed at the node's world transform.
std::shared_ptr<RigidBody> MeshFloorOn(PhysicsEngine& physics, const std::shared_ptr<SceneNode>& node, float half) {
	std::vector<glm::vec3> pos = {
		glm::vec3(-half, 0.0f, -half), glm::vec3(half, 0.0f, -half), glm::vec3(half, 0.0f, half), glm::vec3(-half, 0.0f, half) };
	const std::vector<uint32_t> idx = { 0, 1, 2, 0, 2, 3 };
	auto world = std::make_shared<CollisionWorld>();
	world->AddMesh("floor", pos, idx);
	glm::vec3 mn, mx;
	MeshCollider::LocalBounds(*world, mn, mx);
	auto rb = std::make_shared<RigidBody>();
	rb->setMesh(world, mn, mx, "");
	rb->AttachNode(node);
	rb->setPosition(node->GetWorldPosition());
	rb->computeAABB();
	physics.AddBody(rb);
	return rb;
}

std::shared_ptr<RigidBody> StaticBox(PhysicsEngine& physics, const glm::vec3& pos, const glm::vec3& half) {
	auto rb = std::make_shared<RigidBody>();
	rb->SetBodyType(RigidBody::BodyType::STATIC);
	rb->setBox(half);
	rb->setPosition(pos);
	rb->computeAABB();
	physics.AddBody(rb);
	return rb;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// StaticCollision component
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsAuthoring, StaticCollisionCreatesColliderPerChildModelAndRoundTrips)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	physics.SetModelPathResolver([](const Scene&) { return std::string(kLevelPath); });
	TestScene scene;

	auto level = scene.Make("level", glm::vec3(0.0f));
	auto pieceA = scene.Make("pieceA", glm::vec3(0.0f), level);
	auto pieceB = scene.Make("pieceB", glm::vec3(10.0f, 0.0f, 0.0f), level);
	pieceA->SetModel(std::make_shared<Scene>());
	pieceB->SetModel(std::make_shared<Scene>());

	NOX_REQUIRE(Registry::Get().Find("StaticCollision")->add(*level));
	NOX_REQUIRE(physics.StaticCollisions().Has(level.get()));
	physics.Update(kDt);
	NOX_CHECK(physics.StaticCollisions().GeneratedBodyCount() == 2);

	// The generated bodies belong to the children and are not their RigidBody, so nothing is saved for them.
	NOX_CHECK(pieceA->GetRigidBody() == nullptr);
	NOX_CHECK(pieceB->GetRigidBody() == nullptr);
	int meshBodies = 0;
	for (const auto& body : physics.GetBodies()) {
		if (body && body->getShapeType() == RigidBody::ShapeType::MESH && body->getMeshPath() == kLevelPath) ++meshBodies;
	}
	NOX_CHECK(meshBodies == 2);

	// Settings round-trip through the registry; an edit resynchronises (include_children off keeps only the root's own).
	PropValue value;
	NOX_REQUIRE(GetProperty(*level, "StaticCollision", "include_children", value));
	NOX_CHECK(std::get<bool>(value));
	NOX_CHECK(SetProperty(*level, "StaticCollision", "include_children", PropValue(false)));
	physics.Update(kDt);
	NOX_CHECK(physics.StaticCollisions().GeneratedBodyCount() == 0);   // the root has no model of its own
	NOX_CHECK(SetProperty(*level, "StaticCollision", "include_children", PropValue(true)));
	NOX_CHECK(SetProperty(*level, "StaticCollision", "layer", PropValue(int64_t(4))));
	physics.Update(kDt);
	NOX_CHECK(physics.StaticCollisions().GeneratedBodyCount() == 2);
	for (const auto& body : physics.GetBodies()) {
		if (body && body->getShapeType() == RigidBody::ShapeType::MESH) NOX_CHECK(body->getCollisionLayer() == 4u);
	}

	// Removing the component removes its bodies.
	NOX_REQUIRE(Registry::Get().Find("StaticCollision")->remove(*level));
	NOX_CHECK(!physics.StaticCollisions().Has(level.get()));
	physics.Update(kDt);
	NOX_CHECK(physics.StaticCollisions().GeneratedBodyCount() == 0);
}

NOX_TEST(PhysicsAuthoring, StaticCollisionBodiesFollowTheirParent)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	physics.SetModelPathResolver([](const Scene&) { return std::string(kLevelPath); });
	TestScene scene;

	auto level = scene.Make("level", glm::vec3(0.0f));
	auto piece = scene.Make("piece", glm::vec3(1.0f, 0.0f, 0.0f), level);
	piece->SetModel(std::make_shared<Scene>());
	NOX_REQUIRE(Registry::Get().Find("StaticCollision")->add(*level));
	physics.Update(kDt);
	NOX_REQUIRE(physics.StaticCollisions().GeneratedBodyCount() == 1);

	// Move the parent: the child's generated body is placed at the child's new world position.
	level->SetPosition(glm::vec3(3.0f, 2.0f, 0.0f));
	physics.Update(kDt);
	std::shared_ptr<RigidBody> body;
	for (const auto& b : physics.GetBodies()) {
		if (b && b->getShapeType() == RigidBody::ShapeType::MESH) body = b;
	}
	NOX_REQUIRE(body != nullptr);
	NOX_CHECK(glm::distance(body->getPosition(), piece->GetWorldPosition()) < 1e-4f);
	NOX_CHECK(glm::distance(body->getPosition(), glm::vec3(4.0f, 2.0f, 0.0f)) < 1e-4f);
}

NOX_TEST(PhysicsAuthoring, StaticCollisionBakesTheNodeScaleIntoItsGeometry)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	// cube.glb spans -1..1 on each axis: a node scaled (2, 0.5, 2) gives a collider with those half extents.
	physics.SetModelPathResolver([](const Scene&) { return std::string("models/cube.glb"); });
	TestScene scene;
	auto level = scene.Make("level", glm::vec3(0.0f));
	auto piece = scene.Make("piece", glm::vec3(0.0f, 1.0f, 0.0f), level);
	piece->SetScale(glm::vec3(2.0f, 0.5f, 2.0f));
	piece->SetModel(std::make_shared<Scene>());
	NOX_REQUIRE(Registry::Get().Find("StaticCollision")->add(*level));
	physics.Update(kDt);
	NOX_REQUIRE(physics.StaticCollisions().GeneratedBodyCount() == 1);
	std::shared_ptr<RigidBody> body;
	for (const auto& b : physics.GetBodies()) {
		if (b && b->getShapeType() == RigidBody::ShapeType::MESH) body = b;
	}
	NOX_REQUIRE(body != nullptr);
	glm::vec3 mn, mx;
	NOX_REQUIRE(MeshCollider::LocalBounds(*body->getMesh(), mn, mx));
	NOX_CHECK(glm::distance(mx, glm::vec3(2.0f, 0.5f, 2.0f)) < 1e-2f);
	NOX_CHECK(glm::distance(mn, glm::vec3(-2.0f, -0.5f, -2.0f)) < 1e-2f);

	// A scale change rebuilds the body with the new geometry.
	piece->SetScale(glm::vec3(1.0f, 1.0f, 1.0f));
	physics.StaticCollisions().MarkDirty();   // a plain transform edit is picked up within kResyncSeconds
	physics.Update(kDt);
	NOX_REQUIRE(physics.StaticCollisions().GeneratedBodyCount() == 1);
	body.reset();
	for (const auto& b : physics.GetBodies()) {
		if (b && b->getShapeType() == RigidBody::ShapeType::MESH) body = b;
	}
	NOX_REQUIRE(body != nullptr);
	NOX_REQUIRE(MeshCollider::LocalBounds(*body->getMesh(), mn, mx));
	NOX_CHECK(glm::distance(mx, glm::vec3(1.0f)) < 1e-2f);
}

// ---------------------------------------------------------------------------------------------------------------
// Box contacts and sweeps against meshes
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsAuthoring, BoxLandsOnMeshFloorAndRestsWithOneContactBegin)
{
	PhysicsEngine physics;
	TestScene scene;
	auto floorNode = scene.Make("floor", glm::vec3(0.0f));
	auto floor = MeshFloorOn(physics, floorNode, 20.0f);
	floor->setRestitution(0.0f);

	auto box = std::make_shared<RigidBody>();
	box->SetBodyType(RigidBody::BodyType::DYNAMIC);
	box->setBox(glm::vec3(0.5f));
	box->setPosition(glm::vec3(0.0f, 3.0f, 0.0f));
	box->setMass(1.0f);
	box->setRestitution(0.0f);
	box->computeInertiaTensor();
	box->computeAABB();
	physics.AddBody(box);

	std::vector<PhysicsContactEvent> events;
	for (int frame = 0; frame < 150; ++frame) {
		physics.Update(kDt);
		for (const PhysicsContactEvent& e : physics.TakeContactEvents()) events.push_back(e);
	}
	NOX_REQUIRE(events.size() == 1);
	NOX_CHECK(events[0].type == PhysicsContactEvent::Type::BEGIN);
	NOX_CHECK(std::fabs(box->getPosition().y - 0.5f) < 0.02f);
	for (int frame = 0; frame < 120; ++frame) {
		physics.Update(kDt);
		NOX_CHECK(physics.TakeContactEvents().empty());
	}
	NOX_CHECK(std::fabs(box->getPosition().y - 0.5f) < 0.02f);
}

NOX_TEST(PhysicsAuthoring, BoxSweepStopsAtMeshFloor)
{
	PhysicsEngine physics;
	TestScene scene;
	auto floorNode = scene.Make("floor", glm::vec3(0.0f));
	MeshFloorOn(physics, floorNode, 20.0f);

	ShapeSweep::Probe probe;
	probe.kind = ShapeSweep::Kind::BOX;
	probe.center = glm::vec3(0.0f, 5.0f, 0.0f);
	probe.halfExtents = glm::vec3(0.5f);
	ShapeSweep::Hit hit;
	NOX_REQUIRE(ShapeSweep::Sweep(physics, probe, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, 0xFFFFFFFFu, hit));
	NOX_CHECK(std::fabs(hit.distance - 4.5f) < 1e-2f);
	NOX_CHECK(hit.normal.y > 0.99f);
	NOX_CHECK(hit.body != nullptr);

	// Sideways into a mesh wall: the box face meets the wall after its half width.
	auto wallNode = scene.Make("wall", glm::vec3(0.0f));
	std::vector<glm::vec3> pos = { glm::vec3(4.0f, -5.0f, -5.0f), glm::vec3(4.0f, 5.0f, -5.0f), glm::vec3(4.0f, 5.0f, 5.0f),
		glm::vec3(4.0f, -5.0f, 5.0f) };
	auto world = std::make_shared<CollisionWorld>();
	world->AddMesh("wall", pos, { 0, 1, 2, 0, 2, 3 });
	glm::vec3 mn, mx;
	MeshCollider::LocalBounds(*world, mn, mx);
	auto wall = std::make_shared<RigidBody>();
	wall->setMesh(world, mn, mx, "");
	wall->AttachNode(wallNode);
	wall->computeAABB();
	physics.AddBody(wall);
	probe.center = glm::vec3(0.0f, 1.0f, 0.0f);
	NOX_REQUIRE(ShapeSweep::Sweep(physics, probe, glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, 0xFFFFFFFFu, hit));
	NOX_CHECK(std::fabs(hit.distance - 3.5f) < 1e-2f);
	NOX_CHECK(hit.normal.x < -0.99f);
}

// ---------------------------------------------------------------------------------------------------------------
// Characters: platforms, triggers, hits
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsAuthoring, CharacterStandingOnMovingPlatformIsCarriedWithIt)
{
	PhysicsEngine physics;
	TestScene scene;
	auto platformNode = scene.Make("platform", glm::vec3(0.0f));
	auto platform = MeshFloorOn(physics, platformNode, 20.0f);
	auto heroNode = scene.Make("hero", glm::vec3(0.0f));
	NOX_REQUIRE(physics.Characters().Add(heroNode));

	// Settle on the still platform first.
	for (int frame = 0; frame < 30; ++frame) physics.Update(kDt);
	NOX_REQUIRE(physics.Characters().Find(heroNode.get())->onFloor);
	const float startX = heroNode->GetWorldPosition().x;

	// The platform slides 2 m/s along +x; the character does not move itself.
	const float speed = 2.0f;
	float platformX = 0.0f;
	for (int frame = 0; frame < 60; ++frame) {
		platformX += speed * kDt;
		platformNode->SetPosition(glm::vec3(platformX, 0.0f, 0.0f));
		physics.Update(kDt);
	}
	NOX_CHECK(std::fabs(platform->getPlatformVelocity().x - speed) < 0.05f);
	const float carried = heroNode->GetWorldPosition().x - startX;
	NOX_CHECK(std::fabs(carried - platformX) < 0.1f);
	NOX_CHECK(physics.Characters().Find(heroNode.get())->onFloor);

	// Stopping the platform stops the carry.
	for (int frame = 0; frame < 10; ++frame) physics.Update(kDt);
	const float stopped = heroNode->GetWorldPosition().x;
	for (int frame = 0; frame < 10; ++frame) physics.Update(kDt);
	NOX_CHECK(std::fabs(heroNode->GetWorldPosition().x - stopped) < 1e-3f);
}

NOX_TEST(PhysicsAuthoring, CharacterEnteringTriggerFiresOneEnterAndOneExit)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	physics.SetGravity(glm::vec3(0.0f));
	TestScene scene;
	auto zone = scene.Make("zone", glm::vec3(0.0f));
	NOX_REQUIRE(physics.Triggers().Add(zone));
	physics.Triggers().Find(zone.get())->radius = 1.0f;

	auto hero = scene.Make("hero", glm::vec3(-5.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	physics.Characters().Find(hero.get())->gravity = false;

	std::vector<TriggerEvent> events;
	for (int frame = 0; frame < 120; ++frame) {
		physics.Characters().Move(physics, *hero, glm::vec3(0.1f, 0.0f, 0.0f));
		physics.Update(kDt);
		for (const TriggerEvent& e : physics.Triggers().TakeEvents()) events.push_back(e);
	}
	NOX_REQUIRE(events.size() == 2);
	NOX_CHECK(events[0].enter);
	NOX_CHECK(!events[1].enter);
	NOX_CHECK(events[0].trigger == zone->GetEntityID());
	NOX_CHECK(events[0].other == hero->GetEntityID());
	NOX_CHECK(events[1].other == hero->GetEntityID());
}

NOX_TEST(PhysicsAuthoring, CharacterMoveReportsTheWallItTouches)
{
	PhysicsEngine physics;
	TestScene scene;
	auto wallNode = scene.Make("wall", glm::vec3(0.0f));
	auto wall = StaticBox(physics, glm::vec3(2.0f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 5.0f));
	wall->AttachNode(wallNode);
	auto hero = scene.Make("hero", glm::vec3(0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	physics.Characters().Find(hero.get())->gravity = false;

	NOX_CHECK(physics.Characters().Move(physics, *hero, glm::vec3(1.5f, 0.0f, 0.0f)));
	const std::vector<ControllerHit> hits = physics.Characters().TakeHits();
	NOX_REQUIRE(!hits.empty());
	NOX_CHECK(hits[0].body == wall);
	NOX_CHECK(hits[0].controller == hero->GetEntityID());
	NOX_CHECK(hits[0].normal.x < -0.99f);
	// The character stops at the wall face (x = 1.5) less its radius.
	NOX_CHECK(std::fabs(hero->GetWorldPosition().x - 1.1f) < 0.02f);
	// Nothing new while it stays against the wall without moving into it.
	NOX_CHECK(physics.Characters().TakeHits().empty());
}
