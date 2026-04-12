#include "ComponentManager.h"
#include <iostream>
#include <algorithm>

#ifndef NDEBUG
namespace {
void RunColliderCleanupRegressionCheck(ComponentManager& componentManager) {
	EntityID destroyEntity = componentManager.CreateEntity("__debug_collider_destroy");
	componentManager.AddCollider(destroyEntity);

	auto* destroyMetadata = componentManager.GetMetadata(destroyEntity);
	assert(destroyMetadata && destroyMetadata->HasComponent(ComponentType::COLLIDER));
	assert(componentManager.GetColliderPool().Size() == 1);

	componentManager.DestroyEntity(destroyEntity);
	assert(componentManager.GetColliderPool().Size() == 0);
	assert(!componentManager.IsEntityValid(destroyEntity));

	EntityID clearEntity = componentManager.CreateEntity("__debug_collider_clear");
	componentManager.AddCollider(clearEntity);

	auto* clearMetadata = componentManager.GetMetadata(clearEntity);
	assert(clearMetadata && clearMetadata->HasComponent(ComponentType::COLLIDER));
	assert(componentManager.GetColliderPool().Size() == 1);

	componentManager.Clear();
	assert(componentManager.GetColliderPool().Size() == 0);
	assert(componentManager.GetActiveEntityCount() == 0);
	assert(componentManager.GetMetadata(clearEntity) == nullptr);
}
}
#endif

ComponentManager::ComponentManager() {
	// Pre-allocate for performance
	m_metadata.reserve(1024);
	m_nameToEntity.reserve(1024);
	m_childrenByParent.reserve(1024);
}

ComponentManager::~ComponentManager() {
	Clear();
}

EntityID ComponentManager::CreateEntity(const std::string& name, NodeType type) {
	EntityID newID;

	// Reuse freed IDs if available
	if (!m_freeEntityIDs.empty()) {
		newID = m_freeEntityIDs.back();
		m_freeEntityIDs.pop_back();
	}
	else {
		newID = m_nextEntityID++;
	}

	// Create metadata
	EntityMetadata metadata;
	metadata.name = name.empty() ? ("Entity_" + std::to_string(newID)) : name;
	metadata.nodeType = type;
	metadata.active = true;
	metadata.componentMask = 0;

	m_metadata[newID] = metadata;

	// Register name lookup
	if (!name.empty()) {
		m_nameToEntity[name] = newID;
	}

	// All entities get a transform component by default
	AddTransform(newID);

	return newID;
}

void ComponentManager::DestroyEntity(EntityID entity) {
	if (!IsEntityValid(entity)) return;

	// Get metadata
	auto it = m_metadata.find(entity);
	if (it == m_metadata.end()) return;

	// Remove from name lookup
	if (!it->second.name.empty()) {
		m_nameToEntity.erase(it->second.name);
	}

	// Remove all components
	if (HasTransform(entity)) RemoveTransform(entity);
	if (HasRenderable(entity)) RemoveRenderable(entity);
	if (HasLight(entity)) RemoveLight(entity);
	if (HasCamera(entity)) RemoveCamera(entity);
	if (HasAnimation(entity)) RemoveAnimation(entity);
	if (HasPhysics(entity)) RemovePhysics(entity);
	if (HasCollider(entity)) RemoveCollider(entity);
	if (HasAudio(entity)) RemoveAudio(entity);
	if (HasLPVVolume(entity)) RemoveLPVVolume(entity);

	// Remove metadata
	m_metadata.erase(it);

	// Add to free list
	m_freeEntityIDs.push_back(entity);
	assert(ValidateHierarchyIntegrity());
}

bool ComponentManager::IsEntityValid(EntityID entity) const {
	return entity != INVALID_ENTITY && m_metadata.find(entity) != m_metadata.end();
}

EntityMetadata* ComponentManager::GetMetadata(EntityID entity) {
	auto it = m_metadata.find(entity);
	return (it != m_metadata.end()) ? &it->second : nullptr;
}

const EntityMetadata* ComponentManager::GetMetadata(EntityID entity) const {
	auto it = m_metadata.find(entity);
	return (it != m_metadata.end()) ? &it->second : nullptr;
}

std::string ComponentManager::GetEntityName(EntityID entity) const {
	auto metadata = GetMetadata(entity);
	return metadata ? metadata->name : "";
}

void ComponentManager::SetEntityName(EntityID entity, const std::string& name) {
	auto metadata = GetMetadata(entity);
	if (!metadata) return;

	// Remove old name from lookup
	if (!metadata->name.empty()) {
		m_nameToEntity.erase(metadata->name);
	}

	// Set new name
	metadata->name = name;

	// Add new name to lookup
	if (!name.empty()) {
		m_nameToEntity[name] = entity;
	}
}

// Transform component methods
TransformComponent* ComponentManager::AddTransform(EntityID entity, const TransformComponent& transform) {
	if (!IsEntityValid(entity)) return nullptr;
	TransformComponent initialized = transform;
	if (initialized.transformID == INVALID_TRANSFORM_ID) {
		initialized.transformID = m_nextTransformID++;
		initialized.transformGeneration = 1;
	}
	else if (initialized.transformGeneration == 0) {
		initialized.transformGeneration = 1;
	}
	m_transforms.Add(entity, initialized);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::TRANSFORM);
	return GetTransform(entity);
}

TransformComponent* ComponentManager::GetTransform(EntityID entity) {
	return m_transforms.Get(entity);
}

const TransformComponent* ComponentManager::GetTransform(EntityID entity) const {
	return m_transforms.Get(entity);
}

void ComponentManager::RemoveTransform(EntityID entity) {
	auto transform = GetTransform(entity);
	if (transform) {
		const EntityID parentID = transform->parentID;
		if (parentID != INVALID_ENTITY) {
			auto parentIt = m_childrenByParent.find(parentID);
			if (parentIt != m_childrenByParent.end()) {
				auto& siblings = parentIt->second;
				auto childIt = std::find(siblings.begin(), siblings.end(), entity);
				if (childIt != siblings.end()) {
					*childIt = siblings.back();
					siblings.pop_back();
				}
				if (siblings.empty()) {
					m_childrenByParent.erase(parentIt);
				}
			}
		}

		auto childrenIt = m_childrenByParent.find(entity);
		if (childrenIt != m_childrenByParent.end()) {
			for (EntityID childID : childrenIt->second) {
				auto* childTransform = GetTransform(childID);
				if (childTransform) {
					childTransform->parentID = INVALID_ENTITY;
					childTransform->isDirty = true;
				}
			}
			m_childrenByParent.erase(childrenIt);
		}
	}

	m_transforms.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::TRANSFORM);
	assert(ValidateHierarchyIntegrity());
}

bool ComponentManager::HasTransform(EntityID entity) const {
	return m_transforms.Has(entity);
}

// Renderable component methods
RenderableComponent* ComponentManager::AddRenderable(EntityID entity, const RenderableComponent& renderable) {
	if (!IsEntityValid(entity)) return nullptr;
	m_renderables.Add(entity, renderable);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::RENDERABLE);
	return GetRenderable(entity);
}

RenderableComponent* ComponentManager::GetRenderable(EntityID entity) {
	return m_renderables.Get(entity);
}

const RenderableComponent* ComponentManager::GetRenderable(EntityID entity) const {
	return m_renderables.Get(entity);
}

void ComponentManager::RemoveRenderable(EntityID entity) {
	m_renderables.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::RENDERABLE);
}

bool ComponentManager::HasRenderable(EntityID entity) const {
	return m_renderables.Has(entity);
}

// Light component methods
LightComponent* ComponentManager::AddLight(EntityID entity, const LightComponent& light) {
	if (!IsEntityValid(entity)) return nullptr;
	m_lights.Add(entity, light);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::LIGHT);
	return GetLight(entity);
}

LightComponent* ComponentManager::GetLight(EntityID entity) {
	return m_lights.Get(entity);
}

const LightComponent* ComponentManager::GetLight(EntityID entity) const {
	return m_lights.Get(entity);
}

void ComponentManager::RemoveLight(EntityID entity) {
	m_lights.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::LIGHT);
}

bool ComponentManager::HasLight(EntityID entity) const {
	return m_lights.Has(entity);
}

// Camera component methods
CameraComponent* ComponentManager::AddCamera(EntityID entity, const CameraComponent& camera) {
	if (!IsEntityValid(entity)) return nullptr;
	m_cameras.Add(entity, camera);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::CAMERA);
	return GetCamera(entity);
}

CameraComponent* ComponentManager::GetCamera(EntityID entity) {
	return m_cameras.Get(entity);
}

const CameraComponent* ComponentManager::GetCamera(EntityID entity) const {
	return m_cameras.Get(entity);
}

void ComponentManager::RemoveCamera(EntityID entity) {
	m_cameras.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::CAMERA);
}

bool ComponentManager::HasCamera(EntityID entity) const {
	return m_cameras.Has(entity);
}

// Animation component methods
AnimationComponent* ComponentManager::AddAnimation(EntityID entity, const AnimationComponent& animation) {
	if (!IsEntityValid(entity)) return nullptr;
	m_animations.Add(entity, animation);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::ANIMATION);
	return GetAnimation(entity);
}

AnimationComponent* ComponentManager::GetAnimation(EntityID entity) {
	return m_animations.Get(entity);
}

const AnimationComponent* ComponentManager::GetAnimation(EntityID entity) const {
	return m_animations.Get(entity);
}

void ComponentManager::RemoveAnimation(EntityID entity) {
	m_animations.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::ANIMATION);
}

bool ComponentManager::HasAnimation(EntityID entity) const {
	return m_animations.Has(entity);
}

// Physics component methods
PhysicsComponent* ComponentManager::AddPhysics(EntityID entity, const PhysicsComponent& physics) {
	if (!IsEntityValid(entity)) return nullptr;
	m_physics.Add(entity, physics);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::PHYSICS);
	return GetPhysics(entity);
}

PhysicsComponent* ComponentManager::GetPhysics(EntityID entity) {
	return m_physics.Get(entity);
}

const PhysicsComponent* ComponentManager::GetPhysics(EntityID entity) const {
	return m_physics.Get(entity);
}

void ComponentManager::RemovePhysics(EntityID entity) {
	m_physics.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::PHYSICS);
}

bool ComponentManager::HasPhysics(EntityID entity) const {
	return m_physics.Has(entity);
}

// Collider component methods
ColliderComponent* ComponentManager::AddCollider(EntityID entity, const ColliderComponent& collider) {
	if (!IsEntityValid(entity)) return nullptr;
	m_colliders.Add(entity, collider);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::COLLIDER);
	return GetCollider(entity);
}

ColliderComponent* ComponentManager::GetCollider(EntityID entity) {
	return m_colliders.Get(entity);
}

const ColliderComponent* ComponentManager::GetCollider(EntityID entity) const {
	return m_colliders.Get(entity);
}

void ComponentManager::RemoveCollider(EntityID entity) {
	m_colliders.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::COLLIDER);
}

bool ComponentManager::HasCollider(EntityID entity) const {
	return m_colliders.Has(entity);
}

// Audio component methods
AudioComponent* ComponentManager::AddAudio(EntityID entity, const AudioComponent& audio) {
	if (!IsEntityValid(entity)) return nullptr;
	m_audio.Add(entity, audio);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::AUDIO);
	return GetAudio(entity);
}

AudioComponent* ComponentManager::GetAudio(EntityID entity) {
	return m_audio.Get(entity);
}

const AudioComponent* ComponentManager::GetAudio(EntityID entity) const {
	return m_audio.Get(entity);
}

void ComponentManager::RemoveAudio(EntityID entity) {
	m_audio.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::AUDIO);
}

bool ComponentManager::HasAudio(EntityID entity) const {
	return m_audio.Has(entity);
}

// LPV Volume component methods
LPVVolumeComponent* ComponentManager::AddLPVVolume(EntityID entity, const LPVVolumeComponent& lpv) {
	if (!IsEntityValid(entity)) return nullptr;
	m_lpvVolumes.Add(entity, lpv);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->AddComponent(ComponentType::LPV_VOLUME);
	return GetLPVVolume(entity);
}

LPVVolumeComponent* ComponentManager::GetLPVVolume(EntityID entity) {
	return m_lpvVolumes.Get(entity);
}

const LPVVolumeComponent* ComponentManager::GetLPVVolume(EntityID entity) const {
	return m_lpvVolumes.Get(entity);
}

void ComponentManager::RemoveLPVVolume(EntityID entity) {
	m_lpvVolumes.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::LPV_VOLUME);
}

bool ComponentManager::HasLPVVolume(EntityID entity) const {
	return m_lpvVolumes.Has(entity);
}

// Entity lookup
EntityID ComponentManager::FindEntityByName(const std::string& name) const {
	auto it = m_nameToEntity.find(name);
	return (it != m_nameToEntity.end()) ? it->second : INVALID_ENTITY;
}

std::vector<EntityID> ComponentManager::FindEntitiesByType(NodeType type) const {
	std::vector<EntityID> result;
	for (const auto& [id, metadata] : m_metadata) {
		if (metadata.nodeType == type && metadata.active) {
			result.push_back(id);
		}
	}
	return result;
}

// Hierarchy helpers
void ComponentManager::SetParent(EntityID child, EntityID parent) {
	auto childTransform = GetTransform(child);
	if (!childTransform) return;

	const EntityID oldParentID = childTransform->parentID;
	if (oldParentID == parent) {
		return;
	}

	if (parent != INVALID_ENTITY && (!IsEntityValid(parent) || !HasTransform(parent))) {
		return;
	}

	// Remove from old parent's adjacency list
	if (oldParentID != INVALID_ENTITY) {
		auto oldParentIt = m_childrenByParent.find(oldParentID);
		if (oldParentIt != m_childrenByParent.end()) {
			auto& siblings = oldParentIt->second;
			auto childIt = std::find(siblings.begin(), siblings.end(), child);
			if (childIt != siblings.end()) {
				*childIt = siblings.back();
				siblings.pop_back();
			}

			if (siblings.empty()) {
				m_childrenByParent.erase(oldParentIt);
			}
		}
	}

	// Set new parent
	childTransform->parentID = parent;
	childTransform->isDirty = true;

	// Add to new parent's adjacency list
	if (parent != INVALID_ENTITY) {
		auto& children = m_childrenByParent[parent];
		if (std::find(children.begin(), children.end(), child) == children.end()) {
			children.push_back(child);
		}
	}

	assert(ValidateHierarchyIntegrity());
}

EntityID ComponentManager::GetParent(EntityID entity) const {
	auto transform = GetTransform(entity);
	return transform ? transform->parentID : INVALID_ENTITY;
}

const std::vector<EntityID>& ComponentManager::GetChildren(EntityID entity) const {
	static const std::vector<EntityID> kEmptyChildren;
	const auto it = m_childrenByParent.find(entity);
	return (it != m_childrenByParent.end()) ? it->second : kEmptyChildren;
}

bool ComponentManager::ValidateHierarchyIntegrity() const {
	for (const auto& [parentID, children] : m_childrenByParent) {
		if (!IsEntityValid(parentID) || !HasTransform(parentID)) {
			return false;
		}

		for (EntityID childID : children) {
			if (!IsEntityValid(childID)) {
				return false;
			}

			const auto* childTransform = GetTransform(childID);
			if (!childTransform || childTransform->parentID != parentID) {
				return false;
			}
		}
	}

	for (const auto& entry : m_transforms) {
		const EntityID entityID = entry.entity;
		const auto& transform = entry.component;
		if (transform.parentID == INVALID_ENTITY) {
			continue;
		}

		auto parentIt = m_childrenByParent.find(transform.parentID);
		if (parentIt == m_childrenByParent.end()) {
			return false;
		}

		const auto& siblings = parentIt->second;
		if (std::find(siblings.begin(), siblings.end(), entityID) == siblings.end()) {
			return false;
		}
	}

	return true;
}

// Statistics
size_t ComponentManager::GetActiveEntityCount() const {
	return m_metadata.size();
}

void ComponentManager::PrintStatistics() const {
	std::cout << "=== Component Manager Statistics ===" << std::endl;
	std::cout << "Total entities: " << GetEntityCount() << std::endl;
	std::cout << "Active entities: " << GetActiveEntityCount() << std::endl;
	std::cout << "Transform components: " << m_transforms.Size() << std::endl;
	std::cout << "Renderable components: " << m_renderables.Size() << std::endl;
	std::cout << "Light components: " << m_lights.Size() << std::endl;
	std::cout << "Camera components: " << m_cameras.Size() << std::endl;
	std::cout << "Animation components: " << m_animations.Size() << std::endl;
	std::cout << "Physics components: " << m_physics.Size() << std::endl;
	std::cout << "Collider components: " << m_colliders.Size() << std::endl;
	std::cout << "Audio components: " << m_audio.Size() << std::endl;
	std::cout << "LPV Volume components: " << m_lpvVolumes.Size() << std::endl;
	std::cout << "Parents with children: " << m_childrenByParent.size() << std::endl;
}

void ComponentManager::Clear() {
	m_transforms.Clear();
	m_renderables.Clear();
	m_lights.Clear();
	m_cameras.Clear();
	m_animations.Clear();
	m_physics.Clear();
	m_colliders.Clear();
	m_audio.Clear();
	m_lpvVolumes.Clear();
	m_metadata.clear();
	m_nameToEntity.clear();
	m_childrenByParent.clear();
	m_freeEntityIDs.clear();
	m_nextEntityID = 1;
	m_nextTransformID = 1;
}
