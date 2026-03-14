#include "SceneNode.h"
#include "Scene.h"
#include "ComponentManager.h"
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
}

ComponentManager* SceneNode::RequireComponentManager(const char* caller) const {
	if (m_entityID == INVALID_ENTITY) {
		return nullptr;
	}
	assert(m_componentManager && "SceneNode ECS context not wired: ComponentManager is null");
	if (!m_componentManager) {
		std::cerr << "[SceneNode] " << caller << " requires ECS context but ComponentManager is null" << std::endl;
		return nullptr;
	}
	return m_componentManager;
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
	child->parentNode = shared_from_this();
	if (!child->m_componentManager) {
		child->m_componentManager = m_componentManager;
	}
	if (!child->m_transformSystem) {
		child->m_transformSystem = m_transformSystem;
	}
	children.push_back(child);
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
	// Prefer ECS cached transform if available
	if (ComponentManager* currentManager = RequireComponentManager("GetGlobalTransform")) {
		if (const TransformComponent* comp = currentManager->GetTransform(m_entityID)) {
			if (!comp->isDirty) {
				return comp->worldTransform;
			}
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
		auto [modelMin, modelMax] = m_model->GetBoundingBox();
		minB = modelMin;
		maxB = modelMax;
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
	if (m_rigidbody && !m_updatingFromPhysics) {
		SyncPhysicsFromTransform();
	}
}

// TRANSFORM GETTERS

glm::vec3 SceneNode::GetPosition() const {
	return glm::vec3(transform[3]);
}

glm::vec3 SceneNode::GetRotation() const {
	return glm::eulerAngles(glm::quat_cast(transform));
}

glm::vec3 SceneNode::GetScale() const {
	glm::vec3 translation, scale, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);
	return scale;
}

glm::mat4 SceneNode::GetTransform() const {
	return transform;
}

glm::quat SceneNode::GetOrientation() const {
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);
	return rotation;
}

// PHYSICS

void SceneNode::SyncPhysicsFromTransform() {
	if (!m_rigidbody || m_updatingFromPhysics) return;

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
	
	// Only handle kinematic bodies and already-grabbed bodies
	// Do NOT auto-grab dynamic bodies - gizmo code should explicitly manage grab state
	if (m_rigidbody->IsKinematic()) {
		// Kinematic body - set target position
		m_rigidbody->setKinematicTarget(translation, rotation);
	} else if (m_rigidbody->isGizmoGrabbed()) {
		// Already grabbed by gizmo - update kinematic target
		m_rigidbody->setKinematicTarget(translation, rotation);
	}
	
	// Update AABB
	m_rigidbody->computeAABB();
	
	// Update previous state to prevent interpolation artifacts
	m_rigidbody->storePreviousState();
	
	// Wake up kinematic and grabbed bodies, but NOT dynamic bodies at rest
	if (m_rigidbody->IsKinematic() || m_rigidbody->isGizmoGrabbed()) {
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
	
	ecsTransform->localTransform = transform;
	ecsTransform->animatedTransform = animatedTransform;
	ecsTransform->hasAnimation = (animatedTransform != glm::mat4(1.0f));
	ecsTransform->isDirty = true;
	
	if (auto parent = parentNode.lock()) {
		ecsTransform->parentID = parent->GetEntityID();
	} else {
		ecsTransform->parentID = INVALID_ENTITY;
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
	transformComp.isDirty = true;
	
	if (auto parent = parentNode.lock()) {
		transformComp.parentID = parent->GetEntityID();
	}
	
	m_componentManager->AddTransform(m_entityID, transformComp);
	
	// Add renderable if we have a model
	if (m_model) {
		RenderableComponent renderComp;
		renderComp.model = m_model;
		renderComp.shaderID = m_shader;
		renderComp.boundingRadius = boundingRadius;
		renderComp.isSkinned = isSkinned;
		renderComp.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(m_cullingOverride));
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
				childTransform->parentID = m_entityID;
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