#pragma once
#include "ComponentTypes.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <cassert>

/**
 * Component Storage Pool - Cache-friendly contiguous storage for components
 * Uses sparse set pattern for fast iteration and lookup
 */
template<typename T>
class ComponentPool {
public:
	ComponentPool() {
		m_dense.reserve(256);
		m_sparse.reserve(256);
	}

	// Add component for entity, returns index
	size_t Add(EntityID entity, const T& component) {
		assert(entity != INVALID_ENTITY);

		// Check if entity already has this component
		if (Has(entity)) {
			// Update existing
			size_t denseIndex = m_sparse[entity];
			m_dense[denseIndex].component = component;
			return denseIndex;
		}

		// Add new component
		size_t denseIndex = m_dense.size();

		// Ensure sparse array is large enough
		if (entity >= m_sparse.size()) {
			m_sparse.resize(entity + 1, SIZE_MAX);
		}

		m_sparse[entity] = denseIndex;
		m_dense.push_back({ entity, component });

		return denseIndex;
	}

	// Remove component for entity
	void Remove(EntityID entity) {
		if (!Has(entity)) return;

		size_t denseIndex = m_sparse[entity];
		size_t lastIndex = m_dense.size() - 1;

		// Swap with last element
		if (denseIndex != lastIndex) {
			m_dense[denseIndex] = m_dense[lastIndex];
			EntityID movedEntity = m_dense[denseIndex].entity;
			m_sparse[movedEntity] = denseIndex;
		}

		m_dense.pop_back();
		m_sparse[entity] = SIZE_MAX;
	}

	// Get component for entity
	T* Get(EntityID entity) {
		if (!Has(entity)) return nullptr;
		return &m_dense[m_sparse[entity]].component;
	}

	const T* Get(EntityID entity) const {
		if (!Has(entity)) return nullptr;
		return &m_dense[m_sparse[entity]].component;
	}

	// Check if entity has component
	bool Has(EntityID entity) const {
		return entity < m_sparse.size() &&
			m_sparse[entity] != SIZE_MAX &&
			m_sparse[entity] < m_dense.size();
	}

	// Get dense array for iteration
	std::vector<T>& GetComponents() {
		static std::vector<T> temp;
		temp.clear();
		temp.reserve(m_dense.size());
		for (auto& entry : m_dense) {
			temp.push_back(entry.component);
		}
		return temp;
	}

	// Iterator support for range-based for loops
	struct Entry {
		EntityID entity;
		T component;
	};

	auto begin() { return m_dense.begin(); }
	auto end() { return m_dense.end(); }
	auto begin() const { return m_dense.begin(); }
	auto end() const { return m_dense.end(); }

	size_t Size() const { return m_dense.size(); }
	void Clear() {
		m_dense.clear();
		m_sparse.clear();
	}

private:
	std::vector<Entry> m_dense; // Packed array of components
	std::vector<size_t> m_sparse;     // Entity ID -> dense index mapping
};

/**
 * Component Manager - Central registry for all component pools
 * Manages entity creation, component assignment, and lookup
 */
class ComponentManager {
public:
	ComponentManager();
	~ComponentManager();

	// Entity management
	EntityID CreateEntity(const std::string& name = "", NodeType type = NodeType::NODE);
	void DestroyEntity(EntityID entity);
	bool IsEntityValid(EntityID entity) const;

	// Entity metadata
	EntityMetadata* GetMetadata(EntityID entity);
	const EntityMetadata* GetMetadata(EntityID entity) const;
	std::string GetEntityName(EntityID entity) const;
	void SetEntityName(EntityID entity, const std::string& name);

	// Component access - Transform
	TransformComponent* AddTransform(EntityID entity, const TransformComponent& transform = TransformComponent());
	TransformComponent* GetTransform(EntityID entity);
	const TransformComponent* GetTransform(EntityID entity) const;
	void RemoveTransform(EntityID entity);
	bool HasTransform(EntityID entity) const;

	// Component access - Renderable
	RenderableComponent* AddRenderable(EntityID entity, const RenderableComponent& renderable = RenderableComponent());
	RenderableComponent* GetRenderable(EntityID entity);
	const RenderableComponent* GetRenderable(EntityID entity) const;
	void RemoveRenderable(EntityID entity);
	bool HasRenderable(EntityID entity) const;

	// Component access - Light
	LightComponent* AddLight(EntityID entity, const LightComponent& light = LightComponent());
	LightComponent* GetLight(EntityID entity);
	const LightComponent* GetLight(EntityID entity) const;
	void RemoveLight(EntityID entity);
	bool HasLight(EntityID entity) const;

	// Component access - Camera
	CameraComponent* AddCamera(EntityID entity, const CameraComponent& camera = CameraComponent());
	CameraComponent* GetCamera(EntityID entity);
	const CameraComponent* GetCamera(EntityID entity) const;
	void RemoveCamera(EntityID entity);
	bool HasCamera(EntityID entity) const;

	// Component access - Animation
	AnimationComponent* AddAnimation(EntityID entity, const AnimationComponent& animation = AnimationComponent());
	AnimationComponent* GetAnimation(EntityID entity);
	const AnimationComponent* GetAnimation(EntityID entity) const;
	void RemoveAnimation(EntityID entity);
	bool HasAnimation(EntityID entity) const;

	// Component access - Physics
	PhysicsComponent* AddPhysics(EntityID entity, const PhysicsComponent& physics = PhysicsComponent());
	PhysicsComponent* GetPhysics(EntityID entity);
	const PhysicsComponent* GetPhysics(EntityID entity) const;
	void RemovePhysics(EntityID entity);
	bool HasPhysics(EntityID entity) const;

	// Component access - Collider
	ColliderComponent* AddCollider(EntityID entity, const ColliderComponent& collider = ColliderComponent());
	ColliderComponent* GetCollider(EntityID entity);
	const ColliderComponent* GetCollider(EntityID entity) const;
	void RemoveCollider(EntityID entity);
	bool HasCollider(EntityID entity) const;

	// Component access - Audio
	AudioComponent* AddAudio(EntityID entity, const AudioComponent& audio = AudioComponent());
	AudioComponent* GetAudio(EntityID entity);
	const AudioComponent* GetAudio(EntityID entity) const;
	void RemoveAudio(EntityID entity);
	bool HasAudio(EntityID entity) const;

	// Component access - LPV Volume
	LPVVolumeComponent* AddLPVVolume(EntityID entity, const LPVVolumeComponent& lpv = LPVVolumeComponent());
	LPVVolumeComponent* GetLPVVolume(EntityID entity);
	const LPVVolumeComponent* GetLPVVolume(EntityID entity) const;
	void RemoveLPVVolume(EntityID entity);
	bool HasLPVVolume(EntityID entity) const;

	// Entity lookup by name
	EntityID FindEntityByName(const std::string& name) const;
	std::vector<EntityID> FindEntitiesByType(NodeType type) const;

	// Hierarchy helpers (stored in transform component)
	void SetParent(EntityID child, EntityID parent);
	EntityID GetParent(EntityID entity) const;
	const std::vector<EntityID>& GetChildren(EntityID entity) const;
	bool ValidateHierarchyIntegrity() const;

	// Transform update queue used by the hierarchy/transform systems
	void QueueTransformUpdate(EntityID entity);
	std::vector<EntityID> ConsumePendingTransformUpdates();
	size_t GetPendingTransformUpdateCount() const;
	uint64_t GetTransformUpdateRevision() const { return m_transformUpdateRevision; }
	TransformID GetMaxAllocatedTransformID() const { return (m_nextTransformID > 0) ? (m_nextTransformID - 1) : 0; }

	// Bulk operations for iteration
	ComponentPool<TransformComponent>& GetTransformPool() { return m_transforms; }
	ComponentPool<RenderableComponent>& GetRenderablePool() { return m_renderables; }
	ComponentPool<LightComponent>& GetLightPool() { return m_lights; }
	ComponentPool<CameraComponent>& GetCameraPool() { return m_cameras; }
	ComponentPool<AnimationComponent>& GetAnimationPool() { return m_animations; }
	ComponentPool<PhysicsComponent>& GetPhysicsPool() { return m_physics; }
	ComponentPool<ColliderComponent>& GetColliderPool() { return m_colliders; }
	ComponentPool<AudioComponent>& GetAudioPool() { return m_audio; }
	ComponentPool<LPVVolumeComponent>& GetLPVVolumePool() { return m_lpvVolumes; }

	const ComponentPool<TransformComponent>& GetTransformPool() const { return m_transforms; }
	const ComponentPool<RenderableComponent>& GetRenderablePool() const { return m_renderables; }
	const ComponentPool<LightComponent>& GetLightPool() const { return m_lights; }
	const ComponentPool<CameraComponent>& GetCameraPool() const { return m_cameras; }

	// Debug and statistics
	size_t GetEntityCount() const { return m_nextEntityID - 1; }
	size_t GetActiveEntityCount() const;
	void PrintStatistics() const;

	// Cleanup
	void Clear();

private:
	// Entity ID generation
	EntityID m_nextEntityID = 1;  // Start at 1, 0 is INVALID_ENTITY
	TransformID m_nextTransformID = 1;  // Stable transform identity allocator
	std::vector<EntityID> m_freeEntityIDs;  // Recycled IDs

	// Entity metadata storage
	std::unordered_map<EntityID, EntityMetadata> m_metadata;

	// Component pools
	ComponentPool<TransformComponent> m_transforms;
	ComponentPool<RenderableComponent> m_renderables;
	ComponentPool<LightComponent> m_lights;
	ComponentPool<CameraComponent> m_cameras;
	ComponentPool<AnimationComponent> m_animations;
	ComponentPool<PhysicsComponent> m_physics;
	ComponentPool<ColliderComponent> m_colliders;
	ComponentPool<AudioComponent> m_audio;
	ComponentPool<LPVVolumeComponent> m_lpvVolumes;

	// Name to entity lookup
	std::unordered_map<std::string, EntityID> m_nameToEntity;

	// Parent -> children adjacency list
	std::unordered_map<EntityID, std::vector<EntityID>> m_childrenByParent;

	// Pending transform roots that need recomputation
	std::vector<EntityID> m_pendingTransformUpdates;
	uint64_t m_transformUpdateRevision = 1;
};
