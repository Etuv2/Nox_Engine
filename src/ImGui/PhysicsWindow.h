#pragma once
// Editor "Physics" window body (EditorWorkspace owns the dockable window and its Window menu entry).
//
// Sections: World (gravity, timestep, solver, sleep, defaults for new bodies, broadphase), Collision layers (names and
// the layer matrix), Debug overlay (PhysicsEngine::DebugView, drawn in the Scene viewport), Live stats (counts and a
// step-time graph over the last 240 frames), and the selected node's body.
//
// Settings are edited on the authored copy (PhysicsEngine::GetAuthoredConfig). Each change applies live; a drag or a
// typed value becomes one undo step when the widget is released. Reset to defaults restores config.json values.
// The scene's "physics" block is written from the authored copy when the scene is saved. While playing, changes go
// to the runtime values only (no undo, not saved); Stop resets the runtime values to the authored ones.
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../editor/InspectorUndo.h"
#include "../SimulationConfig.h"

class PhysicsEngine;
class SceneNode;
namespace nox::reflect { struct ComponentDesc; }

class PhysicsWindow {
public:
	struct Host {
		std::shared_ptr<PhysicsEngine> physics;     // null when there is no physics world
		std::shared_ptr<SceneNode> selected;        // the node selected in the Scene / Hierarchy
		bool playing = false;
		bool paused = false;
		nox::editor::UndoPush undo;                 // null while playing: edits apply without an undo step
		std::function<void()> markDirty;            // null while playing
		// Draws one reflected component section of the node with the Inspector's widgets, so its edits take the
		// same undo path. Used for the RigidBody, Collider, Trigger and CharacterController of the selected node.
		std::function<void(const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc)> drawComponent;
	};

	// Draws the window contents (the caller owns ImGui::Begin / End).
	void Draw(const Host& host);

	// Changes the authored settings through mutate, applies them live and pushes one undo step.
	// False when there is no physics world. Used by the Physics window and the editor smoke test.
	bool Edit(const Host& host, const std::string& label, const std::function<void(SimulationConfig&)>& mutate);

private:
	void Apply(const Host& host, const SimulationConfig& cfg);
	void PushEdit(const Host& host, const std::string& label, const SimulationConfig& before, const SimulationConfig& after);
	void DrawWorldSettings(SimulationConfig& cfg);
	void DrawLayers(SimulationConfig& cfg);
	void DrawDebugToggles(PhysicsEngine& physics);
	void DrawStats(const Host& host);
	void DrawSelectedBody(const Host& host);
	void SampleStepTime(float ms);

	static constexpr int kSamples = 240;
	std::array<float, kSamples> m_stepMs{};
	int m_sampleHead = 0;     // index of the next write
	int m_sampleCount = 0;

	// Continuous edit in progress (a drag or typed value): the settings before its first change.
	bool m_editPending = false;
	SimulationConfig m_editBefore;

	float m_impulse[3] = { 0.0f, 0.0f, 0.0f };
};
