#pragma once
#include "ComponentManager.h"
#include "SceneRuntimeData.h"
#include <glm/glm.hpp>
#include <vector>

/**
 * Transform System - Efficient transform propagation using dependency graph
 *
 * This system handles hierarchical transform updates in a cache-friendly way:
 * - Batched updates instead of recursive calls
 * - Dirty flag propagation for minimal work
 * - Breadth-first traversal for better cache locality
 */
class TransformSystem {
public:
	struct Diagnostics {
		float updateTransformsMs = 0.0f;
		float runtimeDirtyEvalMs = 0.0f;
		size_t updateCallCount = 0;
		size_t pendingDirtyRoots = 0;
		size_t dirtyRootsProcessed = 0;
		size_t transformsRecomputed = 0;
		size_t findTopDirtyAncestorCalls = 0;
		size_t findTopDirtyAncestorSteps = 0;
		size_t hierarchyDepthQueryCalls = 0;
		size_t hierarchyDepthQuerySteps = 0;
		size_t runtimeDirtySpanCount = 0;
		size_t runtimeDirtySpanCoverageNodes = 0;
	};

	TransformSystem(ComponentManager* componentManager, SceneRuntimeData* runtimeScene = nullptr);
	void SetRuntimeScene(SceneRuntimeData* runtimeScene);
	void BeginFrameDiagnostics();
	const Diagnostics& GetDiagnostics() const { return m_diagnostics; }

	// Update all dirty transforms in the scene
	void UpdateTransforms();

	// Mark an entity's transform as dirty (requires recalculation)
	void MarkDirty(EntityID entity);

	// Mark an entire subtree as dirty
	void MarkSubtreeDirty(EntityID root);

	// Get world transform for an entity (calculates if dirty)
	const glm::mat4& GetWorldTransform(EntityID entity);

	// Set local transform and mark dirty
	void SetLocalTransform(EntityID entity, const glm::mat4& localTransform);

	// Set world transform (decomposes to local relative to parent)
	void SetWorldTransform(EntityID entity, const glm::mat4& worldTransform);

	// TRS convenience methods
	void SetPosition(EntityID entity, const glm::vec3& position);
	void SetRotation(EntityID entity, const glm::quat& rotation);
	void SetScale(EntityID entity, const glm::vec3& scale);
	void SetTRS(EntityID entity, const glm::vec3& translation,
		const glm::quat& rotation, const glm::vec3& scale);

	// Get decomposed transform components
	glm::vec3 GetWorldPosition(EntityID entity);
	glm::quat GetWorldRotation(EntityID entity);
	glm::vec3 GetWorldScale(EntityID entity);

	// Apply animation transform
	void SetAnimatedTransform(EntityID entity, const glm::mat4& animTransform);
	void ClearAnimatedTransform(EntityID entity);

	// Debug
	void PrintHierarchy(EntityID root = INVALID_ENTITY, int depth = 0) const;

	size_t GetLastDirtyRootCount() const { return m_lastDirtyRootCount; }
	size_t GetLastTransformsRecomputedCount() const { return m_lastTransformsRecomputed; }
	size_t GetPendingDirtyRootCount() const;
	uint64_t GetWorldPublicationGeneration() const { return m_worldPublicationGeneration; }
	const std::vector<EntityID>& GetLastChangedEntities() const { return m_lastChangedEntities; }
	const std::vector<uint32_t>& GetLastChangedRuntimeIndices() const { return m_lastChangedRuntimeIndices; }

private:
	ComponentManager* m_componentManager;
	SceneRuntimeData* m_runtimeScene = nullptr;

	// Cached list of dirty roots for batch processing
	std::vector<EntityID> m_dirtyRoots;

	// Diagnostics for deep hierarchy profiling
	size_t m_lastDirtyRootCount = 0;
	size_t m_lastTransformsRecomputed = 0;
	uint64_t m_worldPublicationGeneration = 0;
	std::vector<EntityID> m_lastChangedEntities;
	std::vector<uint32_t> m_lastChangedRuntimeIndices;
	mutable Diagnostics m_diagnostics;

	// Helper to compute world transform from local + parent
	void ComputeWorldTransform(EntityID entity);

	// Recursive helper for subtree recomputation
	void ComputeSubtreeWorldTransforms(EntityID entity, const glm::mat4& parentWorld);

	// Helper to find the highest dirty ancestor for on-demand recomputation
	EntityID FindTopDirtyAncestor(EntityID entity) const;

	// Helper to fetch a parent world transform when the parent is already clean
	glm::mat4 GetCleanParentWorldTransform(EntityID entity) const;

	// Hierarchy depth for stable dirty-root ordering
	size_t GetHierarchyDepth(EntityID entity) const;

	// Helper to get parent world transform
	glm::mat4 GetParentWorldTransform(EntityID entity) const;
};
