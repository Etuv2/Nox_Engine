#pragma once
// Local-space node poses for skeletal animation: sampling clips, blending poses and converting them to matrices.
// Pure math (no GL, no ECS). The blend rules are the contract of docs/animation.md and tests/native/test_animation_blend.cpp.

#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class Animation;
class Scene;

// Translation, rotation and scale of one scene node in its parent's space.
struct NodePose {
	glm::vec3 translation{0.0f};
	glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 scale{1.0f};
};

// Bind pose: every node's localTransform decomposed into TRS, indexed like scene.nodes.
std::vector<NodePose> MakeBindPose(const Scene& scene);

// Overwrites the channels of `clip` targeting scene nodes with their values at `time` (clip time, clamped to the
// clip's start and end; no wrapping). Nodes the clip does not target keep their value in `pose`, so the pose must be
// sized to scene.nodes (call MakeBindPose first, or copy a previous pose).
void SampleClipPose(const Animation& clip, float time, std::vector<NodePose>& pose);

// Blend of two node poses: t = 0 gives a, t = 1 gives b. Translation and scale are lerped; rotation is slerped on the
// shortest arc (b is negated when the quaternions lie in opposite hemispheres) and falls back to nlerp when the two are
// nearly parallel.
NodePose BlendNodePose(const NodePose& a, const NodePose& b, float t);

// Blends every node of `src` into `dst` with weight t (see BlendNodePose). Both poses must have the same size.
void BlendPoseInto(std::vector<NodePose>& dst, const std::vector<NodePose>& src, float t);

// Local matrices T * R * S for every node of the pose.
void PoseToLocalMatrices(const std::vector<NodePose>& pose, std::vector<glm::mat4>& out);

// Crossfade easing curves. Input and output are in [0, 1].
enum class FadeEasing { Linear, Smoothstep };
float ApplyFadeEasing(float x, FadeEasing easing);
