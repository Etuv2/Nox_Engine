#include "SceneNode.h"
#include "Scene.h"
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "RigidBody.h"
#include "AudioNode.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GL/glew.h>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <cassert>

namespace {
glm::mat4 ComputeImportedNodeWorldTransform(const Scene& model, int nodeIndex, std::unordered_map<int, glm::mat4>& cache)
{
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
		return glm::mat4(1.0f);
	}

	auto it = cache.find(nodeIndex);
	if (it != cache.end()) {
		return it->second;
	}

	const Scene::NodeInfo& nodeInfo = model.nodes[nodeIndex];
	glm::mat4 worldTransform = nodeInfo.localTransform;
	if (nodeInfo.parent >= 0) {
		worldTransform = ComputeImportedNodeWorldTransform(model, nodeInfo.parent, cache) * worldTransform;
	}

	cache[nodeIndex] = worldTransform;
	return worldTransform;
}

glm::mat4 ComputeRelativeImportedNodeTransform(const Scene& model, int referenceNodeIndex, int sourceNodeIndex)
{
	if (sourceNodeIndex < 0 || sourceNodeIndex >= static_cast<int>(model.nodes.size())) {
		return glm::mat4(1.0f);
	}

	if (referenceNodeIndex == sourceNodeIndex) {
		return glm::mat4(1.0f);
	}

	std::unordered_map<int, glm::mat4> cache;
	glm::mat4 sourceWorld = ComputeImportedNodeWorldTransform(model, sourceNodeIndex, cache);
	if (referenceNodeIndex < 0 || referenceNodeIndex >= static_cast<int>(model.nodes.size())) {
		return sourceWorld;
	}

	glm::mat4 referenceWorld = ComputeImportedNodeWorldTransform(model, referenceNodeIndex, cache);
	glm::mat4 referenceInverse = glm::inverse(referenceWorld);
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (!std::isfinite(referenceInverse[c][r])) {
				return sourceWorld;
			}
		}
	}
	return referenceInverse * sourceWorld;
}

void RefreshDerivedSystemsRecursive(SceneNode& node, const glm::mat4& parentWorldTransform)
{
	glm::mat4 worldTransform = node.GetGlobalTransform(parentWorldTransform);
	node.UpdateTransformSystems(worldTransform);

	for (auto& child : node.children) {
		if (child) {
			RefreshDerivedSystemsRecursive(*child, worldTransform);
		}
	}
}

void ExpandBoundsWithTransformedBox(const MeshComponent& mesh,
	const glm::mat4& transform,
	glm::vec3& minBounds,
	glm::vec3& maxBounds,
	bool& valid)
{
	if (!mesh.boundingVolumeValid) {
		return;
	}

	const glm::vec3 corners[8] = {
		{mesh.boundingMin.x, mesh.boundingMin.y, mesh.boundingMin.z},
		{mesh.boundingMax.x, mesh.boundingMin.y, mesh.boundingMin.z},
		{mesh.boundingMin.x, mesh.boundingMax.y, mesh.boundingMin.z},
		{mesh.boundingMax.x, mesh.boundingMax.y, mesh.boundingMin.z},
		{mesh.boundingMin.x, mesh.boundingMin.y, mesh.boundingMax.z},
		{mesh.boundingMax.x, mesh.boundingMin.y, mesh.boundingMax.z},
		{mesh.boundingMin.x, mesh.boundingMax.y, mesh.boundingMax.z},
		{mesh.boundingMax.x, mesh.boundingMax.y, mesh.boundingMax.z}
	};

	for (const glm::vec3& corner : corners) {
		glm::vec3 transformed = glm::vec3(transform * glm::vec4(corner, 1.0f));
		if (!valid) {
			minBounds = transformed;
			maxBounds = transformed;
			valid = true;
		}
		else {
			minBounds = glm::min(minBounds, transformed);
			maxBounds = glm::max(maxBounds, transformed);
		}
	}
}
}

// CONSTRUCTORS

SceneNode::SceneNode() = default;

SceneNode::SceneNode(ComponentManager* manager, TransformSystem* transformSystem, EntityID entityID)
	: m_entityID(entityID)
	, m_componentManager(manager)
	, m_transformSystem(transformSystem)
{
}

void SceneNode::SetECSContext(ComponentManager* manager, TransformSystem* transformSystem) {
	m_componentManager = manager;
	m_transformSystem = transformSystem;

	for (auto& child : children) {
		if (child) {
			child->SetECSContext(manager, transformSystem);
		}
	}
}

ComponentManager* SceneNode::RequireComponentManager(const char* caller) const {
	if (m_entityID == INVALID_ENTITY) {
		return nullptr;
	}

	ComponentManager* resolvedManager = m_componentManager;
	if (!resolvedManager) {
		if (auto parent = parentNode.lock()) {
			resolvedManager = parent->m_componentManager;
		}
	}

	if (!resolvedManager) {
		std::cerr << "[SceneNode] " << caller << " requires ECS context but ComponentManager is null" << std::endl;
		return nullptr;
	}

	return resolvedManager;
}

// LPV VOLUME DATA

glm::mat4 SceneNode::LPVVolumeData::GetTransformMatrix() const {
	glm::mat4 translation = glm::translate(glm::mat4(1.0f), center);
	glm::mat4 rotation = glm::mat4_cast(orientation);
	glm::mat4 scale = glm::scale(glm::mat4(1.0f), extent);
	return translation * rotation * scale;
}

glm::mat4 SceneNode::LPVVolumeData::GetInverseTransformMatrix() const {
	return glm::inverse(GetTransformMatrix());
}

// MODEL

void SceneNode::SetModel(const std::shared_ptr<Scene>& model) {
	m_model = model;
}

// HIERARCHY

void SceneNode::AddChild(const std::shared_ptr<SceneNode>& child) {
	if (!child) {
		return;
	}

	if (auto oldParent = child->parentNode.lock()) {
		if (oldParent.get() != this) {
			auto& siblings = oldParent->children;
			siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
		}
	}

	child->parentNode = shared_from_this();
	child->SetECSContext(m_componentManager, m_transformSystem);
	children.push_back(child);

	if (m_componentManager && m_entityID != INVALID_ENTITY && child->GetEntityID() != INVALID_ENTITY) {
		m_componentManager->SetParent(child->GetEntityID(), m_entityID);
		if (m_transformSystem) {
			m_transformSystem->MarkSubtreeDirty(child->GetEntityID());
		}
	}

	child->InvalidateTransformCache();
}

std::shared_ptr<SceneNode> SceneNode::GetChild(int index) {
	if (index < 0 || index >= static_cast<int>(children.size())) {
		return nullptr;
	}
	return children[index];
}

std::string SceneNode::GetName() {
	if (!m_name.empty()) {
		return m_name;
	}

	if (!m_model) {
		switch (m_nodeType) {
		case NODE:   return "Node";
		case MODEL:  return "Mesh";
		case AUDIO:  return "AudioPlayer";
		case LIGHT:  return "LightSource";
		case CAMERA: return "Camera";
		case GUI:    return "GUI";
		case LPV_VOLUME: return "LPVVolume";
		case SKELETAL: return "Skeleton";
		default:     return "Unknown";
		}
	}
	return m_model->GetName();
}

// WORLD POSITION

glm::vec3 SceneNode::GetWorldPosition() const {
	glm::mat4 parentWorld(1.0f);
	if (auto parent = parentNode.lock()) {
		parentWorld = parent->GetWorldPosition4x4();
	}
	glm::mat4 worldTransform = GetGlobalTransform(parentWorld);
	return glm::vec3(worldTransform[3]);
}

glm::mat4 SceneNode::GetWorldPosition4x4() const {
	glm::mat4 parentWorld(1.0f);
	if (auto parent = parentNode.lock()) {
		parentWorld = parent->GetWorldPosition4x4();
	}
	return GetGlobalTransform(parentWorld);
}

glm::mat4 SceneNode::GetGlobalTransform(const glm::mat4& parentTransform) const {
	// Prefer ECS/TransformSystem world transform whenever available so editor and renderer
	// read the same authoritative hierarchy result.
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		return m_transformSystem->GetWorldTransform(m_entityID);
	}
	if (ComponentManager* currentManager = RequireComponentManager("GetGlobalTransform")) {
		if (const TransformComponent* comp = currentManager->GetTransform(m_entityID)) {
			return comp->worldTransform;
		}
	}

	// Compute local transform with animation
	glm::mat4 localTransform = (animatedTransform != glm::mat4(1.0f)) ? animatedTransform : transform;

	m_cachedWorldTransform = parentTransform * localTransform;
	m_worldTransformValid = true;
	return m_cachedWorldTransform;
}

// BOUNDING BOX

std::pair<glm::vec3, glm::vec3> SceneNode::GetBoundingBox() {
	glm::vec3 minB(-0.5f), maxB(0.5f);
	
	if (m_model) {
		if (!renderWholeModel && !renderMeshIndices.empty()) {
			bool valid = false;
			for (uint32_t meshIndex : renderMeshIndices) {
				if (meshIndex >= m_model->meshes.size()) {
					continue;
				}

				const MeshComponent& mesh = m_model->meshes[meshIndex];
				glm::mat4 meshTransform = glm::mat4(1.0f);
				ExpandBoundsWithTransformedBox(mesh, meshTransform, minB, maxB, valid);
			}

			if (!valid) {
				minB = glm::vec3(-0.5f);
				maxB = glm::vec3(0.5f);
			}
		}
		else {
			bool valid = false;
			for (const auto& mesh : m_model->meshes) {
				ExpandBoundsWithTransformedBox(mesh, glm::mat4(1.0f), minB, maxB, valid);
			}

			if (!valid) {
				auto [modelMin, modelMax] = m_model->GetBoundingBox();
				minB = modelMin;
				maxB = modelMax;
			}
		}
	} else {
		switch (m_nodeType) {
			case LIGHT:
			case CAMERA:
				minB = glm::vec3(-0.5f);
				maxB = glm::vec3(0.5f);
				break;
			case AUDIO:
				minB = glm::vec3(-0.25f);
				maxB = glm::vec3(0.25f);
				break;
			case GUI:
				minB = glm::vec3(-0.1f);
				maxB = glm::vec3(0.1f);
				break;
			default:
				minB = glm::vec3(-0.5f);
				maxB = glm::vec3(0.5f);
				break;
		}
	}

	// Apply local transform - for animated nodes, use animated transform if active
	glm::mat4 localTransform = transform;
	if (animatedTransform != glm::mat4(1.0f)) {
		// For skeletal animation, animatedTransform IS the local transform
		localTransform = animatedTransform;
	}

	// Transform corners
	glm::vec3 corners[8] = {
		{minB.x, minB.y, minB.z}, {maxB.x, minB.y, minB.z},
		{minB.x, maxB.y, minB.z}, {maxB.x, maxB.y, minB.z},
		{minB.x, minB.y, maxB.z}, {maxB.x, minB.y, maxB.z},
		{minB.x, maxB.y, maxB.z}, {maxB.x, maxB.y, maxB.z}
	};

	glm::vec3 transformedMin(std::numeric_limits<float>::max());
	glm::vec3 transformedMax(std::numeric_limits<float>::lowest());

	for (int i = 0; i < 8; ++i) {
		glm::vec3 corner = glm::vec3(localTransform * glm::vec4(corners[i], 1.0f));
		transformedMin = glm::min(transformedMin, corner);
		transformedMax = glm::max(transformedMax, corner);
	}

	return {transformedMin, transformedMax};
}

// TRANSFORM SETTERS

void SceneNode::SetAnimatedTransform(const glm::mat4& newAnimatedTransform) {
	animatedTransform = newAnimatedTransform;
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		const bool hasAnimatedPose = (newAnimatedTransform != glm::mat4(1.0f));
		if (hasAnimatedPose) {
			m_transformSystem->SetAnimatedTransform(m_entityID, newAnimatedTransform);
		}
		else {
			m_transformSystem->ClearAnimatedTransform(m_entityID);
		}
	}
}

void SceneNode::SetPosition(glm::vec3 pos) {
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
		return;
	}
	
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);
	
	pos = glm::clamp(pos, glm::vec3(-100000.0f), glm::vec3(100000.0f));
	
	transform = glm::translate(glm::mat4(1.0f), pos) * 
	            glm::mat4_cast(rotation) * 
	            glm::scale(glm::mat4(1.0f), scale);
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
	}
}

void SceneNode::SetRotation(glm::vec3 axis, float angle) {
	if (!std::isfinite(angle) || glm::length(axis) < 1e-6f) {
		return;
	}
	
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat oldRotation;
	glm::decompose(transform, scale, oldRotation, translation, skew, perspective);
	
	glm::quat newRotation = glm::angleAxis(angle, glm::normalize(axis));
	
	transform = glm::translate(glm::mat4(1.0f), translation) * 
	            glm::mat4_cast(newRotation) * 
	            glm::scale(glm::mat4(1.0f), scale);
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
	}
}

void SceneNode::SetScale(glm::vec3 scale) {
	if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)) {
		return;
	}
	
	scale = glm::clamp(scale, glm::vec3(1e-6f), glm::vec3(1000.0f));
	
	glm::vec3 oldScale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, oldScale, rotation, translation, skew, perspective);
	
	transform = glm::translate(glm::mat4(1.0f), translation) * 
	            glm::mat4_cast(rotation) * 
	            glm::scale(glm::mat4(1.0f), scale);
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
	}
}

void SceneNode::SetTransform(const glm::mat4& newTransform) {
	// Validate
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (!std::isfinite(newTransform[c][r])) {
				return;
			}
		}
	}
	
	transform = newTransform;
	
	if (m_rigidbody && !m_updatingFromPhysics) {
		SyncPhysicsFromTransform();
	}
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
	}
}

void SceneNode::SetWorldTransform(const glm::mat4& newWorldTransform) {
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (!std::isfinite(newWorldTransform[c][r])) {
				return;
			}
		}
	}

	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetWorldTransform(m_entityID, newWorldTransform);
		m_transformSystem->UpdateTransforms();
		SyncFromECS();
		RefreshDerivedSystemsRecursive(*this, glm::mat4(1.0f));
		return;
	}

	glm::mat4 localTransform = newWorldTransform;
	if (auto parent = parentNode.lock()) {
		glm::mat4 parentWorld = parent->GetWorldPosition4x4();
		glm::mat4 parentInverse = glm::inverse(parentWorld);
		bool validInverse = true;
		for (int c = 0; c < 4 && validInverse; ++c) {
			for (int r = 0; r < 4 && validInverse; ++r) {
				if (!std::isfinite(parentInverse[c][r])) {
					validInverse = false;
				}
			}
		}
		if (validInverse) {
			localTransform = parentInverse * newWorldTransform;
		}
	}

	transform = localTransform;
	InvalidateTransformCache();

	if (m_rigidbody && !m_updatingFromPhysics) {
		SyncPhysicsFromTransform();
	}
}

void SceneNode::SetLocalTRS(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
	glm::vec3 safeT = translation;
	glm::quat safeR = rotation;
	glm::vec3 safeS = scale;
	
	// Validate and sanitize
	if (!std::isfinite(safeT.x) || !std::isfinite(safeT.y) || !std::isfinite(safeT.z)) {
		safeT = GetPosition();
	}
	if (!std::isfinite(safeR.x) || !std::isfinite(safeR.y) || !std::isfinite(safeR.z) || !std::isfinite(safeR.w) || glm::length(safeR) < 1e-6f) {
		safeR = glm::quat(1, 0, 0, 0);
	} else {
		safeR = glm::normalize(safeR);
	}
	if (!std::isfinite(safeS.x) || !std::isfinite(safeS.y) || !std::isfinite(safeS.z)) {
		safeS = GetScale();
	}
	
	safeT = glm::clamp(safeT, glm::vec3(-100000.0f), glm::vec3(100000.0f));
	safeS = glm::clamp(safeS, glm::vec3(1e-6f), glm::vec3(1000.0f));
	
	transform = glm::translate(glm::mat4(1.0f), safeT) * 
	            glm::mat4_cast(safeR) * 
	            glm::scale(glm::mat4(1.0f), safeS);
	
	InvalidateTransformCache();
	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
	}
	if (m_rigidbody && !m_updatingFromPhysics) {
		SyncPhysicsFromTransform();
	}
}

// TRANSFORM GETTERS

glm::vec3 SceneNode::GetPosition() const {
	if (ComponentManager* currentManager = RequireComponentManager("GetPosition")) {
		if (const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			return glm::vec3(ecsTransform->localTransform[3]);
		}
	}
	return glm::vec3(transform[3]);
}

glm::vec3 SceneNode::GetRotation() const {
	glm::mat4 source = transform;
	if (ComponentManager* currentManager = RequireComponentManager("GetRotation")) {
		if (const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			source = ecsTransform->localTransform;
		}
	}
	return glm::eulerAngles(glm::quat_cast(source));
}

glm::vec3 SceneNode::GetScale() const {
	glm::mat4 source = transform;
	if (ComponentManager* currentManager = RequireComponentManager("GetScale")) {
		if (const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			source = ecsTransform->localTransform;
		}
	}
	glm::vec3 translation, scale, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(source, scale, rotation, translation, skew, perspective);
	return scale;
}

glm::mat4 SceneNode::GetTransform() const {
	if (ComponentManager* currentManager = RequireComponentManager("GetTransform")) {
		if (const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			return ecsTransform->localTransform;
		}
	}
	return transform;
}

glm::quat SceneNode::GetOrientation() const {
	glm::mat4 source = transform;
	if (ComponentManager* currentManager = RequireComponentManager("GetOrientation")) {
		if (const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			source = ecsTransform->localTransform;
		}
	}
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(source, scale, rotation, translation, skew, perspective);
	return rotation;
}

// PHYSICS

void SceneNode::SyncPhysicsFromTransform() {
	if (!m_rigidbody || m_updatingFromPhysics) return;

	// Editor-driven grabs are authored through PhysicsEngine::UpdateGizmoTarget.
	// Avoid a second transform write path from SceneNode while the editor owns the body.
	if (m_rigidbody->isEditorControlled()) {
		return;
	}

	// Scene-side writes should only push into scene-owned or kinematic bodies.
	if (!m_rigidbody->IsKinematic() && !m_rigidbody->isSceneOwned()) {
		return;
	}

	m_updatingFromPhysics = true;

	// Get WORLD position and orientation, not local
	// Physics bodies operate in world space, so we need world transform
	glm::mat4 worldTransform = GetWorldPosition4x4();
	
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

	// Update physics body position and orientation (in world space)
	m_rigidbody->setPosition(translation);
	m_rigidbody->setOrientation(rotation);
	
	// Only handle scene-authored kinematic targets here.
	if (m_rigidbody->IsKinematic()) {
		m_rigidbody->setKinematicTarget(translation, rotation);
	}
	
	// Update AABB
	m_rigidbody->computeAABB();
	
	// Update previous state to prevent interpolation artifacts
	m_rigidbody->storePreviousState();
	
	// Only wake kinematic bodies here; sleeping dynamics stay scene-owned until
	// an explicit physics-side interaction wakes them.
	if (m_rigidbody->IsKinematic()) {
		m_rigidbody->wakeUp();
	}

	m_updatingFromPhysics = false;
}

void SceneNode::InvalidateTransformCache() {
	m_worldTransformValid = false;
	m_transformCacheDirty = true;

	if (ComponentManager* currentManager = RequireComponentManager("InvalidateTransformCache")) {
		if (TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID)) {
			ecsTransform->isDirty = true;
		}
	}

	if (m_transformSystem && m_entityID != INVALID_ENTITY) {
		m_transformSystem->MarkSubtreeDirty(m_entityID);
	}

	for (auto& child : children) {
		if (child) {
			child->m_worldTransformValid = false;
			child->m_transformCacheDirty = true;
		}
	}
}

// TRANSFORM SYNCHRONIZATION

void SceneNode::UpdateTransformSystems(const glm::mat4& worldTransform) {
	// Base implementation does nothing
	// Derived classes (LightNode, AudioNode, etc.) override this to sync their systems
	// with the calculated world transform
}

// LIFECYCLE

void SceneNode::Shutdown() {
	if (m_nodeType == MODEL && m_rigidbody) {
		// Clear node attachment (RigidBody uses weak_ptr so this is safe)
		m_rigidbody->AttachNode(nullptr);
		m_rigidbody.reset();
	}

	if (m_nodeType == AUDIO) {
		AudioNode* audioNode = dynamic_cast<AudioNode*>(this);
		if (audioNode) {
			audioNode->stop();
		}
	}

	for (auto& child : children) {
		child->Shutdown();
	}
}

void SceneNode::UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle) {
	UpdateAudioNodesWithTransform(listenerPos, listenerAngle, glm::mat4(1.0f));
}

void SceneNode::UpdateAudioNodesWithTransform(const glm::vec3& listenerPos, float listenerAngle, const glm::mat4& parentWorldTransform) {
	// Calculate our world transform using parent context
	glm::mat4 worldTransform = GetGlobalTransform(parentWorldTransform);
	
	// Propagate world transform to derived node types
	UpdateTransformSystems(worldTransform);
	
	// Recursively update children with our world transform
	for (auto& child : children) {
		if (child) {
			child->UpdateAudioNodesWithTransform(listenerPos, listenerAngle, worldTransform);
		}
	}
}

// SKINNING HELPERS

std::vector<glm::mat4> SceneNode::GetBoneTransforms() const {
	std::vector<glm::mat4> boneMatrices;
	boneMatrices.reserve(boneNodes.size());
	
	// Get the skinned mesh's world transform (for proper coordinate space)
	// Bone matrices must be relative to the mesh's space, not world space
	glm::mat4 meshWorldTransform = GetWorldPosition4x4();
	glm::mat4 meshWorldInverse = glm::inverse(meshWorldTransform);
	
	// Validate meshWorldInverse
	bool meshInverseValid = true;
	for (int c = 0; c < 4 && meshInverseValid; ++c) {
		for (int r = 0; r < 4 && meshInverseValid; ++r) {
			if (!std::isfinite(meshWorldInverse[c][r])) {
				meshInverseValid = false;
			}
		}
	}
	if (!meshInverseValid) {
		meshWorldInverse = glm::mat4(1.0f);
	}
	
	for (size_t i = 0; i < boneNodes.size(); ++i) {
		if (boneNodes[i]) {
			// Get bone's WORLD transform (not local!)
			// This includes the full hierarchy and any animated transforms
			glm::mat4 boneWorld = boneNodes[i]->GetWorldPosition4x4();
			
			// Apply the correct skinning formula:
			// boneMatrix = inverse(meshWorld) * boneWorld * inverseBindMatrix
			glm::mat4 boneMatrix;
			if (i < boneInverseBindMatrices.size()) {
				boneMatrix = meshWorldInverse * boneWorld * boneInverseBindMatrices[i];
			} else {
				boneMatrix = meshWorldInverse * boneWorld;
			}
			
			// Validate the bone matrix
			bool valid = true;
			for (int c = 0; c < 4 && valid; ++c) {
				for (int r = 0; r < 4 && valid; ++r) {
					if (!std::isfinite(boneMatrix[c][r])) {
						valid = false;
					}
				}
			}
			
			boneMatrices.push_back(valid ? boneMatrix : glm::mat4(1.0f));
		} else {
			boneMatrices.push_back(glm::mat4(1.0f));
		}
	}
	
	return boneMatrices;
}

std::shared_ptr<SceneNode> SceneNode::FindNodeByIndex(int nodeIdx) {
	if (nodeIndex == nodeIdx) {
		return shared_from_this();
	}
	
	for (auto& child : children) {
		if (child) {
			auto found = child->FindNodeByIndex(nodeIdx);
			if (found) return found;
		}
	}
	
	return nullptr;
}

void SceneNode::BuildSkeleton(const Scene& model) {
	// Clear existing bone data
	boneNodes.clear();
	boneInverseBindMatrices.clear();
	
	// Check if this model has skin data
	if (!model.hasSkin || model.skin.joints.empty()) {
		isSkinned = false;
		return;
	}
	
	// Mark this node as skinned
	isSkinned = true;
	
	// Copy inverse bind matrices from the model's skin
	boneInverseBindMatrices = model.skin.inverseBindMatrices;
	
	// Resize boneNodes to match the number of joints
	boneNodes.resize(model.skin.joints.size());
	
	// Build SceneNode hierarchy for each joint
	// First, create a flat map of all joint nodes
	std::unordered_map<int, std::shared_ptr<SceneNode>> jointNodeMap;
	
	// Create SceneNodes for each joint in the skin
	for (size_t i = 0; i < model.skin.joints.size(); ++i) {
		int jointNodeIndex = model.skin.joints[i];
		
		if (jointNodeIndex < 0 || jointNodeIndex >= static_cast<int>(model.nodes.size())) {
			std::cerr << "[SceneNode::BuildSkeleton] Invalid joint node index: " << jointNodeIndex << std::endl;
			continue;
		}
		
		const auto& nodeInfo = model.nodes[jointNodeIndex];
		
		// Create a new SceneNode for this bone
		auto boneNode = std::make_shared<SceneNode>(m_componentManager, m_transformSystem);
		boneNode->nodeIndex = jointNodeIndex;
		boneNode->transform = nodeInfo.localTransform;
		boneNode->SetNodeType(SKELETAL);
		boneNode->SetName(nodeInfo.name);
		
		// Store in map and array
		jointNodeMap[jointNodeIndex] = boneNode;
		boneNodes[i] = boneNode;
	}
	
	// Establish parent-child relationships based on the glTF node hierarchy
	for (size_t i = 0; i < model.skin.joints.size(); ++i) {
		int jointNodeIndex = model.skin.joints[i];
		
		if (jointNodeIndex < 0 || jointNodeIndex >= static_cast<int>(model.nodes.size())) {
			continue;
		}
		
		const auto& nodeInfo = model.nodes[jointNodeIndex];
		auto boneNode = boneNodes[i];
		
		if (!boneNode) continue;
		
		// Find and set parent
		if (nodeInfo.parent >= 0) {
			auto parentIt = jointNodeMap.find(nodeInfo.parent);
			if (parentIt != jointNodeMap.end()) {
				// Parent is in the skeleton
				parentIt->second->AddChild(boneNode);
			} else {
				// Parent is not a joint - bone is a root of the skeleton subtree
				// Attach to this skinned mesh node
				AddChild(boneNode);
			}
		} else {
			// No parent - this is a root bone
			AddChild(boneNode);
		}
	}
	
	std::cout << "[SceneNode::BuildSkeleton] Built skeleton with " << boneNodes.size() 
	          << " joints, " << boneInverseBindMatrices.size() << " inverse bind matrices" << std::endl;
}

// ANIMATION UPDATE WITH TRANSFORM PROPAGATION

void SceneNode::UpdateAnimation(float deltaTime) {
	UpdateAnimationWithTransform(deltaTime, glm::mat4(1.0f));
}

void SceneNode::UpdateAnimationWithTransform(float deltaTime, const glm::mat4& parentWorldTransform) {
	// Delegate to AnimationSystem if we have an ECS entity
	if (ComponentManager* currentManager = RequireComponentManager("UpdateAnimationWithTransform")) {
		if (AnimationComponent* animComp = currentManager->GetAnimation(m_entityID)) {
			if (animComp->isPlaying && !animComp->isPaused) {
				SyncFromECS();
			}
		}
	}
	
	// Calculate our world transform using parent context
	glm::mat4 worldTransform = GetGlobalTransform(parentWorldTransform);
	
	// Propagate world transform to derived node types
	UpdateTransformSystems(worldTransform);
	
	// Recursively update children with our world transform
	for (auto& child : children) {
		if (child) {
			child->UpdateAnimationWithTransform(deltaTime, worldTransform);
		}
	}
}

// ECS BRIDGE

void SceneNode::SyncToECS() {
	ComponentManager* currentManager = RequireComponentManager("SyncToECS");
	if (!currentManager) return;
	
	TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID);
	if (!ecsTransform) return;
	
	if (m_transformSystem) {
		m_transformSystem->SetLocalTransform(m_entityID, transform);
		ecsTransform = currentManager->GetTransform(m_entityID);
		if (!ecsTransform) return;
	} else {
		ecsTransform->localTransform = transform;
	}
	ecsTransform->animatedTransform = animatedTransform;
	ecsTransform->hasAnimation = (animatedTransform != glm::mat4(1.0f));
	ecsTransform->isDirty = true;
	ecsTransform->prevWorldTransform = ecsTransform->worldTransform;

	EntityID parentID = INVALID_ENTITY;
	if (auto parent = parentNode.lock()) {
		parentID = parent->GetEntityID();
	}
	currentManager->SetParent(m_entityID, parentID);

	if (m_transformSystem) {
		m_transformSystem->UpdateTransforms();
		RefreshDerivedSystemsRecursive(*this, glm::mat4(1.0f));
	}
}

void SceneNode::SyncFromECS() {
	ComponentManager* currentManager = RequireComponentManager("SyncFromECS");
	if (!currentManager) return;
	
	const TransformComponent* ecsTransform = currentManager->GetTransform(m_entityID);
	if (!ecsTransform) return;
	
	transform = ecsTransform->localTransform;
	animatedTransform = ecsTransform->animatedTransform;
	m_cachedWorldTransform = ecsTransform->worldTransform;
	m_worldTransformValid = !ecsTransform->isDirty;
}

// ECS COMPONENT ACCESS

TransformComponent* SceneNode::GetTransformComponent() {
	ComponentManager* currentManager = RequireComponentManager("GetTransformComponent");
	if (!currentManager) return nullptr;
	return currentManager->GetTransform(m_entityID);
}

const TransformComponent* SceneNode::GetTransformComponent() const {
	ComponentManager* currentManager = RequireComponentManager("GetTransformComponent const");
	if (!currentManager) return nullptr;
	return currentManager->GetTransform(m_entityID);
}

RenderableComponent* SceneNode::GetRenderableComponent() {
	ComponentManager* currentManager = RequireComponentManager("GetRenderableComponent");
	if (!currentManager) return nullptr;
	return currentManager->GetRenderable(m_entityID);
}

const RenderableComponent* SceneNode::GetRenderableComponent() const {
	ComponentManager* currentManager = RequireComponentManager("GetRenderableComponent const");
	if (!currentManager) return nullptr;
	return currentManager->GetRenderable(m_entityID);
}

AnimationComponent* SceneNode::GetAnimationComponent() {
	ComponentManager* currentManager = RequireComponentManager("GetAnimationComponent");
	if (!currentManager) return nullptr;
	return currentManager->GetAnimation(m_entityID);
}

const AnimationComponent* SceneNode::GetAnimationComponent() const {
	ComponentManager* currentManager = RequireComponentManager("GetAnimationComponent const");
	if (!currentManager) return nullptr;
	return currentManager->GetAnimation(m_entityID);
}

// ECS ENTITY CREATION

EntityID SceneNode::CreateECSEntity(const std::string& name) {
	if (!m_componentManager) {
		std::cerr << "[SceneNode] Cannot create ECS entity: no ComponentManager" << std::endl;
		return INVALID_ENTITY;
	}
	
	// Map node type
	NodeType type = NodeType::NODE;
	switch (m_nodeType) {
		case NODE:       type = NodeType::NODE; break;
		case MODEL:      type = NodeType::MODEL; break;
		case LIGHT:      type = NodeType::LIGHT; break;
		case CAMERA:     type = NodeType::CAMERA; break;
		case AUDIO:      type = NodeType::AUDIO; break;
		case GUI:        type = NodeType::GUI; break;
		case LPV_VOLUME: type = NodeType::LPV_VOLUME; break;
		case SKELETAL:   type = NodeType::SKELETAL; break;
	}
	
	std::string entityName = name.empty() ? GetName() : name;
	m_entityID = m_componentManager->CreateEntity(entityName, type);
	
	// Add transform component
	TransformComponent transformComp;
	transformComp.localTransform = transform;
	transformComp.animatedTransform = animatedTransform;
	transformComp.worldTransform = m_cachedWorldTransform;
	transformComp.prevWorldTransform = m_cachedWorldTransform;
	transformComp.isDirty = true;
	transformComp.parentID = INVALID_ENTITY;
	
	m_componentManager->AddTransform(m_entityID, transformComp);

	EntityID parentID = INVALID_ENTITY;
	if (auto parent = parentNode.lock()) {
		parentID = parent->GetEntityID();
	}
	m_componentManager->SetParent(m_entityID, parentID);
	
	// Add renderable if we have a model
	if (m_model && (renderWholeModel || !renderMeshIndices.empty())) {
		RenderableComponent renderComp;
		renderComp.model = m_model;
		renderComp.shaderID = m_shader;
		renderComp.boundingRadius = boundingRadius;
		renderComp.isSkinned = isSkinned;
		renderComp.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(m_cullingOverride));
		renderComp.renderWholeModel = renderWholeModel;
		renderComp.meshIndices = renderMeshIndices;
		renderComp.boneNodes = boneNodes;
		renderComp.boneInverseBindMatrices = boneInverseBindMatrices;
		renderComp.nodeIndex = nodeIndex;
		
		m_componentManager->AddRenderable(m_entityID, renderComp);
	}
	
	return m_entityID;
}

void SceneNode::MigrateHierarchyToECS() {
	if (m_entityID == INVALID_ENTITY) {
		CreateECSEntity();
	}
	
	for (auto& child : children) {
		if (child) {
			child->MigrateHierarchyToECS();
			
			TransformComponent* childTransform = child->GetTransformComponent();
			if (childTransform) {
				m_componentManager->SetParent(child->GetEntityID(), m_entityID);
			}
		}
	}
}

std::shared_ptr<SceneNode> SceneNode::CreateWithECS(
	ComponentManager* manager,
	TransformSystem* transformSystem,
	const std::string& name,
	NodeType type)
{
	if (!manager) {
		std::cerr << "[SceneNode] Cannot create ECS-backed node: no ComponentManager" << std::endl;
		return nullptr;
	}
	
	EntityID entityID = manager->CreateEntity(name, type);
	
	TransformComponent transformComp;
	manager->AddTransform(entityID, transformComp);
	
	auto node = std::make_shared<SceneNode>(manager, transformSystem, entityID);
	
	switch (type) {
		case NodeType::NODE:       node->SetNodeType(NODE); break;
		case NodeType::MODEL:      node->SetNodeType(MODEL); break;
		case NodeType::LIGHT:      node->SetNodeType(LIGHT); break;
		case NodeType::CAMERA:     node->SetNodeType(CAMERA); break;
		case NodeType::AUDIO:      node->SetNodeType(AUDIO); break;
		case NodeType::GUI:        node->SetNodeType(GUI); break;
		case NodeType::LPV_VOLUME: node->SetNodeType(LPV_VOLUME); break;
		case NodeType::SKELETAL:   node->SetNodeType(SKELETAL); break;
	}
	
	return node;
}
