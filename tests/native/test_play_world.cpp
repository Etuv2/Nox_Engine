// Play-world restoration (Stop): transforms, rigid-body states, runtime-created entities and their bodies.
#include "TestFramework.h"

#include "ComponentManager.h"
#include "PhysicsEngine.h"
#include "PlayWorldSnapshot.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "TransformSystem.h"

#include <memory>

namespace {
	using BodyID = PhysicsEngine::BodyID;
	// Adds a dynamic body to `entity` the way the scene loader does (AddBody + PhysicsComponent).
	BodyID AttachBody(SceneGraph& scene, PhysicsEngine& physics, EntityID entity, const glm::vec3& position, const glm::vec3& velocity) {
		auto body = std::make_shared<RigidBody>();
		body->setPosition(position);
		body->setVelocity(velocity);
		const BodyID id = physics.AddBody(body);
		PhysicsComponent component;
		component.bodyID = id;
		component.rigidBody = body;
		scene.GetComponentManager()->AddPhysics(entity, component);
		return id;
	}
}

NOX_TEST(PlayWorld, StopRestoresTransformsBodiesAndRemovesRuntimeEntities)
{
	SceneGraph scene;
	PhysicsEngine physics;
	ComponentManager* cm = scene.GetComponentManager();
	TransformSystem* ts = scene.GetTransformSystem();

	const EntityID crate = cm->CreateEntity("crate");
	ts->SetPosition(crate, glm::vec3(1.0f, 0.0f, 0.0f));
	AttachBody(scene, physics, crate, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	const size_t bodiesBefore = physics.GetBodies().size();

	SceneLoader loader(nullptr, nullptr, 1280, 720);
	PlayWorldSnapshot snapshot;
	snapshot.Capture(scene, loader);
	NOX_REQUIRE(snapshot.Captured());
	NOX_CHECK(snapshot.EntityCount() >= 2);   // the scene root and "crate"

	// Play: modify the authored entity, spawn and attach a body to a runtime entity, then destroy it again.
	ts->SetPosition(crate, glm::vec3(9.0f, 9.0f, 9.0f));
	cm->GetPhysics(crate)->rigidBody->setPosition(glm::vec3(3.0f, 3.0f, 3.0f));
	cm->GetPhysics(crate)->rigidBody->setVelocity(glm::vec3(7.0f, 0.0f, 0.0f));
	const EntityID spawned = cm->CreateEntity("runtime-prop");
	AttachBody(scene, physics, spawned, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f));
	const EntityID transient = cm->CreateEntity("transient");
	NOX_CHECK(cm->IsHandleValid(cm->MakeHandle(transient)));
	cm->DestroyEntity(transient);
	NOX_CHECK_EQ(physics.GetBodies().size(), bodiesBefore + 1);

	const PlayWorldSnapshot::Report report = snapshot.Restore(scene, &physics, loader, "");
	NOX_CHECK_EQ(report.destroyedEntities, size_t(1));   // runtime-prop
	NOX_CHECK_EQ(report.removedBodies, size_t(1));
	NOX_CHECK_EQ(report.restoredTransforms, snapshot.EntityCount());
	NOX_CHECK_EQ(report.restoredBodies, size_t(1));

	NOX_CHECK(cm->FindEntityByName("runtime-prop") == INVALID_ENTITY);
	NOX_CHECK_EQ(physics.GetBodies().size(), bodiesBefore);
	NOX_CHECK(ts->GetWorldPosition(crate).x == 1.0f);
	NOX_CHECK(ts->GetWorldPosition(crate).y == 0.0f);
	const RigidBody& body = *cm->GetPhysics(crate)->rigidBody;
	NOX_CHECK(body.getPosition() == glm::vec3(0.0f, 5.0f, 0.0f));
	NOX_CHECK(body.getVelocity() == glm::vec3(1.0f, 0.0f, 0.0f));
	NOX_CHECK(body.getPreviousPosition() == glm::vec3(0.0f, 5.0f, 0.0f));
}

NOX_TEST(PlayWorldSnapshot, SceneNodesCreatedDuringPlayAreRemoved)
{
	SceneGraph scene;
	PhysicsEngine physics;
	ComponentManager* cm = scene.GetComponentManager();
	TransformSystem* ts = scene.GetTransformSystem();
	SceneLoader loader(nullptr, nullptr, 1280, 720);

	PlayWorldSnapshot snapshot;
	snapshot.Capture(scene, loader);

	// A node added to the graph during play (instantiated or created by a script) leaves at Stop, with its entity.
	const EntityID nodeEntity = cm->CreateEntity("scene-node");
	auto node = std::make_shared<SceneNode>(cm, ts, nodeEntity);
	scene.GetRoot()->children.push_back(node);
	const EntityID strayEntity = cm->CreateEntity("stray");

	const PlayWorldSnapshot::Report report = snapshot.Restore(scene, &physics, loader, "");
	NOX_CHECK_EQ(report.removedNodes, size_t(1));
	NOX_CHECK_EQ(report.destroyedEntities, size_t(2));   // the node's entity and the stray entity
	NOX_CHECK(!cm->IsHandleValid(cm->MakeHandle(nodeEntity)));
	NOX_CHECK(!cm->IsHandleValid(cm->MakeHandle(strayEntity)));
	NOX_CHECK(scene.GetRoot()->children.empty());
}

NOX_TEST(PlayWorldSnapshot, AuthoredNodesDestroyedDuringPlayAreRecreated)
{
	SceneGraph scene;
	PhysicsEngine physics;
	ComponentManager* cm = scene.GetComponentManager();
	SceneLoader loader(nullptr, nullptr, 1280, 720);

	// Authored: a parent with one child, both with a Light-free node JSON, built through the loader.
	const nlohmann::json authored = nlohmann::json::parse(R"({"name":"Parent","position":[1,2,3],"children":[
		{"name":"Child","position":[4,0,0]}]})");
	auto parent = loader.InstantiateNode(scene, authored, scene.GetRoot(), "scenes/_test.json");
	NOX_REQUIRE(parent != nullptr);
	NOX_REQUIRE(parent->children.size() == 1);

	PlayWorldSnapshot snapshot;
	snapshot.Capture(scene, loader);
	NOX_CHECK(snapshot.NodeCount() == 2);

	// Play: destroy the parent (its subtree goes with it).
	NOX_CHECK(PlayWorldSnapshot::RemoveSubtree(scene, parent, &physics));
	NOX_CHECK(scene.GetRoot()->children.empty());

	const PlayWorldSnapshot::Report report = snapshot.Restore(scene, &physics, loader, "scenes/_test.json");
	NOX_CHECK_EQ(report.recreatedNodes, size_t(1));   // the child is covered by the parent's JSON
	NOX_REQUIRE(scene.GetRoot()->children.size() == 1);
	auto restored = scene.GetRoot()->children[0];
	NOX_CHECK(restored->GetName() == "Parent");
	NOX_REQUIRE(restored->children.size() == 1);
	NOX_CHECK(restored->children[0]->GetName() == "Child");
	NOX_CHECK(restored->GetPosition() == glm::vec3(1.0f, 2.0f, 3.0f));
}

NOX_TEST(PlayWorldSnapshot, RestoreWithoutCaptureDoesNothing)
{
	SceneGraph scene;
	PlayWorldSnapshot snapshot;
	const EntityID id = scene.GetComponentManager()->CreateEntity("x");
	SceneLoader loader(nullptr, nullptr, 1280, 720);
	const PlayWorldSnapshot::Report report = snapshot.Restore(scene, nullptr, loader, "");
	NOX_CHECK_EQ(report.destroyedEntities, size_t(0));
	NOX_CHECK(scene.GetComponentManager()->IsHandleValid(scene.GetComponentManager()->MakeHandle(id)));
}
