#include "ComponentManager.h"
#include <iostream>
#include <algorithm>

ComponentManager::ComponentManager() {
	// Pre-allocate for performance
	m_metadata.reserve(1024);
	m_nameToEntity.reserve(1024);
	m_childrenStorage.reserve(2048);
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
	if (HasAudio(entity)) RemoveAudio(entity);
	if (HasLPVVolume(entity)) RemoveLPVVolume(entity);

	// Remove metadata
	m_metadata.erase(it);

	// Add to free list
	m_freeEntityIDs.push_back(entity);
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
	m_transforms.Add(entity, transform);
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
	m_transforms.Remove(entity);
	auto metadata = GetMetadata(entity);
	if (metadata) metadata->RemoveComponent(ComponentType::TRANSFORM);
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

	// Remove from old parent's children list
	if (childTransform->parentID != INVALID_ENTITY) {
		auto oldParent = GetTransform(childTransform->parentID);
		if (oldParent && oldParent->childCount > 0) {
			// Find and remove from children storage
			uint32_t startIdx = oldParent->firstChildIndex;
			uint32_t endIdx = startIdx + oldParent->childCount;
			for (uint32_t i = startIdx; i < endIdx; ++i) {
				if (m_childrenStorage[i] == child) {
					// Swap with last child and decrease count
					m_childrenStorage[i] = m_childrenStorage[endIdx - 1];
					oldParent->childCount--;
					break;
				}
			}
		}
	}

	// Set new parent
	childTransform->parentID = parent;
	childTransform->isDirty = true;

	// Add to new parent's children list
	if (parent != INVALID_ENTITY) {
		auto parentTransform = GetTransform(parent);
		if (parentTransform) {
			if (parentTransform->childCount == 0) {
				// First child
				parentTransform->firstChildIndex = static_cast<uint32_t>(m_childrenStorage.size());
				m_childrenStorage.push_back(child);
			}
			else {
				// Append to existing children
				m_childrenStorage.push_back(child);
			}
			parentTransform->childCount++;
		}
	}
}

EntityID ComponentManager::GetParent(EntityID entity) const {
	auto transform = GetTransform(entity);
	return transform ? transform->parentID : INVALID_ENTITY;
}

std::vector<EntityID> ComponentManager::GetChildren(EntityID entity) const {
	std::vector<EntityID> result;
	auto transform = GetTransform(entity);
	if (!transform || transform->childCount == 0) return result;

	uint32_t startIdx = transform->firstChildIndex;
	uint32_t count = transform->childCount;
	result.reserve(count);

	for (uint32_t i = 0; i < count; ++i) {
		result.push_back(m_childrenStorage[startIdx + i]);
	}

	return result;
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
	std::cout << "Audio components: " << m_audio.Size() << std::endl;
	std::cout << "LPV Volume components: " << m_lpvVolumes.Size() << std::endl;
	std::cout << "Children storage size: " << m_childrenStorage.size() << std::endl;
}

void ComponentManager::Clear() {
	m_transforms.Clear();
	m_renderables.Clear();
	m_lights.Clear();
	m_cameras.Clear();
	m_animations.Clear();
	m_physics.Clear();
	m_audio.Clear();
	m_lpvVolumes.Clear();
	m_metadata.clear();
	m_nameToEntity.clear();
	m_childrenStorage.clear();
	m_freeEntityIDs.clear();
	m_nextEntityID = 1;
}
