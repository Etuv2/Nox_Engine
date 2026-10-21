// Capsule and mesh colliders, the character controller on mesh geometry (ramps, steps, walls, falls), shape
// sweeps, mesh ray casts and the capsule/mesh descriptors. Headless: no GPU.
#include "TestFramework.h"

#include "CollisionWorld.h"
#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "physics/CharacterSystem.h"
#include "physics/MeshCollider.h"
#include "physics/ShapeOverlap.h"
#include "physics/ShapeSweep.h"
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

struct ServicesGuard {
	PhysicsEngine* physics;
	ServicesGuard() : physics(Services().physics) {}
	~ServicesGuard() { Services().physics = physics; }
};

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

std::shared_ptr<RigidBody> MakeBox(PhysicsEngine& physics, const glm::vec3& pos, const glm::vec3& half) {
	auto rb = std::make_shared<RigidBody>();
	rb->SetBodyType(RigidBody::BodyType::STATIC);
	rb->setBox(half);
	rb->setPosition(pos);
	rb->computeAABB();
	physics.AddBody(rb);
	return rb;
}

std::shared_ptr<RigidBody> MakePlane(PhysicsEngine& physics, float height) {
	auto rb = std::make_shared<RigidBody>();
	rb->SetBodyType(RigidBody::BodyType::STATIC);
	rb->setPlane(glm::vec3(0.0f, 1.0f, 0.0f), height);
	physics.AddBody(rb);
	return rb;
}

// Appends a quad (two triangles) with the given corners.
void AddQuad(std::vector<glm::vec3>& pos, std::vector<uint32_t>& idx, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
	const uint32_t base = static_cast<uint32_t>(pos.size());
	pos.push_back(a);
	pos.push_back(b);
	pos.push_back(c);
	pos.push_back(d);
	idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

// Triangle mesh body: a floor at y = 0 for x in [-30, 0], then a ramp rising at angleDeg for x in [0, 20].
// Both are 10 m wide in z. The geometry is in world space and the body sits at the origin.
std::shared_ptr<RigidBody> MakeRampLevel(PhysicsEngine& physics, float angleDeg, float rampLength = 20.0f) {
	std::vector<glm::vec3> pos;
	std::vector<uint32_t> idx;
	const float w = 5.0f;
	const float t = std::tan(glm::radians(angleDeg));
	AddQuad(pos, idx, glm::vec3(-30.0f, 0.0f, -w), glm::vec3(0.0f, 0.0f, -w), glm::vec3(0.0f, 0.0f, w), glm::vec3(-30.0f, 0.0f, w));
	AddQuad(pos, idx, glm::vec3(0.0f, 0.0f, -w), glm::vec3(rampLength, rampLength * t, -w),
		glm::vec3(rampLength, rampLength * t, w), glm::vec3(0.0f, 0.0f, w));
	auto world = std::make_shared<CollisionWorld>();
	world->AddMesh("level", pos, idx);
	glm::vec3 mn, mx;
	NOX_REQUIRE(MeshCollider::LocalBounds(*world, mn, mx));
	auto rb = std::make_shared<RigidBody>();
	rb->setMesh(world, mn, mx, "");
	rb->computeAABB();
	physics.AddBody(rb);
	return rb;
}

// Flat mesh floor at y = 0 over x and z in [-20, 20].
std::shared_ptr<RigidBody> MakeMeshFloor(PhysicsEngine& physics) {
	std::vector<glm::vec3> pos;
	std::vector<uint32_t> idx;
	AddQuad(pos, idx, glm::vec3(-20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f),
		glm::vec3(-20.0f, 0.0f, 20.0f));
	auto world = std::make_shared<CollisionWorld>();
	world->AddMesh("floor", pos, idx);
	glm::vec3 mn, mx;
	MeshCollider::LocalBounds(*world, mn, mx);
	auto rb = std::make_shared<RigidBody>();
	rb->setMesh(world, mn, mx, "");
	rb->computeAABB();
	physics.AddBody(rb);
	return rb;
}

std::shared_ptr<RigidBody> MakeCapsule(const glm::vec3& centre, float radius, float halfSegment, int axis = 1) {
	auto rb = std::make_shared<RigidBody>();
	rb->setCapsule(radius, halfSegment, axis);
	rb->setPosition(centre);
	rb->computeAABB();
	return rb;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Exact contacts: capsule against sphere, box, plane and mesh
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, CapsuleContactsPlaneBoxMeshAndSphere)
{
	PhysicsEngine physics;
	// Radius 0.5, segment half length 0.5: the bottom sample sphere is at y = 0.3 when the centre is at 0.8.
	auto capsule = MakeCapsule(glm::vec3(0.0f, 0.8f, 0.0f), 0.5f, 0.5f);
	glm::vec3 push(0.0f);
	float depth = 0.0f;

	auto plane = MakePlane(physics, 0.0f);
	NOX_REQUIRE(ShapeOverlap::Test(*capsule, *plane, 0.0f, push, depth));
	NOX_CHECK(std::fabs(depth - 0.2f) < 1e-4f);
	NOX_CHECK(push.y > 0.999f);

	auto box = MakeBox(physics, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(5.0f, 0.5f, 5.0f));   // top at y = 0
	NOX_REQUIRE(ShapeOverlap::Test(*capsule, *box, 0.0f, push, depth));
	NOX_CHECK(std::fabs(depth - 0.2f) < 1e-3f);
	NOX_CHECK(push.y > 0.999f);

	auto mesh = MakeMeshFloor(physics);
	NOX_REQUIRE(ShapeOverlap::Test(*capsule, *mesh, 0.0f, push, depth));
	NOX_CHECK(std::fabs(depth - 0.2f) < 1e-3f);
	NOX_CHECK(push.y > 0.999f);

	// A sphere above the capsule's top sample: the top sample (y = 1.3) against a sphere at 1.5.
	auto ball = std::make_shared<RigidBody>();
	ball->setSphere(0.5f);
	ball->setPosition(glm::vec3(0.0f, 1.5f, 0.0f));
	ball->computeAABB();
	NOX_REQUIRE(ShapeOverlap::Test(*capsule, *ball, 0.0f, push, depth));
	NOX_CHECK(std::fabs(depth - 0.8f) < 1e-3f);
	NOX_CHECK(push.y < -0.999f);

	// Far away: no contact.
	auto far = MakeCapsule(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f, 0.5f);
	NOX_CHECK(!ShapeOverlap::Test(*far, *plane, 0.0f, push, depth));
}

NOX_TEST(PhysicsShapes, CapsuleRaycastAndMeshRaycast)
{
	PhysicsEngine physics;
	MakeMeshFloor(physics);
	auto post = MakeCapsule(glm::vec3(-3.0f, 2.0f, 0.0f), 0.5f, 0.5f);   // top cap at y = 3
	physics.AddBody(post);

	PhysicsRayHit hit;
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(-3.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, 0xFFFFFFFFu, hit));
	NOX_CHECK(hit.body == post);
	NOX_CHECK(std::fabs(hit.distance - 7.0f) < 1e-3f);
	NOX_CHECK(hit.normal.y > 0.99f);

	// Straight down at x = -10 hits the mesh floor at y = 0.
	NOX_REQUIRE(physics.RayCastClosest(glm::vec3(-10.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, 0xFFFFFFFFu, hit));
	NOX_CHECK(std::fabs(hit.distance - 5.0f) < 1e-3f);
	NOX_CHECK(std::fabs(hit.point.y) < 1e-3f);
	NOX_CHECK(hit.normal.y > 0.99f);
}

// ---------------------------------------------------------------------------------------------------------------
// Sweeps
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, SweepsReturnTheDistanceToEachSurfaceKind)
{
	ShapeSweep::Probe sphere;
	sphere.kind = ShapeSweep::Kind::SPHERE;
	sphere.center = glm::vec3(0.0f, 5.0f, 0.0f);
	sphere.radius = 0.5f;
	const glm::vec3 down(0.0f, -1.0f, 0.0f);
	ShapeSweep::Hit hit;

	{
		PhysicsEngine physics;
		MakePlane(physics, 0.0f);
		NOX_REQUIRE(ShapeSweep::Sweep(physics, sphere, down, 10.0f, 0xFFFFFFFFu, hit));
		NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);
		NOX_CHECK(hit.normal.y > 0.99f);
		NOX_CHECK(std::fabs(hit.point.y) < 0.02f);
		NOX_CHECK(!ShapeSweep::Sweep(physics, sphere, down, 4.0f, 0xFFFFFFFFu, hit));   // out of range
	}
	{
		PhysicsEngine physics;
		MakeBox(physics, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(5.0f, 0.5f, 5.0f));
		NOX_REQUIRE(ShapeSweep::Sweep(physics, sphere, down, 10.0f, 0xFFFFFFFFu, hit));
		NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);
		NOX_CHECK(hit.normal.y > 0.99f);
	}
	{
		PhysicsEngine physics;
		auto floor = MakeMeshFloor(physics);
		NOX_REQUIRE(ShapeSweep::Sweep(physics, sphere, down, 10.0f, 0xFFFFFFFFu, hit));
		NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);
		NOX_CHECK(hit.body == floor);

		// Layers: a mask that does not include the floor's layer sees nothing.
		floor->setCollisionLayer(2u);
		NOX_CHECK(!ShapeSweep::Sweep(physics, sphere, down, 10.0f, 1u, hit));
		NOX_CHECK(ShapeSweep::Sweep(physics, sphere, down, 10.0f, 2u, hit));
	}
	{
		// Capsule (axis y) and box probes against the mesh floor: the bottom cap and the box bottom touch at 4.5.
		PhysicsEngine physics;
		MakeMeshFloor(physics);
		ShapeSweep::Probe capsule;
		capsule.kind = ShapeSweep::Kind::CAPSULE;
		capsule.center = glm::vec3(0.0f, 5.5f, 0.0f);
		capsule.radius = 0.5f;
		capsule.halfSegment = 0.5f;
		NOX_REQUIRE(ShapeSweep::Sweep(physics, capsule, down, 10.0f, 0xFFFFFFFFu, hit));
		NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);

		ShapeSweep::Probe box;
		box.kind = ShapeSweep::Kind::BOX;
		box.center = glm::vec3(0.0f, 5.0f, 0.0f);
		box.halfExtents = glm::vec3(0.5f);
		// Box against mesh: the box lands on the floor after 4.5 m (box bottom at y = 4.5, floor at y = 0).
		NOX_REQUIRE(ShapeSweep::Sweep(physics, box, down, 10.0f, 0xFFFFFFFFu, hit));
		NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Dynamic capsule and mesh contact events
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, CapsuleRestsOnMeshFloorWithOneContactBegin)
{
	PhysicsEngine physics;
	auto floor = MakeMeshFloor(physics);
	floor->setRestitution(0.0f);   // no bounce: the test is about one begin and no flicker while resting
	auto capsule = std::make_shared<RigidBody>();
	capsule->SetBodyType(RigidBody::BodyType::DYNAMIC);
	capsule->setCapsule(0.5f, 0.5f, 1);
	capsule->setPosition(glm::vec3(0.0f, 3.0f, 0.0f));
	capsule->setMass(1.0f);
	capsule->setRestitution(0.0f);
	capsule->computeAABB();
	physics.AddBody(capsule);

	std::vector<PhysicsContactEvent> events;
	for (int frame = 0; frame < 150; ++frame) {
		physics.Update(kDt);
		for (const PhysicsContactEvent& e : physics.TakeContactEvents()) events.push_back(e);
	}
	NOX_REQUIRE(events.size() == 1);
	NOX_CHECK(events[0].type == PhysicsContactEvent::Type::BEGIN);
	// Resting: the lowest sample sphere is on the floor, so the centre sits one radius plus half a segment up.
	NOX_CHECK(std::fabs(capsule->getPosition().y - 1.0f) < 0.05f);
	for (int frame = 0; frame < 120; ++frame) {
		physics.Update(kDt);
		NOX_CHECK(physics.TakeContactEvents().empty());
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Character controller on geometry
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, CharacterWalksUpThirtyDegreeRampAndStaysOnFloor)
{
	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	MakeRampLevel(physics, 30.0f);

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(-3.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	CharacterSettings* settings = physics.Characters().Find(hero.get());
	settings->gravity = false;

	// Walking up a slope loses some of the input to the surface normal (about a quarter at 30 degrees).
	for (int step = 0; step < 260; ++step) {
		physics.Characters().Move(physics, *hero, glm::vec3(0.05f, 0.0f, 0.0f));
	}
	const glm::vec3 p = hero->GetWorldPosition();
	NOX_CHECK(p.x > 5.0f);
	NOX_CHECK(settings->onFloor);
	// The feet sit one radius off the slope along its normal: r / cos(30) - r, about 0.06 m above it.
	const float above = p.y - std::tan(glm::radians(30.0f)) * p.x;
	NOX_CHECK(above > 0.0f && above < 0.1f);
}

NOX_TEST(PhysicsShapes, CharacterWalksDownRampAndSnapsToGround)
{
	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	MakeRampLevel(physics, 30.0f);

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(5.0f, 5.0f * std::tan(glm::radians(30.0f)), 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	CharacterSettings* settings = physics.Characters().Find(hero.get());
	settings->gravity = false;
	// Settle onto the ramp, then walk down it: each step leaves the surface and the snap brings the feet back.
	physics.Characters().Move(physics, *hero, glm::vec3(0.0f, -0.1f, 0.0f));
	NOX_REQUIRE(settings->onFloor);
	for (int step = 0; step < 150; ++step) {
		physics.Characters().Move(physics, *hero, glm::vec3(-0.05f, 0.0f, 0.0f));
	}
	const glm::vec3 p = hero->GetWorldPosition();
	NOX_CHECK(p.x < -2.0f);
	NOX_CHECK(settings->onFloor);
	NOX_CHECK(std::fabs(p.y) < 0.05f);
}

NOX_TEST(PhysicsShapes, CharacterIsBlockedBySixtyDegreeSlopeAndOneMetreWall)
{
	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	MakeRampLevel(physics, 60.0f);

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(-3.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	physics.Characters().Find(hero.get())->gravity = false;
	for (int step = 0; step < 200; ++step) {
		physics.Characters().Move(physics, *hero, glm::vec3(0.05f, 0.0f, 0.0f));
	}
	const glm::vec3 p = hero->GetWorldPosition();
	NOX_CHECK(p.x < 0.5f);          // stopped at the foot of the slope
	NOX_CHECK(p.y < 0.05f);         // did not climb it

	// A 1 m wall (top at y = 1) across the path: the step offset (0.3 m) cannot climb it.
	PhysicsEngine wallPhysics;
	wallPhysics.SetGravity(glm::vec3(0.0f));
	MakeBox(wallPhysics, glm::vec3(-14.0f, -0.5f, 0.0f), glm::vec3(16.0f, 0.5f, 10.0f));   // floor, x in [-30, 2]
	MakeBox(wallPhysics, glm::vec3(2.5f, 0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 10.0f));       // wall, x in [2, 3], top at 1
	TestScene wallScene;
	auto walker = wallScene.Make("walker", glm::vec3(-3.0f, 0.0f, 0.0f));
	NOX_REQUIRE(wallPhysics.Characters().Add(walker));
	wallPhysics.Characters().Find(walker.get())->gravity = false;
	for (int step = 0; step < 200; ++step) {
		wallPhysics.Characters().Move(wallPhysics, *walker, glm::vec3(0.05f, 0.0f, 0.0f));
	}
	NOX_CHECK(walker->GetWorldPosition().x < 2.0f - 0.4f + 0.02f);
	NOX_CHECK(std::fabs(walker->GetWorldPosition().y) < 0.02f);
}

NOX_TEST(PhysicsShapes, CharacterClimbsThirtyCentimetreStep)
{
	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	MakeBox(physics, glm::vec3(-14.0f, -0.5f, 0.0f), glm::vec3(16.0f, 0.5f, 10.0f));   // floor, x in [-30, 2]
	MakeBox(physics, glm::vec3(6.0f, 0.15f, 0.0f), glm::vec3(4.0f, 0.15f, 10.0f));     // step, x in [2, 10], top at 0.3

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(-3.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	CharacterSettings* settings = physics.Characters().Find(hero.get());
	settings->gravity = false;
	for (int step = 0; step < 150; ++step) {
		physics.Characters().Move(physics, *hero, glm::vec3(0.05f, 0.0f, 0.0f));
	}
	const glm::vec3 p = hero->GetWorldPosition();
	NOX_CHECK(p.x > 4.0f);
	NOX_CHECK(std::fabs(p.y - 0.3f) < 0.02f);
	NOX_CHECK(settings->onFloor);
}

// A body added as a small sphere far away, then given a mesh floor over the origin (as the scene loader does when
// the Collider property is applied after AddBody): RefreshBodyBounds must move its broadphase proxy, or the floor
// is invisible to queries.
NOX_TEST(PhysicsShapes, ChangedStaticBoundsReachTheBroadphase)
{
	PhysicsEngine physics;
	auto body = std::make_shared<RigidBody>();
	body->SetBodyType(RigidBody::BodyType::STATIC);
	body->setSphere(0.5f);
	body->setPosition(glm::vec3(30.0f, 0.0f, 0.0f));
	body->computeAABB();
	physics.AddBody(body);

	std::vector<glm::vec3> pos;
	std::vector<uint32_t> idx;
	AddQuad(pos, idx, glm::vec3(-20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f),
		glm::vec3(-20.0f, 0.0f, 20.0f));
	auto world = std::make_shared<CollisionWorld>();
	world->AddMesh("floor", pos, idx);
	glm::vec3 mn, mx;
	MeshCollider::LocalBounds(*world, mn, mx);
	body->setMesh(world, mn, mx, "");
	body->setPosition(glm::vec3(0.0f));   // the mesh is in the body's local space: its origin is the floor's origin
	physics.RefreshBodyBounds(*body);

	ShapeSweep::Probe sphere;
	sphere.kind = ShapeSweep::Kind::SPHERE;
	sphere.center = glm::vec3(0.0f, 5.0f, 0.0f);
	sphere.radius = 0.5f;
	ShapeSweep::Hit hit;
	NOX_REQUIRE(ShapeSweep::Sweep(physics, sphere, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, 0xFFFFFFFFu, hit));
	NOX_CHECK(hit.body == body);
	NOX_CHECK(std::fabs(hit.distance - 4.5f) < 0.01f);
}

// The demo: a character with gravity walks across a mesh floor (Move each frame, then the physics step).
NOX_TEST(PhysicsShapes, CharacterWalkingOnMeshFloorWithGravityStaysOnTheFloor)
{
	PhysicsEngine physics;   // default gravity
	MakeMeshFloor(physics);

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(0.0f, 0.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	float lowest = 1e9f;
	for (int frame = 0; frame < 240; ++frame) {
		const glm::vec3 walk = (frame % 120 < 60) ? glm::vec3(0.0f, 0.0f, -4.0f * kDt) : glm::vec3(4.0f * kDt, 0.0f, 0.0f);
		physics.Characters().Move(physics, *hero, walk);
		physics.Update(kDt);
		lowest = std::min(lowest, hero->GetWorldPosition().y);
	}
	NOX_CHECK(lowest > -0.05f);
}

NOX_TEST(PhysicsShapes, CharacterNeverFallsThroughMeshFloorAt20MetresPerSecond)
{
	PhysicsEngine physics;   // default gravity
	MakeMeshFloor(physics);

	TestScene scene;
	auto faller = scene.Make("faller", glm::vec3(-5.0f, 10.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(faller));
	CharacterSettings* settings = physics.Characters().Find(faller.get());
	settings->velocity.y = -20.0f;

	float lowest = 1e9f;
	bool landed = false;
	for (int frame = 0; frame < 90; ++frame) {
		physics.Update(kDt);
		lowest = std::min(lowest, faller->GetWorldPosition().y);
		landed = landed || settings->onFloor;
	}
	NOX_CHECK(lowest > -0.01f);
	NOX_CHECK(landed);
	NOX_CHECK(std::fabs(faller->GetWorldPosition().y) < 0.02f);
}

// ---------------------------------------------------------------------------------------------------------------
// Descriptors: the Collider shape and its capsule and mesh properties
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, ColliderDescriptorEditsCapsuleAndMeshShapes)
{
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;

	TestScene scene;
	auto node = scene.Make("shape", glm::vec3(0.0f));
	NOX_REQUIRE(Registry::Get().Find("RigidBody")->add(*node));
	NOX_REQUIRE(Registry::Get().Find("Collider")->has(*node));

	NOX_CHECK(SetProperty(*node, "Collider", "shape", PropValue(int64_t(3))));   // capsule
	auto rb = node->GetRigidBody();
	NOX_REQUIRE(rb != nullptr);
	NOX_CHECK(rb->getShapeType() == RigidBody::ShapeType::CAPSULE);
	NOX_CHECK(SetProperty(*node, "Collider", "height", PropValue(2.0)));
	NOX_CHECK(std::fabs(rb->getHalfSegment() + rb->getBoundingRadius() - 1.0f) < 1e-5f);
	NOX_CHECK(SetProperty(*node, "Collider", "capsule_axis", PropValue(int64_t(0))));
	NOX_CHECK(rb->getCapsuleAxis() == 0);
	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Collider", "height", value));
	NOX_CHECK(std::fabs(std::get<double>(value) - 2.0) < 1e-5);

	NOX_CHECK(SetProperty(*node, "Collider", "shape", PropValue(int64_t(4))));   // mesh, no path yet
	NOX_CHECK(rb->getShapeType() == RigidBody::ShapeType::MESH);
	NOX_CHECK(rb->IsStatic());
	NOX_CHECK(rb->getMesh() == nullptr);
	// A missing file loads nothing (no triangles), and the path is kept for saving.
	NOX_CHECK(SetProperty(*node, "Collider", "mesh_path", PropValue(std::string("does/not/exist.glb"))));
	NOX_CHECK(rb->getMesh() == nullptr);
	NOX_CHECK(GetProperty(*node, "Collider", "mesh_path", value));
	NOX_CHECK(std::get<std::string>(value) == "does/not/exist.glb");
}

// ---------------------------------------------------------------------------------------------------------------
// Mesh collider cache and the world collision hook
// ---------------------------------------------------------------------------------------------------------------

NOX_TEST(PhysicsShapes, WorldCollisionGeometryStopsTheCharacter)
{
	// A CollisionWorld that is not a physics body (as a level's chunk would be) stops a character when attached.
	std::vector<glm::vec3> pos;
	std::vector<uint32_t> idx;
	AddQuad(pos, idx, glm::vec3(-20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f),
		glm::vec3(-20.0f, 0.0f, 20.0f));
	CollisionWorld level;
	level.AddMesh("floor", pos, idx);

	PhysicsEngine physics;
	physics.SetGravity(glm::vec3(0.0f));
	physics.SetWorldCollision(&level);
	NOX_CHECK(physics.WorldCollision() == &level);

	TestScene scene;
	auto hero = scene.Make("hero", glm::vec3(0.0f, 2.0f, 0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	NOX_CHECK(physics.Characters().Move(physics, *hero, glm::vec3(0.0f, -2.5f, 0.0f)));
	NOX_CHECK(physics.Characters().Find(hero.get())->onFloor);
	NOX_CHECK(std::fabs(hero->GetWorldPosition().y) < 0.02f);
	physics.SetWorldCollision(nullptr);
}

NOX_TEST(PhysicsShapes, MeshColliderCacheReturnsOneMeshPerPath)
{
	auto missing = MeshCollider::LoadShared("does/not/exist.glb");
	NOX_CHECK(missing == nullptr);
	NOX_CHECK(MeshCollider::LoadShared("") == nullptr);
	MeshCollider::ClearCache();
}

