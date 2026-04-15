#include "TransformSystem.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <iostream>
#include <queue>
#include <unordered_map>

TransformSystem::TransformSystem(ComponentManager* componentManager)
	: m_componentManager(componentManager)
{
	m_dirtyRoots.reserve(256);
}

void TransformSystem::UpdateTransforms() {
	if (!m_componentManager) return;

	m_lastDirtyRootCount = 0;
	m_lastTransformsRecomputed = 0;
	m_dirtyRoots.clear();

	// Start with explicitly queued dirty roots from hierarchy mutations.
	// The actual subtree recomputation happens top-down from those roots, using the
	// authoritative child adjacency cache instead of recursively scanning the full pool.
	auto queuedDirtyRoots = m_componentManager->ConsumePendingTransformUpdates();
	m_dirtyRoots.insert(m_dirtyRoots.end(), queuedDirtyRoots.begin(), queuedDirtyRoots.end());

	// Keep a safety net for legacy direct dirty writes that still exist in the codebase.
	auto& transformPool = m_componentManager->GetTransformPool();
	for (auto& entry : transformPool) {
		if (entry.component.isDirty) {
			m_dirtyRoots.push_back(entry.entity);
		}
	}

	if (m_dirtyRoots.empty()) {
		return;
	}

	std::unordered_map<EntityID, size_t> depthCache;
	auto getDepth = [&](EntityID entity) -> size_t {
		auto it = depthCache.find(entity);
		if (it != depthCache.end()) {
			return it->second;
		}

		size_t depth = 0;
		EntityID current = entity;
		while (current != INVALID_ENTITY) {
			auto* transform = m_componentManager->GetTransform(current);
			if (!transform || transform->parentID == INVALID_ENTITY) {
				break;
			}
			++depth;
			current = transform->parentID;
		}

		depthCache[entity] = depth;
		return depth;
	};

	std::sort(m_dirtyRoots.begin(), m_dirtyRoots.end(),
		[&](EntityID a, EntityID b) {
			const size_t depthA = getDepth(a);
			const size_t depthB = getDepth(b);
			if (depthA == depthB) {
				return a < b;
			}
			return depthA < depthB;
		});

	m_dirtyRoots.erase(std::unique(m_dirtyRoots.begin(), m_dirtyRoots.end()), m_dirtyRoots.end());

	for (EntityID entity : m_dirtyRoots) {
		auto transform = m_componentManager->GetTransform(entity);
		if (!transform || !transform->isDirty) {
			continue;
		}

		EntityID dirtyRoot = FindTopDirtyAncestor(entity);
		if (dirtyRoot == INVALID_ENTITY) {
			continue;
		}

		auto rootTransform = m_componentManager->GetTransform(dirtyRoot);
		if (!rootTransform || !rootTransform->isDirty) {
			continue;
		}

		ComputeWorldTransform(dirtyRoot);
		++m_lastDirtyRootCount;
	}
}

void TransformSystem::MarkDirty(EntityID entity) {
	MarkSubtreeDirty(entity);
}

void TransformSystem::MarkSubtreeDirty(EntityID root) {
	auto transform = m_componentManager->GetTransform(root);
	if (!transform) return;

	// Only the root of the affected subtree needs to be queued; descendants are
	// recomputed by walking the authoritative child adjacency when the root is processed.
	transform->isDirty = true;
	m_componentManager->QueueTransformUpdate(root);
}

const glm::mat4& TransformSystem::GetWorldTransform(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) {
		static glm::mat4 identity(1.0f);
		return identity;
	}

	EntityID dirtyRoot = FindTopDirtyAncestor(entity);
	if (dirtyRoot != INVALID_ENTITY) {
		auto dirtyTransform = m_componentManager->GetTransform(dirtyRoot);
		if (dirtyTransform && dirtyTransform->isDirty) {
			ComputeWorldTransform(dirtyRoot);
		}
	}

	return transform->worldTransform;
}

void TransformSystem::ComputeWorldTransform(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	const glm::mat4 parentWorld = GetCleanParentWorldTransform(entity);
	ComputeSubtreeWorldTransforms(entity, parentWorld);
}

void TransformSystem::ComputeSubtreeWorldTransforms(EntityID entity, const glm::mat4& parentWorld) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// For skeletal animation, animatedTransform contains the COMPLETE local transform
	// (either from animation data or from the base node transform)
	// We use it directly when hasAnimation is true, otherwise use localTransform
	glm::mat4 combinedLocal = transform->localTransform;
	if (transform->hasAnimation && transform->animatedTransform != glm::mat4(1.0f)) {
		combinedLocal = transform->animatedTransform;
	}

	// Compute world transform
	transform->prevWorldTransform = transform->worldTransform;
	transform->worldTransform = parentWorld * combinedLocal;
	transform->isDirty = false;
	++m_lastTransformsRecomputed;

	const glm::mat4 currentWorld = transform->worldTransform;
	const auto& children = m_componentManager->GetChildren(entity);
	for (EntityID child : children) {
		ComputeSubtreeWorldTransforms(child, currentWorld);
	}
}

glm::mat4 TransformSystem::GetParentWorldTransform(EntityID entity) const {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform || transform->parentID == INVALID_ENTITY) {
		return glm::mat4(1.0f);
	}

	auto parentTransform = m_componentManager->GetTransform(transform->parentID);
	if (!parentTransform) {
		return glm::mat4(1.0f);
	}

	// Parent world transforms must be authoritative before children compose against them.
	// Imported glTF hierarchies often involve deep non-root chains, and reading a stale
	// parent matrix here causes child renderables/gizmo edits to appear detached.
	EntityID dirtyRoot = const_cast<TransformSystem*>(this)->FindTopDirtyAncestor(transform->parentID);
	if (dirtyRoot != INVALID_ENTITY) {
		auto* dirtyTransform = m_componentManager->GetTransform(dirtyRoot);
		if (dirtyTransform && dirtyTransform->isDirty) {
			const_cast<TransformSystem*>(this)->ComputeWorldTransform(dirtyRoot);
		}
	}

	return parentTransform->worldTransform;
}

void TransformSystem::SetLocalTransform(EntityID entity, const glm::mat4& localTransform) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	transform->localTransform = localTransform;
	MarkSubtreeDirty(entity);
}

void TransformSystem::SetWorldTransform(EntityID entity, const glm::mat4& worldTransform) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// Convert world transform to local relative to parent
	glm::mat4 parentWorld = GetParentWorldTransform(entity);
	glm::mat4 localTransform = glm::inverse(parentWorld) * worldTransform;

	transform->localTransform = localTransform;
	MarkSubtreeDirty(entity);
}

void TransformSystem::SetPosition(EntityID entity, const glm::vec3& position) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// Extract current rotation and scale
	glm::vec3 scale, translation, skew;
	glm::quat rotation;
	glm::vec4 perspective;
	glm::decompose(transform->localTransform, scale, rotation, translation, skew, perspective);

	// Build new transform with updated position
	glm::mat4 T = glm::translate(glm::mat4(1.0f), position);
	glm::mat4 R = glm::mat4_cast(rotation);
	glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

	transform->localTransform = T * R * S;
	MarkSubtreeDirty(entity);
}

void TransformSystem::SetRotation(EntityID entity, const glm::quat& rotation) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// Extract current position and scale
	glm::vec3 scale, translation, skew;
	glm::quat oldRotation;
	glm::vec4 perspective;
	glm::decompose(transform->localTransform, scale, oldRotation, translation, skew, perspective);

	// Build new transform with updated rotation
	glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
	glm::mat4 R = glm::mat4_cast(rotation);
	glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

	transform->localTransform = T * R * S;
	MarkSubtreeDirty(entity);
}

void TransformSystem::SetScale(EntityID entity, const glm::vec3& scale) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// Extract current position and rotation
	glm::vec3 oldScale, translation, skew;
	glm::quat rotation;
	glm::vec4 perspective;
	glm::decompose(transform->localTransform, oldScale, rotation, translation, skew, perspective);

	// Build new transform with updated scale
	glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
	glm::mat4 R = glm::mat4_cast(rotation);
	glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

	transform->localTransform = T * R * S;
	MarkSubtreeDirty(entity);
}

void TransformSystem::SetTRS(EntityID entity, const glm::vec3& translation,
	const glm::quat& rotation, const glm::vec3& scale) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
	glm::mat4 R = glm::mat4_cast(rotation);
	glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

	transform->localTransform = T * R * S;
	MarkSubtreeDirty(entity);
}

glm::vec3 TransformSystem::GetWorldPosition(EntityID entity) {
	const glm::mat4& worldTransform = GetWorldTransform(entity);
	return glm::vec3(worldTransform[3]);
}

glm::quat TransformSystem::GetWorldRotation(EntityID entity) {
	const glm::mat4& worldTransform = GetWorldTransform(entity);

	glm::vec3 scale, translation, skew;
	glm::quat rotation;
	glm::vec4 perspective;
	glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

	return rotation;
}

glm::vec3 TransformSystem::GetWorldScale(EntityID entity) {
	const glm::mat4& worldTransform = GetWorldTransform(entity);

	glm::vec3 scale, translation, skew;
	glm::quat rotation;
	glm::vec4 perspective;
	glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

	return scale;
}

void TransformSystem::SetAnimatedTransform(EntityID entity, const glm::mat4& animTransform) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	transform->animatedTransform = animTransform;
	transform->hasAnimation = true;
	MarkSubtreeDirty(entity);
}

void TransformSystem::ClearAnimatedTransform(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	transform->animatedTransform = glm::mat4(1.0f);
	transform->hasAnimation = false;
	MarkSubtreeDirty(entity);
}

void TransformSystem::PrintHierarchy(EntityID root, int depth) const {
	if (!m_componentManager) return;

	// If root is invalid, print all root entities
	if (root == INVALID_ENTITY) {
		auto& transformPool = m_componentManager->GetTransformPool();
		for (auto& entry : transformPool) {
			if (entry.component.parentID == INVALID_ENTITY) {
				PrintHierarchy(entry.entity, 0);
			}
		}
		return;
	}

	// Print this entity
	std::string indent(depth * 2, ' ');
	auto metadata = m_componentManager->GetMetadata(root);
	std::string name = metadata ? metadata->name : "Unknown";

	std::cout << indent << "- " << name << " (ID: " << root << ")" << std::endl;

	// Recursively print children
	const auto& children = m_componentManager->GetChildren(root);
	for (EntityID child : children) {
		PrintHierarchy(child, depth + 1);
	}
}

size_t TransformSystem::GetPendingDirtyRootCount() const {
	return m_componentManager ? m_componentManager->GetPendingTransformUpdateCount() : 0;
}

EntityID TransformSystem::FindTopDirtyAncestor(EntityID entity) const {
	EntityID current = entity;
	EntityID highestDirtyAncestor = INVALID_ENTITY;
	while (current != INVALID_ENTITY) {
		auto currentTransform = m_componentManager->GetTransform(current);
		if (!currentTransform) {
			break;
		}

		if (currentTransform->isDirty) {
			highestDirtyAncestor = current;
		}

		if (currentTransform->parentID == INVALID_ENTITY) {
			break;
		}

		current = currentTransform->parentID;
	}

	return highestDirtyAncestor;
}

glm::mat4 TransformSystem::GetCleanParentWorldTransform(EntityID entity) const {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform || transform->parentID == INVALID_ENTITY) {
		return glm::mat4(1.0f);
	}

	auto parentTransform = m_componentManager->GetTransform(transform->parentID);
	if (!parentTransform) {
		return glm::mat4(1.0f);
	}

	return parentTransform->worldTransform;
}

size_t TransformSystem::GetHierarchyDepth(EntityID entity) const {
	size_t depth = 0;
	EntityID current = entity;
	while (current != INVALID_ENTITY) {
		auto* transform = m_componentManager->GetTransform(current);
		if (!transform || transform->parentID == INVALID_ENTITY) {
			break;
		}

		++depth;
		current = transform->parentID;
	}

	return depth;
}
