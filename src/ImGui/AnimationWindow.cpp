#include "AnimationWindow.h"
#include "../SceneNode.h"
#include "../SceneGraph.h"
#include "../Scene.h"
#include "../Animation.h"
#include "../ComponentTypes.h"
#include "../ComponentManager.h"
#include <IMGUI/imgui.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_transform.hpp>

AnimationWindow::AnimationWindow()
	: BaseWindow("Animation", "F8")
{
	m_size = ImVec2(320, 400);
	m_visible = true; // Visible by default for discoverability
}

void AnimationWindow::Render()
{
	if (!m_visible) return;

	// Validate state each frame to handle scene changes
	ValidateState();

	ImGui::SetNextWindowSize(m_size, ImGuiCond_FirstUseEver);

	if (ImGui::Begin("Animation Preview", &m_visible, m_flags)) {
		RenderTargetSection();

		auto target = m_targetNode.lock();
		if (!target) {
			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No target selected");
			ImGui::TextWrapped("Select a node in the Hierarchy window or choose a target above.");
		}
		else if (!HasAnimations()) {
			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No animations found");
			ImGui::TextWrapped("The selected model does not contain any animations.");
		}
		else {
			RenderAnimationListSection();
			RenderPlaybackControls();
			RenderTimelineSection();
			RenderInfoSection();
		}
	}
	ImGui::End();
}

void AnimationWindow::ValidateState()
{
	// Check if target node is still valid
	auto target = m_targetNode.lock();
	if (!target) {
		// Target was deleted - reset state
		m_selectedAnimationIndex = -1;
		m_currentTime = 0.0f;
		m_isPlaying = false;
		m_isPaused = false;
		m_cachedAnimationCount = 0;
		m_cachedDuration = 0.0f;
		m_cachedAnimationName.clear();

		// If using hierarchy selection, try to use selected node
		if (m_useHierarchySelection && m_selectedNode) {
			m_targetNode = m_selectedNode;
		}
		return;
	}

	// Check if animations are still available
	const auto* animations = GetAnimations();
	if (!animations || animations->empty()) {
		m_selectedAnimationIndex = -1;
		m_cachedAnimationCount = 0;
		m_cachedDuration = 0.0f;
		m_cachedAnimationName.clear();
		return;
	}

	// Update cached animation count
	m_cachedAnimationCount = static_cast<int>(animations->size());

	// Validate selected animation index
	if (m_selectedAnimationIndex >= m_cachedAnimationCount) {
		m_selectedAnimationIndex = -1;
		m_currentTime = 0.0f;
		m_isPlaying = false;
		m_isPaused = false;
	}

	// Update cached animation info
	if (m_selectedAnimationIndex >= 0 && m_selectedAnimationIndex < m_cachedAnimationCount) {
		const Animation& anim = (*animations)[m_selectedAnimationIndex];
		m_cachedDuration = anim.duration;
		m_cachedAnimationName = GetAnimationDisplayName(anim, m_selectedAnimationIndex);
	}

	// Sync with actual animation component state
	if (target->HasECSEntity()) {
		auto* animComp = target->GetAnimationComponent();
		if (animComp) {
			// Sync playback state from ECS
			m_isPlaying = animComp->isPlaying;
			m_isPaused = animComp->isPaused;

			// If animation is playing (not paused), sync the current time from ECS
			if (m_isPlaying && !m_isPaused && !m_isScrubbing) {
				m_currentTime = animComp->animationTime;
			}

			// Also sync the animation index if it changed externally
			if (animComp->currentAnimationIndex >= 0 &&
				animComp->currentAnimationIndex != m_selectedAnimationIndex &&
				animComp->currentAnimationIndex < m_cachedAnimationCount) {
				m_selectedAnimationIndex = animComp->currentAnimationIndex;
				const Animation& anim = (*animations)[m_selectedAnimationIndex];
				m_cachedDuration = anim.duration;
				m_cachedAnimationName = GetAnimationDisplayName(anim, m_selectedAnimationIndex);
			}
		}
	}
}

bool AnimationWindow::HasAnimations() const
{
	const auto* animations = GetAnimations();
	return animations && !animations->empty();
}

const std::vector<Animation>* AnimationWindow::GetAnimations() const
{
	auto target = m_targetNode.lock();
	if (!target) return nullptr;

	auto model = target->GetModel();
	if (!model) return nullptr;

	return &model->animations;
}

std::string AnimationWindow::GetAnimationDisplayName(const Animation& anim, int index) const
{
	if (!anim.name.empty()) {
		return anim.name;
	}
	return "Animation " + std::to_string(index);
}

void AnimationWindow::SetTargetNode(const std::shared_ptr<SceneNode>& node)
{
	m_targetNode = node;
	m_useHierarchySelection = false;
	m_selectedAnimationIndex = -1;
	m_currentTime = 0.0f;
	m_isPlaying = false;
	m_isPaused = false;
	ValidateState();
}

void AnimationWindow::SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph)
{
	BaseWindow::SetSceneGraph(sceneGraph);
	m_sceneGraphRef = sceneGraph;
	OnSceneChanged();
}

void AnimationWindow::SetSelectedNode(const std::shared_ptr<SceneNode>& node)
{
	BaseWindow::SetSelectedNode(node);

	// If using hierarchy selection, update target
	if (m_useHierarchySelection && node) {
		m_targetNode = node;
		m_selectedAnimationIndex = -1;
		m_currentTime = 0.0f;
		m_isPlaying = false;
		m_isPaused = false;
		ValidateState();
	}
}

void AnimationWindow::OnSceneChanged()
{
	// Reset all state when scene changes
	m_targetNode.reset();
	m_selectedAnimationIndex = -1;
	m_currentTime = 0.0f;
	m_isPlaying = false;
	m_isPaused = false;
	m_cachedAnimationCount = 0;
	m_cachedDuration = 0.0f;
	m_cachedAnimationName.clear();
	m_useHierarchySelection = true;
}

void AnimationWindow::RenderTargetSection()
{
	ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Target Object");

	auto target = m_targetNode.lock();

	// Show current target
	if (target) {
		ImGui::Text("Current: %s", target->GetName().c_str());
	}
	else {
		ImGui::TextDisabled("Current: None");
	}

	// Checkbox for hierarchy selection mode
	if (ImGui::Checkbox("Follow Hierarchy Selection", &m_useHierarchySelection)) {
		if (m_useHierarchySelection && m_selectedNode) {
			m_targetNode = m_selectedNode;
			m_selectedAnimationIndex = -1;
			m_currentTime = 0.0f;
			m_isPlaying = false;
			m_isPaused = false;
			ValidateState();
		}
	}

	// Manual target selection from scene
	if (!m_useHierarchySelection) {
		auto sceneGraph = m_sceneGraphRef.lock();
		if (sceneGraph && sceneGraph->GetRoot()) {
			if (ImGui::Button("Select Target...")) {
				ImGui::OpenPopup("SelectTargetPopup");
			}

			if (ImGui::BeginPopup("SelectTargetPopup")) {
				ImGui::Text("Select a node with animations:");
				ImGui::Separator();

				// Collect all nodes with models that have animations
				std::function<void(const std::shared_ptr<SceneNode>&)> renderNodeOption;
				renderNodeOption = [&](const std::shared_ptr<SceneNode>& node) {
					if (!node) return;

					auto model = node->GetModel();
					bool hasAnims = model && !model->animations.empty();

					if (hasAnims) {
						std::string label = node->GetName() + " (" +
							std::to_string(model->animations.size()) + " anims)";
						if (ImGui::Selectable(label.c_str())) {
							SetTargetNode(node);
						}
					}

					for (auto& child : node->children) {
						renderNodeOption(child);
					}
					};

				auto root = sceneGraph->GetRoot();
				for (auto& child : root->children) {
					renderNodeOption(child);
				}

				ImGui::EndPopup();
			}
		}
	}

	ImGui::Separator();
}

void AnimationWindow::RenderAnimationListSection()
{
	const auto* animations = GetAnimations();
	if (!animations) return;

	ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Animations");

	ImGui::Text("Available: %d", static_cast<int>(animations->size()));

	// Animation combo box
	const char* previewValue = (m_selectedAnimationIndex >= 0 && m_selectedAnimationIndex < static_cast<int>(animations->size()))
		? m_cachedAnimationName.c_str()
		: "-- Select Animation --";

	if (ImGui::BeginCombo("##AnimSelect", previewValue)) {
		for (int i = 0; i < static_cast<int>(animations->size()); ++i) {
			const Animation& anim = (*animations)[i];
			std::string displayName = GetAnimationDisplayName(anim, i);

			bool isSelected = (m_selectedAnimationIndex == i);
			if (ImGui::Selectable(displayName.c_str(), isSelected)) {
				// Selection changed - reset playback
				m_selectedAnimationIndex = i;
				m_currentTime = anim.startTime;
				m_cachedDuration = anim.duration;
				m_cachedAnimationName = displayName;
				m_isPlaying = false;
				m_isPaused = false;

				// Apply to target node
				Stop();
			}

			if (isSelected) {
				ImGui::SetItemDefaultFocus();
			}

			// Tooltip with animation details
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text("Duration: %s", FormatTime(anim.duration).c_str());
				ImGui::Text("Channels: %d", static_cast<int>(anim.channels.size()));
				ImGui::EndTooltip();
			}
		}
		ImGui::EndCombo();
	}

	ImGui::Separator();
}

void AnimationWindow::RenderPlaybackControls()
{
	if (m_selectedAnimationIndex < 0) {
		ImGui::TextDisabled("Select an animation to enable playback");
		return;
	}

	ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Playback");

	// Status indicator
	if (m_isPlaying && !m_isPaused) {
		ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Status: Playing");
	}
	else if (m_isPaused) {
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Status: Paused");
	}
	else {
		ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Status: Stopped");
	}

	// Playback buttons
	float buttonWidth = 50.0f;

	// Play/Pause button
	if (m_isPlaying && !m_isPaused) {
		if (ImGui::Button("Pause", ImVec2(buttonWidth, 0))) {
			Pause();
		}
	}
	else {
		if (ImGui::Button("Play", ImVec2(buttonWidth, 0))) {
			Play();
		}
	}

	ImGui::SameLine();

	// Stop button
	if (ImGui::Button("Stop", ImVec2(buttonWidth, 0))) {
		Stop();
	}

	ImGui::SameLine();

	// Restart button
	if (ImGui::Button("Restart", ImVec2(buttonWidth, 0))) {
		Restart();
	}

	// Loop toggle
	ImGui::Checkbox("Loop", &m_isLooping);

	// Playback speed
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("Speed", &m_playbackSpeed, 0.0f, 3.0f, "%.2fx");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Animation playback speed multiplier");
	}

	// Speed presets
	ImGui::SameLine();
	if (ImGui::SmallButton("1x")) m_playbackSpeed = 1.0f;
	ImGui::SameLine();
	if (ImGui::SmallButton("0.5x")) m_playbackSpeed = 0.5f;
	ImGui::SameLine();
	if (ImGui::SmallButton("2x")) m_playbackSpeed = 2.0f;

	ImGui::Separator();
}

void AnimationWindow::RenderTimelineSection()
{
	if (m_selectedAnimationIndex < 0 || m_cachedDuration <= 0.0f) {
		return;
	}

	ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Timeline");

	// Time display
	ImGui::Text("Time: %s / %s", FormatTime(m_currentTime).c_str(), FormatTime(m_cachedDuration).c_str());

	// Timeline scrub slider
	float displayTime = m_isScrubbing ? m_scrubTime : m_currentTime;
	float normalizedTime = displayTime / m_cachedDuration;

	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderFloat("##Timeline", &normalizedTime, 0.0f, 1.0f, "")) {
		m_scrubTime = normalizedTime * m_cachedDuration;

		// Detect if user is actively scrubbing
		if (ImGui::IsItemActive()) {
			if (!m_isScrubbing) {
				m_isScrubbing = true;
				// Optionally pause during scrub
			}
			// Apply scrub time immediately for visual feedback
			SetTime(m_scrubTime);
		}
	}

	// Detect when scrubbing ends
	if (m_isScrubbing && !ImGui::IsItemActive()) {
		m_isScrubbing = false;
		m_currentTime = m_scrubTime;
	}

	// Frame stepping buttons
	const auto* animations = GetAnimations();
	if (animations && m_selectedAnimationIndex >= 0) {
		const Animation& anim = (*animations)[m_selectedAnimationIndex];
		float frameStep = 1.0f / 30.0f; // Assume 30fps for stepping

		if (ImGui::Button("|<")) {
			SetTime(anim.startTime);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to start");

		ImGui::SameLine();
		if (ImGui::Button("<")) {
			SetTime(std::max(anim.startTime, m_currentTime - frameStep));
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step backward");

		ImGui::SameLine();
		if (ImGui::Button(">")) {
			SetTime(std::min(anim.endTime, m_currentTime + frameStep));
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step forward");

		ImGui::SameLine();
		if (ImGui::Button(">|")) {
			SetTime(anim.endTime);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to end");
	}

	ImGui::Separator();
}

void AnimationWindow::RenderInfoSection()
{
	if (m_selectedAnimationIndex < 0) return;

	const auto* animations = GetAnimations();
	if (!animations || m_selectedAnimationIndex >= static_cast<int>(animations->size())) return;

	const Animation& anim = (*animations)[m_selectedAnimationIndex];

	if (ImGui::CollapsingHeader("Animation Info")) {
		ImGui::Text("Name: %s", m_cachedAnimationName.c_str());
		ImGui::Text("Duration: %.3fs", anim.duration);
		ImGui::Text("Start Time: %.3fs", anim.startTime);
		ImGui::Text("End Time: %.3fs", anim.endTime);
		ImGui::Text("Channels: %d", static_cast<int>(anim.channels.size()));
		ImGui::Text("Samplers: %d", static_cast<int>(anim.samplers.size()));

		// Channel breakdown
		int transChannels = 0, rotChannels = 0, scaleChannels = 0, weightChannels = 0;
		for (const auto& channel : anim.channels) {
			switch (channel.type) {
			case Animation::ChannelType::TRANSLATION: transChannels++; break;
			case Animation::ChannelType::ROTATION: rotChannels++; break;
			case Animation::ChannelType::SCALE: scaleChannels++; break;
			case Animation::ChannelType::WEIGHTS: weightChannels++; break;
			}
		}

		if (transChannels > 0) ImGui::BulletText("Translation: %d", transChannels);
		if (rotChannels > 0) ImGui::BulletText("Rotation: %d", rotChannels);
		if (scaleChannels > 0) ImGui::BulletText("Scale: %d", scaleChannels);
		if (weightChannels > 0) ImGui::BulletText("Morph Weights: %d", weightChannels);
	}
}

void AnimationWindow::Play()
{
	if (m_selectedAnimationIndex < 0) {
		std::cout << "[AnimationWindow] Play failed: No animation selected" << std::endl;
		return;
	}

	auto target = m_targetNode.lock();
	if (!target) {
		std::cout << "[AnimationWindow] Play failed: No target node" << std::endl;
		return;
	}

	auto sceneGraph = m_sceneGraphRef.lock();
	if (!sceneGraph) {
		std::cout << "[AnimationWindow] Play failed: No scene graph" << std::endl;
		return;
	}

	std::cout << "[AnimationWindow] Playing animation " << m_selectedAnimationIndex
		<< " on node: " << target->GetName() << std::endl;

	// If paused, just resume
	if (m_isPaused) {
		m_isPaused = false;
		m_isPlaying = true;

		// Resume via AnimationSystem
		if (target->HasECSEntity()) {
			sceneGraph->GetAnimationSystem()->ResumeAnimation(target->GetEntityID());
			std::cout << "[AnimationWindow] Resumed animation on entity " << target->GetEntityID() << std::endl;
		}
		return;
	}

	m_isPlaying = true;
	m_isPaused = false;

	// Check if node has ECS entity
	if (!target->HasECSEntity()) {
		std::cout << "[AnimationWindow] Warning: Node has no ECS entity, creating one..." << std::endl;
		target->CreateECSEntity(target->GetName());
	}

	EntityID entityID = target->GetEntityID();
	std::cout << "[AnimationWindow] Entity ID: " << entityID << std::endl;

	// Check if RenderableComponent exists and has model
	auto* renderable = sceneGraph->GetComponentManager()->GetRenderable(entityID);
	if (!renderable) {
		std::cout << "[AnimationWindow] Warning: No RenderableComponent, checking node model..." << std::endl;
		auto model = target->GetModel();
		if (model) {
			std::cout << "[AnimationWindow] Node has model with " << model->animations.size() << " animations" << std::endl;
			// Create RenderableComponent
			RenderableComponent renderComp;
			renderComp.model = model;
			renderComp.isSkinned = target->isSkinned;
			renderComp.boneNodes = target->boneNodes;
			renderComp.boneInverseBindMatrices = target->boneInverseBindMatrices;
			sceneGraph->GetComponentManager()->AddRenderable(entityID, renderComp);
			renderable = sceneGraph->GetComponentManager()->GetRenderable(entityID);
		}
	}

	if (renderable && renderable->model) {
		std::cout << "[AnimationWindow] RenderableComponent has model with "
			<< renderable->model->animations.size() << " animations" << std::endl;

		// Set looping on the animation before playing
		if (m_selectedAnimationIndex < static_cast<int>(renderable->model->animations.size())) {
			renderable->model->animations[m_selectedAnimationIndex].isLooping = m_isLooping;
			std::cout << "[AnimationWindow] Set animation " << m_selectedAnimationIndex
				<< " looping to " << m_isLooping << std::endl;
		}
	}
	else {
		std::cout << "[AnimationWindow] ERROR: No model in RenderableComponent!" << std::endl;
	}

	// Use the AnimationSystem's PlayAnimation method
	sceneGraph->GetAnimationSystem()->PlayAnimation(entityID, m_selectedAnimationIndex, m_isLooping);
	std::cout << "[AnimationWindow] Called AnimationSystem::PlayAnimation" << std::endl;

	// Verify the animation component was created/updated
	auto* animComp = sceneGraph->GetComponentManager()->GetAnimation(entityID);
	if (animComp) {
		std::cout << "[AnimationWindow] AnimationComponent state - isPlaying: " << animComp->isPlaying
			<< ", isPaused: " << animComp->isPaused
			<< ", animIndex: " << animComp->currentAnimationIndex
			<< ", time: " << animComp->animationTime << std::endl;
	}
	else {
		std::cout << "[AnimationWindow] ERROR: AnimationComponent not found after PlayAnimation!" << std::endl;
	}

	// Set the starting time if we had a previous position
	if (m_currentTime > 0.0f && animComp) {
		animComp->animationTime = m_currentTime;
	}
}

void AnimationWindow::Pause()
{
	if (!m_isPlaying) return;

	m_isPaused = true;

	auto target = m_targetNode.lock();
	if (!target) return;

	auto sceneGraph = m_sceneGraphRef.lock();
	if (sceneGraph && target->HasECSEntity()) {
		sceneGraph->GetAnimationSystem()->PauseAnimation(target->GetEntityID());
	}
}

void AnimationWindow::Stop()
{
	m_isPlaying = false;
	m_isPaused = false;

	const auto* animations = GetAnimations();
	if (animations && m_selectedAnimationIndex >= 0 && m_selectedAnimationIndex < static_cast<int>(animations->size())) {
		m_currentTime = (*animations)[m_selectedAnimationIndex].startTime;
	}
	else {
		m_currentTime = 0.0f;
	}

	auto target = m_targetNode.lock();
	if (!target) return;

	auto sceneGraph = m_sceneGraphRef.lock();
	if (sceneGraph && target->HasECSEntity()) {
		sceneGraph->GetAnimationSystem()->StopAnimation(target->GetEntityID());
	}
}

void AnimationWindow::Restart()
{
	// Reset time to start
	const auto* animations = GetAnimations();
	if (animations && m_selectedAnimationIndex >= 0 && m_selectedAnimationIndex < static_cast<int>(animations->size())) {
		m_currentTime = (*animations)[m_selectedAnimationIndex].startTime;
	}
	else {
		m_currentTime = 0.0f;
	}

	m_isPlaying = false;
	m_isPaused = false;

	// Now play from start
	Play();
}

void AnimationWindow::SetTime(float time)
{
	const auto* animations = GetAnimations();
	if (!animations || m_selectedAnimationIndex < 0 || m_selectedAnimationIndex >= static_cast<int>(animations->size())) {
		return;
	}

	const Animation& anim = (*animations)[m_selectedAnimationIndex];
	m_currentTime = std::clamp(time, anim.startTime, anim.endTime);

	auto target = m_targetNode.lock();
	if (!target) return;

	auto sceneGraph = m_sceneGraphRef.lock();
	if (sceneGraph && target->HasECSEntity()) {
		EntityID entityID = target->GetEntityID();

		// Ensure animation component exists
		auto* animComp = target->GetAnimationComponent();
		if (animComp) {
			animComp->animationTime = m_currentTime;
			animComp->currentAnimationIndex = m_selectedAnimationIndex;
		}
		else {
			// Create component for scrubbing even if not "playing"
			AnimationComponent newComp;
			newComp.currentAnimationIndex = m_selectedAnimationIndex;
			newComp.animationTime = m_currentTime;
			newComp.isPlaying = false;
			newComp.isPaused = true;
			sceneGraph->GetComponentManager()->AddAnimation(entityID, newComp);
		}

		// Force an animation update to show the pose immediately
		// This requires manually applying the animation at the current time
		auto* renderable = sceneGraph->GetComponentManager()->GetRenderable(entityID);
		if (renderable && renderable->model &&
			m_selectedAnimationIndex < static_cast<int>(renderable->model->animations.size())) {

			const Animation& targetAnim = renderable->model->animations[m_selectedAnimationIndex];

			// Apply animation to bones/transform
			if (renderable->isSkinned) {
				// For skinned meshes, update bone transforms
				for (size_t i = 0; i < renderable->boneNodes.size(); ++i) {
					auto& boneNode = renderable->boneNodes[i];
					if (!boneNode) continue;

					int nodeIndex = boneNode->nodeIndex;
					if (nodeIndex < 0) continue;

					// Find channels targeting this node and apply
					glm::vec3 translation(0.0f);
					glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
					glm::vec3 scale(1.0f);
					bool hasT = false, hasR = false, hasS = false;

					for (const auto& channel : targetAnim.channels) {
						if (channel.targetNode != nodeIndex) continue;

						switch (channel.type) {
						case Animation::ChannelType::TRANSLATION:
							translation = targetAnim.InterpolateTranslation(channel, m_currentTime);
							hasT = true;
							break;
						case Animation::ChannelType::ROTATION:
							rotation = targetAnim.InterpolateRotation(channel, m_currentTime);
							hasR = true;
							break;
						case Animation::ChannelType::SCALE:
							scale = targetAnim.InterpolateScale(channel, m_currentTime);
							hasS = true;
							break;
						default:
							break;
						}
					}

					if (hasT || hasR || hasS) {
						// Build animated local transform
						glm::mat4 T = hasT ? glm::translate(glm::mat4(1.0f), translation) : glm::mat4(1.0f);
						glm::mat4 R = hasR ? glm::mat4_cast(rotation) : glm::mat4(1.0f);
						glm::mat4 S = hasS ? glm::scale(glm::mat4(1.0f), scale) : glm::mat4(1.0f);

						// If partial animation, fill in from base transform
						if (!hasT || !hasR || !hasS) {
							if (renderable->model && nodeIndex >= 0 &&
								nodeIndex < static_cast<int>(renderable->model->nodes.size())) {
								const auto& nodeInfo = renderable->model->nodes[nodeIndex];
								glm::vec3 baseScale, baseTranslation, baseSkew;
								glm::quat baseRotation;
								glm::vec4 basePerspective;
								glm::decompose(nodeInfo.localTransform, baseScale, baseRotation,
									baseTranslation, baseSkew, basePerspective);

								if (!hasT) T = glm::translate(glm::mat4(1.0f), baseTranslation);
								if (!hasR) R = glm::mat4_cast(baseRotation);
								if (!hasS) S = glm::scale(glm::mat4(1.0f), baseScale);
							}
						}

						boneNode->SetAnimatedTransform(T * R * S);
					}
				}
			}
			else {
				// For non-skinned, apply to root transform
				glm::mat4 animTransform = targetAnim.GetNodeTransform(m_currentTime);
				target->SetAnimatedTransform(animTransform);
			}
		}
	}
}

std::string AnimationWindow::FormatTime(float seconds) const
{
	int totalMs = static_cast<int>(seconds * 1000.0f);
	int secs = totalMs / 1000;
	int ms = totalMs % 1000;

	std::ostringstream oss;
	oss << secs << "." << std::setfill('0') << std::setw(3) << ms << "s";
	return oss.str();
}
