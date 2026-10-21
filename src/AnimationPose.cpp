#include "AnimationPose.h"

#include "Animation.h"
#include "Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

std::vector<NodePose> MakeBindPose(const Scene& scene)
{
	std::vector<NodePose> pose(scene.nodes.size());
	for (size_t i = 0; i < scene.nodes.size(); ++i) {
		glm::vec3 skew;
		glm::vec4 perspective;
		glm::decompose(scene.nodes[i].localTransform, pose[i].scale, pose[i].rotation, pose[i].translation, skew, perspective);
	}
	return pose;
}

void SampleClipPose(const Animation& clip, float time, std::vector<NodePose>& pose)
{
	const float clamped = glm::clamp(time, clip.startTime, clip.endTime);
	for (const Animation::Channel& channel : clip.channels) {
		if (channel.keyframes.empty()) continue;
		if (channel.targetNode < 0 || channel.targetNode >= static_cast<int>(pose.size())) continue;
		NodePose& node = pose[static_cast<size_t>(channel.targetNode)];
		switch (channel.type) {
		case Animation::ChannelType::TRANSLATION:
			node.translation = clip.InterpolateTranslation(channel, clamped);
			break;
		case Animation::ChannelType::ROTATION:
			node.rotation = clip.InterpolateRotation(channel, clamped);
			break;
		case Animation::ChannelType::SCALE:
			node.scale = clip.InterpolateScale(channel, clamped);
			break;
		case Animation::ChannelType::WEIGHTS:
			break;
		}
	}
}

NodePose BlendNodePose(const NodePose& a, const NodePose& b, float t)
{
	NodePose out;
	out.translation = glm::mix(a.translation, b.translation, t);
	out.scale = glm::mix(a.scale, b.scale, t);

	glm::quat target = b.rotation;
	if (glm::dot(a.rotation, target) < 0.0f) {
		target = -target;   // shortest arc
	}
	const float cosTheta = glm::dot(a.rotation, target);
	if (cosTheta > 0.9995f) {
		out.rotation = glm::normalize(glm::mix(a.rotation, target, t));
	}
	else {
		out.rotation = glm::normalize(glm::slerp(a.rotation, target, t));
	}
	return out;
}

void BlendPoseInto(std::vector<NodePose>& dst, const std::vector<NodePose>& src, float t)
{
	const size_t count = std::min(dst.size(), src.size());
	for (size_t i = 0; i < count; ++i) {
		dst[i] = BlendNodePose(dst[i], src[i], t);
	}
}

void PoseToLocalMatrices(const std::vector<NodePose>& pose, std::vector<glm::mat4>& out)
{
	out.resize(pose.size());
	for (size_t i = 0; i < pose.size(); ++i) {
		const NodePose& node = pose[i];
		out[i] = glm::translate(glm::mat4(1.0f), node.translation)
			* glm::mat4_cast(node.rotation)
			* glm::scale(glm::mat4(1.0f), node.scale);
	}
}

float ApplyFadeEasing(float x, FadeEasing easing)
{
	const float c = glm::clamp(x, 0.0f, 1.0f);
	switch (easing) {
	case FadeEasing::Smoothstep:
		return c * c * (3.0f - 2.0f * c);
	case FadeEasing::Linear:
	default:
		return c;
	}
}
