#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward declarations
class Scene;
class BaseLight;
class AnimationController;
class RigidBody;

/**
 * Component-Based Architecture for Scene Nodes
 *
 * This header defines lightweight component structures for a data-oriented
 * scene graph system. Each component type is tightly packed and stored in
 * contiguous arrays for cache efficiency.
 */

 // Unique identifier for entities in the component system
using EntityID = uint32_t;
constexpr EntityID INVALID_ENTITY = 0;

// Component type enumeration for quick type checking
enum class ComponentType : uint8_t {
	TRANSFORM = 0,
	RENDERABLE,
	LIGHT,
	CAMERA,
	AUDIO,
	ANIMATION,
	PHYSICS,
	LPV_VOLUME,
	GUI,
	COUNT
};

// Node type enum (matches SceneNode::NODE_TYPE for compatibility)
enum class NodeType : uint8_t {
	NODE = 0,
	MODEL,
	LIGHT,
	CAMERA,
	AUDIO,
	GUI,
	LPV_VOLUME
};

// Culling override modes (matches SceneNode::CullingOverride)
enum class CullingOverride : uint8_t {
	CULLING_INHERIT = 0,
	CULLING_FORCE_ENABLE,
	CULLING_FORCE_DISABLE,
	CULLING_FORCE_FRONT
};

/**
 * Transform Component - Stores local and world transformation data
 * Packed structure for cache efficiency
 */
struct alignas(16) TransformComponent {
	glm::mat4 localTransform;      // Local transformation matrix
	glm::mat4 worldTransform;      // Cached world transformation
	glm::mat4 animatedTransform;   // Animation offset transform

	EntityID parentID;     // Parent entity ID (INVALID_ENTITY if root)
	uint32_t firstChildIndex;      // Index into children array
	uint16_t childCount;           // Number of children

	bool isDirty : 1;              // World transform needs recalculation
	bool hasAnimation : 1;       // Has animated transform
	uint8_t padding : 6;

	TransformComponent()
		: localTransform(1.0f)
		, worldTransform(1.0f)
		, animatedTransform(1.0f)
		, parentID(INVALID_ENTITY)
		, firstChildIndex(0)
		, childCount(0)
		, isDirty(true)
		, hasAnimation(false)
		, padding(0)
	{
	}
};

/**
 * Renderable Component - References a model/scene for rendering
 */
struct RenderableComponent {
	std::shared_ptr<Scene> model;
	uint32_t shaderID;
	float boundingRadius;
	bool isSkinned;
	bool hasAlpha;
	CullingOverride cullingOverride;

	// Skinning data
	std::vector<std::shared_ptr<class SceneNode>> boneNodes;  // Temporary during transition
	std::vector<glm::mat4> boneInverseBindMatrices;
	int nodeIndex;  // glTF node index for skinning

	RenderableComponent()
		: shaderID(0)
		, boundingRadius(1.0f)
		, isSkinned(false)
		, hasAlpha(false)
		, cullingOverride(CullingOverride::CULLING_INHERIT)
		, nodeIndex(-1)
	{
	}
};

/**
 * Light Component - Stores light data reference
 */
struct LightComponent {
	std::shared_ptr<BaseLight> light;
	bool castsShadows;
	bool enabled;

	LightComponent()
		: castsShadows(true)
		, enabled(true)
	{
	}
};

/**
 * Camera Component - Stores camera parameters
 */
struct CameraComponent {
	glm::vec3 up;
	glm::vec3 front;
	glm::vec3 right;

	float yaw;
	float pitch;
	float fov;
	float aspectRatio;
	float nearPlane;
	float farPlane;

	float movementSpeed;
	float mouseSensitivity;

	enum class ProjectionType : uint8_t {
		Perspective,
		Orthographic
	};
	ProjectionType projectionType;

	CameraComponent()
		: up(0, 1, 0)
		, front(0, 0, -1)
		, right(1, 0, 0)
		, yaw(-90.0f)
		, pitch(0.0f)
		, fov(45.0f)
		, aspectRatio(16.0f / 9.0f)
		, nearPlane(0.1f)
		, farPlane(100.0f)
		, movementSpeed(2.5f)
		, mouseSensitivity(0.1f)
		, projectionType(ProjectionType::Perspective)
	{
	}
};

/**
 * Animation Component - Stores animation controller and state
 */
struct AnimationComponent {
	std::shared_ptr<AnimationController> controller;
	int currentAnimationIndex;
	float animationTime;
	bool isPlaying;
	bool isPaused;

	// Morph target weights
	std::vector<float> morphWeights;

	AnimationComponent()
		: currentAnimationIndex(-1)
		, animationTime(0.0f)
		, isPlaying(false)
		, isPaused(false)
	{
	}
};

/**
 * Physics Component - Stores rigid body reference
 */
struct PhysicsComponent {
	std::shared_ptr<RigidBody> rigidBody;
	bool updatingFromPhysics;

	PhysicsComponent()
		: updatingFromPhysics(false)
	{
	}
};

/**
 * Audio Component - Stores audio source data
 */
struct AudioComponent {
	uint32_t sourceID;
	uint32_t bufferID;
	bool isLooping;
	bool isPlaying;
	float volume;
	float pitch;
	float maxDistance;
	float referenceDistance;

	AudioComponent()
		: sourceID(0)
		, bufferID(0)
		, isLooping(false)
		, isPlaying(false)
		, volume(1.0f)
		, pitch(1.0f)
		, maxDistance(100.0f)
		, referenceDistance(1.0f)
	{
	}
};

/**
 * LPV Volume Component - Light Propagation Volume data
 */
struct LPVVolumeComponent {
	glm::vec3 minBounds;
	glm::vec3 maxBounds;
	glm::ivec3 gridResolution;
	float cellSize;
	uint32_t rsmTextureID;
	uint32_t lpvTextureID;
	bool enabled;

	LPVVolumeComponent()
		: minBounds(-10.0f)
		, maxBounds(10.0f)
		, gridResolution(32)
		, cellSize(0.625f)
		, rsmTextureID(0)
		, lpvTextureID(0)
		, enabled(true)
	{
	}
};

/**
 * Entity Metadata - Stores name and type information
 */
struct EntityMetadata {
	std::string name;
	NodeType nodeType;
	uint32_t componentMask;  // Bitfield of active components
	bool active;

	EntityMetadata()
		: nodeType(NodeType::NODE)
		, componentMask(0)
		, active(true)
	{
	}

	bool HasComponent(ComponentType type) const {
		return (componentMask & (1u << static_cast<uint8_t>(type))) != 0;
	}

	void AddComponent(ComponentType type) {
		componentMask |= (1u << static_cast<uint8_t>(type));
	}

	void RemoveComponent(ComponentType type) {
		componentMask &= ~(1u << static_cast<uint8_t>(type));
	}
};
