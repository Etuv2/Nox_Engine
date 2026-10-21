#pragma once
// Parameter-driven animation state machine for one skinned node: states (a clip, or a 1D blend space over clips),
// float / bool / trigger parameters, transitions with conditions, crossfades and per-clip event markers.
// The graph is authored as JSON text (the Animator "graph" property, docs/animation.md). Pure logic: no GL, no ECS.

#include "AnimationPose.h"

#include <string>
#include <vector>

class Scene;

class AnimatorRuntime {
public:
	enum class ParamType { Float, Bool, Trigger };
	enum class CompareOp { Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual, Set };

	struct Param {
		std::string name;
		ParamType type = ParamType::Float;
		float value = 0.0f;   // float value (Float), 0 or 1 (Bool), 1 while a trigger is pending (Trigger)
	};

	struct Condition {
		int param = -1;       // index into params
		CompareOp op = CompareOp::Greater;
		float value = 0.0f;   // threshold; for Bool use 0 or 1 with Equal, for Trigger the op is Set
	};

	struct Event {
		std::string name;
		float phase = 0.0f;   // normalised clip time in [0, 1)
	};

	struct BlendPoint {
		int clip = -1;        // index into scene.animations
		float threshold = 0.0f;
	};

	struct State {
		std::string name;
		int clip = -1;        // single-clip state (blend is empty)
		int blendParam = -1;  // 1D blend space: float parameter index; points sorted by threshold
		std::vector<BlendPoint> blend;
		float speed = 1.0f;
		bool loop = true;
		std::vector<Event> events;
	};

	struct Transition {
		int from = -1;        // state index, or kAnyState
		int to = -1;
		float duration = 0.0f;        // crossfade seconds; 0 switches at once
		FadeEasing easing = FadeEasing::Linear;
		float exitTime = -1.0f;       // normalised time the source state must reach; negative = none
		std::vector<Condition> when;  // all must hold
	};

	struct FiredEvent {
		std::string name;
		std::string clip;
		float phase = 0.0f;
	};

	static constexpr int kAnyState = -2;

	// Stores the graph text. It is parsed on the next Bind() with a scene, because clip names resolve against the model.
	// Changing the text drops the parsed graph and the playback.
	void SetGraphJson(const std::string& json);
	const std::string& GraphJson() const { return m_graphJson; }
	bool HasGraph() const { return !m_graphJson.empty(); }

	// Binds to the model: builds the bind pose and parses the graph (once per scene). False when the graph is invalid;
	// the reason is in LastError(). Safe to call every frame.
	bool Bind(const Scene& scene);
	bool IsBound() const { return m_bound != nullptr; }
	bool GraphValid() const { return m_graphParsed && m_graphValid; }

	// Starts the graph at its default state (cut, no crossfade). Needs a bound, valid graph.
	void StartGraph();
	// Plays one clip as a single-clip state. A crossfade of blendTime seconds starts from `fromPose` when given, otherwise
	// from the current pose when one exists, otherwise it cuts.
	void PlayClip(const Scene& scene, int clipIndex, bool loop, float blendTime, const std::vector<NodePose>* fromPose = nullptr);
	// Drops playback: no pose until the next start.
	void Reset();

	// Advances playback by dt seconds, evaluates transitions and recomputes the pose. Call once per simulation step.
	void Update(float dt, const Scene& scene);

	// Parameters. False when the graph is not valid or the name or type does not match.
	bool SetFloat(const std::string& name, float value);
	bool SetBool(const std::string& name, bool value);
	bool SetTrigger(const std::string& name);
	bool GetFloat(const std::string& name, float& out) const;

	// Name of the current state, or "" when nothing plays.
	std::string CurrentStateName() const;
	bool IsRunning() const { return m_state >= 0; }
	bool HasPose() const { return m_hasPose; }
	bool IsFading() const { return m_fromValid && m_fadeElapsed < m_fadeDuration; }

	// Local matrix of a node in the current pose; identity when there is no pose or the index is out of range.
	const glm::mat4& LocalMatrix(int node) const;
	const std::vector<NodePose>& Pose() const { return m_pose; }

	// Primary clip of the current state (the single clip, or the heavier clip of a blend) and its clip time.
	int PrimaryClip() const { return m_primaryClip; }
	float PrimaryClipTime() const { return m_primaryClipTime; }
	// Normalised playhead of the current state: [0, 1) when looping, clamped to [0, 1] otherwise.
	float Phase() const;

	// Events fired since the last call (marker crossings; once per loop).
	std::vector<FiredEvent> TakeEvents();

	const std::string& LastError() const { return m_lastError; }

private:
	int FindParam(const std::string& name) const;
	int FindState(const std::string& name) const;
	bool ParseGraph(const Scene& scene, std::string& error);
	bool EvaluateCondition(const Condition& c) const;
	float BlendWeightOf(const State& state, int& clipA, int& clipB) const;   // returns t toward clipB (0 for single clips)
	float EffectiveDuration(const State& state, const Scene& scene) const;
	void ComputeStatePose(const Scene& scene, std::vector<NodePose>& out);
	void RefreshPose(const Scene& scene);
	void FireMarkers(const State& state, const Scene& scene, double from, double to);
	void SwitchTo(int state);
	void BeginCrossfade(float duration, FadeEasing easing, const std::vector<NodePose>* fromPose);
	int EnsureClipState(const Scene& scene, int clipIndex, bool loop);

	std::string m_graphJson;
	const Scene* m_bound = nullptr;
	bool m_graphParsed = false;
	bool m_graphValid = false;
	std::string m_lastError;

	std::vector<Param> m_params;
	std::vector<State> m_states;
	std::vector<Transition> m_transitions;
	int m_defaultState = -1;

	int m_state = -1;
	double m_phase = 0.0;         // unwrapped playhead in cycles of the current state
	bool m_finished = false;      // a non-looping state reached its end

	std::vector<NodePose> m_bindPose;
	std::vector<NodePose> m_pose;             // composite pose: current state, blended with the fading source
	std::vector<NodePose> m_scratchA;         // scratch for state sampling
	std::vector<NodePose> m_scratchB;
	std::vector<NodePose> m_fromPose;         // source of the active crossfade
	std::vector<glm::mat4> m_localMatrices;
	bool m_hasPose = false;
	bool m_fromValid = false;
	float m_fadeDuration = 0.0f;
	float m_fadeElapsed = 0.0f;
	FadeEasing m_fadeEasing = FadeEasing::Linear;

	int m_primaryClip = -1;
	float m_primaryClipTime = 0.0f;

	std::vector<FiredEvent> m_events;
};
