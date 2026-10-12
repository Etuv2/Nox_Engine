#include "PlayWorldSnapshot.h"

#include "ComponentManager.h"
#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "TransformSystem.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace {
	// Entities referenced by the scene graph (any node with an ECS entity).
	std::set<EntityID> SceneOwnedEntities(SceneGraph& scene) {
		std::set<EntityID> owned;
		std::function<void(const std::shared_ptr<SceneNode>&)> visit = [&](const std::shared_ptr<SceneNode>& node) {
			if (!node) return;
			if (node->GetEntityID() != INVALID_ENTITY) owned.insert(node->GetEntityID());
			for (const auto& child : node->children) visit(child);
		};
		if (auto root = scene.GetRoot()) visit(root);
		return owned;
	}
}

void PlayWorldSnapshot::Capture(SceneGraph& scene) {
	m_entities.clear();
	m_known.clear();
	ComponentManager* cm = scene.GetComponentManager();
	if (!cm) return;
	std::vector<EntityID> ids;
	for (const auto& entry : cm->GetTransformPool()) ids.push_back(entry.entity);
	std::sort(ids.begin(), ids.end());
	for (EntityID id : ids) {
		const EntityHandle handle = cm->MakeHandle(id);
		if (!cm->IsHandleValid(handle)) continue;
		m_known.insert(handle);
		EntityState state;
		state.handle = handle;
		if (const TransformComponent* transform = cm->GetTransform(id)) state.localTransform = transform->localTransform;
		if (const PhysicsComponent* physics = cm->GetPhysics(id)) {
			if (physics->rigidBody) {
				state.hasBody = true;
				state.body.position = physics->rigidBody->getPosition();
				state.body.orientation = physics->rigidBody->getOrientation();
				state.body.linearVelocity = physics->rigidBody->getVelocity();
				state.body.angularVelocity = physics->rigidBody->getAngularVelocity();
			}
		}
		m_entities.push_back(state);
	}
	m_captured = true;
}

PlayWorldSnapshot::Report PlayWorldSnapshot::Restore(SceneGraph& scene, PhysicsEngine* physics) {
	Report report;
	if (!m_captured) return report;
	ComponentManager* cm = scene.GetComponentManager();
	TransformSystem* ts = scene.GetTransformSystem();
	if (!cm || !ts) return report;

	// 1. Entities created during play: remove them with their bodies. Scene-owned ones stay (the scene graph
	//    still refers to them); they are not part of the authored snapshot and keep their current state.
	const std::set<EntityID> owned = SceneOwnedEntities(scene);
	std::vector<EntityID> ids;
	for (const auto& entry : cm->GetTransformPool()) ids.push_back(entry.entity);
	std::sort(ids.begin(), ids.end());
	for (EntityID id : ids) {
		const EntityHandle handle = cm->MakeHandle(id);
		if (!cm->IsHandleValid(handle) || m_known.count(handle)) continue;
		if (owned.count(id)) {
			++report.keptSceneEntities;
			continue;
		}
		if (const PhysicsComponent* physics_ = cm->GetPhysics(id)) {
			if (physics && physics_->bodyID != 0) {
				physics->RemoveBody(physics_->bodyID);
				++report.removedBodies;
			}
		}
		cm->DestroyEntity(id);
		++report.destroyedEntities;
	}

	// 2. Authored entities: transforms and body states back to the captured values.
	for (const EntityState& state : m_entities) {
		if (!cm->IsHandleValid(state.handle)) continue;   // destroyed by something other than play
		ts->SetLocalTransform(state.handle.id, state.localTransform);
		++report.restoredTransforms;
		if (!state.hasBody) continue;
		PhysicsComponent* physics_ = cm->GetPhysics(state.handle.id);
		if (!physics_ || !physics_->rigidBody) continue;
		RigidBody& body = *physics_->rigidBody;
		body.setPosition(state.body.position);
		body.setOrientation(state.body.orientation);
		body.setVelocity(state.body.linearVelocity);
		body.setAngularVelocity(state.body.angularVelocity);
		body.storePreviousState();   // no interpolation from the play-time pose
		body.computeAABB();
		++report.restoredBodies;
	}
	return report;
}
