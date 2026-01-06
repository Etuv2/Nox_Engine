#pragma once

#include "BaseWindow.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>

// Forward declarations
class SceneNode;
class SceneGraph;
class Animation;

/**
 * @brief Animation preview and control window
 *
 * Provides editor-facing tools for previewing and controlling animation playback.
 * Supports:
 * - Target object selection (defaults to hierarchy selection)
 * - Animation list display and selection
 * - Playback controls (play, pause, stop, restart)
 * - Loop toggle and playback speed control
 * - Timeline scrubbing with immediate pose updates
 * - Robust handling of scene changes and node deletion
 */
class AnimationWindow : public BaseWindow {
public:
	AnimationWindow();

	void Render() override;

	// Target node management
	void SetTargetNode(const std::shared_ptr<SceneNode>& node);
	std::shared_ptr<SceneNode> GetTargetNode() const { return m_targetNode.lock(); }

	// Scene graph for node selection
	void SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph) override;

	// Selected node from hierarchy (auto-updates target if no explicit target set)
	void SetSelectedNode(const std::shared_ptr<SceneNode>& node) override;

	// Called when scene changes - revalidates state
	void OnSceneChanged();

private:
	// Validate current state (target node, animation, etc.)
	void ValidateState();

	// Check if target node has animations available
	bool HasAnimations() const;

	// Get animations from target node's model
	const std::vector<Animation>* GetAnimations() const;

	// Get animation name with fallback for unnamed animations
	std::string GetAnimationDisplayName(const Animation& anim, int index) const;

	// Playback control methods
	void Play();
	void Pause();
	void Stop();
	void Restart();
	void SetTime(float time);

	// Render sections
	void RenderTargetSection();
	void RenderAnimationListSection();
	void RenderPlaybackControls();
	void RenderTimelineSection();
	void RenderInfoSection();

	// Target node (weak pointer to handle deletion)
	std::weak_ptr<SceneNode> m_targetNode;

	// Tracks if we're using hierarchy selection or explicit target
	bool m_useHierarchySelection = true;

	// Animation state
	int m_selectedAnimationIndex = -1;
	float m_currentTime = 0.0f;
	float m_playbackSpeed = 1.0f;
	bool m_isPlaying = false;
	bool m_isPaused = false;
	bool m_isLooping = true;

	// UI state
	bool m_isScrubbing = false;
	float m_scrubTime = 0.0f;

	// Cached animation info (updated on target/animation change)
	std::string m_cachedAnimationName;
	float m_cachedDuration = 0.0f;
	int m_cachedAnimationCount = 0;

	// Scene reference for node lookup
	std::weak_ptr<SceneGraph> m_sceneGraphRef;

	// Formatting helpers
	std::string FormatTime(float seconds) const;
};
