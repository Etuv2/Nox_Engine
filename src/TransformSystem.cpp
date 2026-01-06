#include "TransformSystem.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <iostream>
#include <queue>

TransformSystem::TransformSystem(ComponentManager* componentManager)
	: m_componentManager(componentManager)
{
	m_dirtyEntities.reserve(256);
}

void TransformSystem::UpdateTransforms() {
	if (!m_componentManager) return;

	// Collect all dirty entities
	m_dirtyEntities.clear();

	auto& transformPool = m_componentManager->GetTransformPool();
	for (auto& entry : transformPool) {
		if (entry.component.isDirty) {
			m_dirtyEntities.push_back(entry.entity);
		}
	}

	// Update in breadth-first order to respect dependencies
	// Process parents before children naturally
	std::queue<EntityID> queue;

	// Start with root entities (no parent)
	for (EntityID entity : m_dirtyEntities) {
		auto transform = m_componentManager->GetTransform(entity);
		if (transform && transform->parentID == INVALID_ENTITY) {
			queue.push(entity);
		}
	}

	// Process queue
	while (!queue.empty()) {
		EntityID current = queue.front();
		queue.pop();

		// Compute world transform
		ComputeWorldTransform(current);

		// Add children to queue
		auto children = m_componentManager->GetChildren(current);
		for (EntityID child : children) {
			auto childTransform = m_componentManager->GetTransform(child);
			if (childTransform && childTransform->isDirty) {
				queue.push(child);
			}
		}
	}
}

void TransformSystem::MarkDirty(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (transform) {
		transform->isDirty = true;
	}
}

void TransformSystem::MarkSubtreeDirty(EntityID root) {
	MarkSubtreeDirtyRecursive(root);
}

void TransformSystem::MarkSubtreeDirtyRecursive(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	transform->isDirty = true;

	// Recursively mark children
	auto children = m_componentManager->GetChildren(entity);
	for (EntityID child : children) {
		MarkSubtreeDirtyRecursive(child);
	}
}

const glm::mat4& TransformSystem::GetWorldTransform(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) {
		static glm::mat4 identity(1.0f);
		return identity;
	}

	// Update if dirty
	if (transform->isDirty) {
		ComputeWorldTransform(entity);
	}

	return transform->worldTransform;
}

void TransformSystem::ComputeWorldTransform(EntityID entity) {
	auto transform = m_componentManager->GetTransform(entity);
	if (!transform) return;

	// For skeletal animation, animatedTransform contains the COMPLETE local transform
	// (either from animation data or from the base node transform)
	// We use it directly when hasAnimation is true, otherwise use localTransform
	glm::mat4 combinedLocal = transform->localTransform;
	if (transform->hasAnimation && transform->animatedTransform != glm::mat4(1.0f)) {
		combinedLocal = transform->animatedTransform;
	}

	// Get parent world transform
	glm::mat4 parentWorld = GetParentWorldTransform(entity);

	// Compute world transform
	transform->worldTransform = parentWorld * combinedLocal;
	transform->isDirty = false;
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

	// Return parent's world transform (may need to compute if dirty)
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
	auto children = m_componentManager->GetChildren(root);
	for (EntityID child : children) {
		PrintHierarchy(child, depth + 1);
	}
}
