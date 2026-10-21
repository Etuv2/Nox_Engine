#include "AnimatorRuntime.h"

#include "Animation.h"
#include "Scene.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

int ClipIndexByName(const Scene& scene, const std::string& name)
{
	for (size_t i = 0; i < scene.animations.size(); ++i) {
		if (scene.animations[i].name == name) return static_cast<int>(i);
	}
	return -1;
}

// Clip duration in seconds; 0 for a clip without keyframes.
float ClipDuration(const Scene& scene, int clip)
{
	if (clip < 0 || clip >= static_cast<int>(scene.animations.size())) return 0.0f;
	return scene.animations[static_cast<size_t>(clip)].duration;
}

bool ReadOp(const std::string& text, AnimatorRuntime::CompareOp& op)
{
	if (text == ">") op = AnimatorRuntime::CompareOp::Greater;
	else if (text == "<") op = AnimatorRuntime::CompareOp::Less;
	else if (text == ">=") op = AnimatorRuntime::CompareOp::GreaterEqual;
	else if (text == "<=") op = AnimatorRuntime::CompareOp::LessEqual;
	else if (text == "==") op = AnimatorRuntime::CompareOp::Equal;
	else if (text == "!=") op = AnimatorRuntime::CompareOp::NotEqual;
	else return false;
	return true;
}

} // namespace

void AnimatorRuntime::SetGraphJson(const std::string& json)
{
	if (json == m_graphJson) return;
	m_graphJson = json;
	m_graphParsed = false;
	m_graphValid = false;
	m_lastError.clear();
	m_params.clear();
	m_states.clear();
	m_transitions.clear();
	m_defaultState = -1;
	Reset();
}

bool AnimatorRuntime::Bind(const Scene& scene)
{
	if (m_bound != &scene) {
		m_bound = &scene;
		m_bindPose = MakeBindPose(scene);
		m_params.clear();
		m_states.clear();
		m_transitions.clear();
		m_defaultState = -1;
		m_graphParsed = false;
		m_graphValid = false;
		m_lastError.clear();
		Reset();
	}
	if (!m_graphParsed) {
		m_graphParsed = true;
		if (HasGraph()) {
			std::string error;
			m_graphValid = ParseGraph(scene, error);
			if (!m_graphValid) m_lastError = error;
		}
	}
	return true;
}

bool AnimatorRuntime::ParseGraph(const Scene& scene, std::string& error)
{
	auto fail = [&error](const std::string& message) {
		error = message;
		return false;
	};

	const nlohmann::json doc = nlohmann::json::parse(m_graphJson, nullptr, false);
	if (doc.is_discarded() || !doc.is_object()) return fail("graph is not a JSON object");

	// Parameters: name -> "float" | "bool" | "trigger".
	if (doc.contains("params")) {
		const nlohmann::json& params = doc.at("params");
		if (!params.is_object()) return fail("'params' must be an object");
		for (auto it = params.begin(); it != params.end(); ++it) {
			Param p;
			p.name = it.key();
			const std::string type = it.value().is_string() ? it.value().get<std::string>() : std::string();
			if (type == "float") p.type = ParamType::Float;
			else if (type == "bool") p.type = ParamType::Bool;
			else if (type == "trigger") p.type = ParamType::Trigger;
			else return fail("parameter '" + p.name + "' must be float, bool or trigger");
			m_params.push_back(p);
		}
	}

	// States: a single clip, or a 1D blend space over float parameter.
	if (!doc.contains("states") || !doc.at("states").is_array() || doc.at("states").empty()) {
		return fail("graph needs a non-empty 'states' array");
	}
	for (const nlohmann::json& js : doc.at("states")) {
		if (!js.is_object() || !js.contains("name") || !js.at("name").is_string()) return fail("every state needs a name");
		State s;
		s.name = js.at("name").get<std::string>();
		if (FindState(s.name) >= 0) return fail("duplicate state '" + s.name + "'");
		if (js.contains("blend")) {
			const nlohmann::json& blend = js.at("blend");
			if (!blend.is_object() || !blend.contains("param") || !blend.at("param").is_string()) {
				return fail("state '" + s.name + "': blend needs a 'param'");
			}
			s.blendParam = FindParam(blend.at("param").get<std::string>());
			if (s.blendParam < 0 || m_params[static_cast<size_t>(s.blendParam)].type != ParamType::Float) {
				return fail("state '" + s.name + "': blend param must be a declared float");
			}
			if (!blend.contains("points") || !blend.at("points").is_array() || blend.at("points").empty()) {
				return fail("state '" + s.name + "': blend needs a non-empty 'points' array");
			}
			for (const nlohmann::json& jp : blend.at("points")) {
				if (!jp.is_object() || !jp.contains("clip") || !jp.at("clip").is_string() || !jp.contains("at") || !jp.at("at").is_number()) {
					return fail("state '" + s.name + "': each blend point needs 'clip' and 'at'");
				}
				BlendPoint point;
				point.clip = ClipIndexByName(scene, jp.at("clip").get<std::string>());
				if (point.clip < 0) return fail("state '" + s.name + "': unknown clip '" + jp.at("clip").get<std::string>() + "'");
				point.threshold = jp.at("at").get<float>();
				s.blend.push_back(point);
			}
			std::stable_sort(s.blend.begin(), s.blend.end(),
				[](const BlendPoint& a, const BlendPoint& b) { return a.threshold < b.threshold; });
		}
		else {
			if (!js.contains("clip") || !js.at("clip").is_string()) return fail("state '" + s.name + "' needs a 'clip'");
			s.clip = ClipIndexByName(scene, js.at("clip").get<std::string>());
			if (s.clip < 0) return fail("state '" + s.name + "': unknown clip '" + js.at("clip").get<std::string>() + "'");
		}
		if (js.contains("speed") && js.at("speed").is_number()) s.speed = js.at("speed").get<float>();
		if (js.contains("loop") && js.at("loop").is_boolean()) s.loop = js.at("loop").get<bool>();
		if (js.contains("events")) {
			if (!js.at("events").is_array()) return fail("state '" + s.name + "': 'events' must be an array");
			for (const nlohmann::json& je : js.at("events")) {
				if (!je.is_object() || !je.contains("name") || !je.at("name").is_string() || !je.contains("t") || !je.at("t").is_number()) {
					return fail("state '" + s.name + "': each event needs 'name' and 't'");
				}
				Event e;
				e.name = je.at("name").get<std::string>();
				e.phase = je.at("t").get<float>();
				if (!(e.phase >= 0.0f && e.phase < 1.0f)) return fail("state '" + s.name + "': event time must be in [0, 1)");
				s.events.push_back(e);
			}
		}
		m_states.push_back(std::move(s));
	}

	if (doc.contains("default")) {
		if (!doc.at("default").is_string()) return fail("'default' must be a state name");
		m_defaultState = FindState(doc.at("default").get<std::string>());
		if (m_defaultState < 0) return fail("'default' names an unknown state");
	}
	else {
		m_defaultState = 0;
	}

	// Transitions: from a state (or "*" for any state) to a state, crossfade, optional exit time, conditions.
	if (doc.contains("transitions")) {
		if (!doc.at("transitions").is_array()) return fail("'transitions' must be an array");
		for (const nlohmann::json& jt : doc.at("transitions")) {
			if (!jt.is_object() || !jt.contains("from") || !jt.at("from").is_string() || !jt.contains("to") || !jt.at("to").is_string()) {
				return fail("every transition needs 'from' and 'to'");
			}
			Transition t;
			const std::string from = jt.at("from").get<std::string>();
			if (from == "*") {
				t.from = kAnyState;
			}
			else {
				t.from = FindState(from);
				if (t.from < 0) return fail("transition from unknown state '" + from + "'");
			}
			t.to = FindState(jt.at("to").get<std::string>());
			if (t.to < 0) return fail("transition to unknown state '" + jt.at("to").get<std::string>() + "'");
			if (jt.contains("duration") && jt.at("duration").is_number()) t.duration = std::max(0.0f, jt.at("duration").get<float>());
			if (jt.contains("ease") && jt.at("ease").is_string() && jt.at("ease").get<std::string>() == "smoothstep") {
				t.easing = FadeEasing::Smoothstep;
			}
			if (jt.contains("exit_time") && jt.at("exit_time").is_number()) t.exitTime = jt.at("exit_time").get<float>();
			if (jt.contains("when")) {
				if (!jt.at("when").is_array()) return fail("transition 'when' must be an array");
				for (const nlohmann::json& jc : jt.at("when")) {
					if (!jc.is_object() || !jc.contains("param") || !jc.at("param").is_string()) return fail("each condition needs a 'param'");
					Condition c;
					c.param = FindParam(jc.at("param").get<std::string>());
					if (c.param < 0) return fail("condition uses undeclared parameter '" + jc.at("param").get<std::string>() + "'");
					const ParamType type = m_params[static_cast<size_t>(c.param)].type;
					if (type == ParamType::Trigger) {
						c.op = CompareOp::Set;
					}
					else if (type == ParamType::Bool) {
						bool wanted = true;
						if (jc.contains("value")) {
							if (jc.at("value").is_boolean()) wanted = jc.at("value").get<bool>();
							else if (jc.at("value").is_number()) wanted = jc.at("value").get<float>() != 0.0f;
						}
						c.op = CompareOp::Equal;
						c.value = wanted ? 1.0f : 0.0f;
					}
					else {
						if (!jc.contains("op") || !jc.at("op").is_string() || !ReadOp(jc.at("op").get<std::string>(), c.op)) {
							return fail("float condition on '" + m_params[static_cast<size_t>(c.param)].name + "' needs op >, <, >=, <=, == or !=");
						}
						if (!jc.contains("value") || !jc.at("value").is_number()) return fail("float condition needs a numeric 'value'");
						c.value = jc.at("value").get<float>();
					}
					t.when.push_back(c);
				}
			}
			m_transitions.push_back(std::move(t));
		}
	}
	return true;
}

int AnimatorRuntime::FindParam(const std::string& name) const
{
	for (size_t i = 0; i < m_params.size(); ++i) {
		if (m_params[i].name == name) return static_cast<int>(i);
	}
	return -1;
}

int AnimatorRuntime::FindState(const std::string& name) const
{
	for (size_t i = 0; i < m_states.size(); ++i) {
		if (m_states[i].name == name) return static_cast<int>(i);
	}
	return -1;
}

void AnimatorRuntime::StartGraph()
{
	if (!GraphValid() || !m_bound) return;
	SwitchTo(m_defaultState);
	m_fromValid = false;
	m_fadeDuration = 0.0f;
	RefreshPose(*m_bound);
}

int AnimatorRuntime::EnsureClipState(const Scene& scene, int clipIndex, bool loop)
{
	const std::string name = "clip:" + std::to_string(clipIndex);
	int index = FindState(name);
	if (index < 0) {
		State s;
		s.name = name;
		s.clip = clipIndex;

		m_states.push_back(s);
		index = static_cast<int>(m_states.size()) - 1;
	}
	m_states[static_cast<size_t>(index)].loop = loop;
	m_states[static_cast<size_t>(index)].speed = 1.0f;
	return index;
}

void AnimatorRuntime::PlayClip(const Scene& scene, int clipIndex, bool loop, float blendTime, const std::vector<NodePose>* fromPose)
{
	Bind(scene);
	if (clipIndex < 0 || clipIndex >= static_cast<int>(scene.animations.size())) return;
	const int state = EnsureClipState(scene, clipIndex, loop);
	const std::vector<NodePose>* from = fromPose;
	if (!from && m_hasPose) from = &m_pose;
	BeginCrossfade(blendTime, FadeEasing::Linear, from);
	SwitchTo(state);
	RefreshPose(scene);
}

void AnimatorRuntime::Reset()
{
	m_state = -1;
	m_phase = 0.0;
	m_finished = false;
	m_hasPose = false;
	m_fromValid = false;
	m_fadeDuration = 0.0f;
	m_fadeElapsed = 0.0f;
	m_primaryClip = -1;
	m_primaryClipTime = 0.0f;
	m_events.clear();
}

void AnimatorRuntime::SwitchTo(int state)
{
	m_state = state;
	m_phase = 0.0;
	m_finished = false;
}

void AnimatorRuntime::BeginCrossfade(float duration, FadeEasing easing, const std::vector<NodePose>* fromPose)
{
	if (duration > 0.0f && fromPose) {
		m_fromPose = *fromPose;
		m_fromValid = true;
		m_fadeDuration = duration;
		m_fadeElapsed = 0.0f;
		m_fadeEasing = easing;
	}
	else {
		m_fromValid = false;
		m_fadeDuration = 0.0f;
		m_fadeElapsed = 0.0f;
	}
}

float AnimatorRuntime::Phase() const
{
	if (m_state < 0) return 0.0f;
	if (m_states[static_cast<size_t>(m_state)].loop) {
		return static_cast<float>(m_phase - std::floor(m_phase));
	}
	return static_cast<float>(std::min(std::max(m_phase, 0.0), 1.0));
}

bool AnimatorRuntime::EvaluateCondition(const Condition& c) const
{
	const Param& p = m_params[static_cast<size_t>(c.param)];
	switch (c.op) {
	case CompareOp::Greater: return p.value > c.value;
	case CompareOp::Less: return p.value < c.value;
	case CompareOp::GreaterEqual: return p.value >= c.value;
	case CompareOp::LessEqual: return p.value <= c.value;
	case CompareOp::Equal: return p.value == c.value;
	case CompareOp::NotEqual: return p.value != c.value;
	case CompareOp::Set: return p.value != 0.0f;
	}
	return false;
}

float AnimatorRuntime::BlendWeightOf(const State& state, int& clipA, int& clipB) const
{
	if (state.blend.empty()) {
		clipA = clipB = state.clip;
		return 0.0f;
	}
	const float v = m_params[static_cast<size_t>(state.blendParam)].value;
	const auto& pts = state.blend;
	if (v <= pts.front().threshold) {
		clipA = clipB = pts.front().clip;
		return 0.0f;
	}
	if (v >= pts.back().threshold) {
		clipA = clipB = pts.back().clip;
		return 0.0f;
	}
	for (size_t i = 0; i + 1 < pts.size(); ++i) {
		const float a = pts[i].threshold;
		const float b = pts[i + 1].threshold;
		if (v >= a && v < b) {
			clipA = pts[i].clip;
			clipB = pts[i + 1].clip;
			return (b > a) ? (v - a) / (b - a) : 0.0f;
		}
	}
	clipA = clipB = pts.back().clip;
	return 0.0f;
}

float AnimatorRuntime::EffectiveDuration(const State& state, const Scene& scene) const
{
	int a = -1, b = -1;
	const float t = BlendWeightOf(state, a, b);
	const float d = (1.0f - t) * ClipDuration(scene, a) + t * ClipDuration(scene, b);
	// A static clip still advances the playhead, so its state can end and transition.
	return d > 1e-6f ? d : 1.0f;
}

void AnimatorRuntime::ComputeStatePose(const Scene& scene, std::vector<NodePose>& out)
{
	const State& s = m_states[static_cast<size_t>(m_state)];
	const float n = Phase();
	int a = -1, b = -1;
	const float t = BlendWeightOf(s, a, b);

	// Phase-synchronised: every clip of a blend samples the same normalised time, so feet keep their cycle.
	out = m_bindPose;
	const Animation& clipA = scene.animations[static_cast<size_t>(a)];
	SampleClipPose(clipA, clipA.startTime + n * clipA.duration, out);
	if (b != a && t > 0.0f) {
		const Animation& clipB = scene.animations[static_cast<size_t>(b)];
		m_scratchB = m_bindPose;
		SampleClipPose(clipB, clipB.startTime + n * clipB.duration, m_scratchB);
		BlendPoseInto(out, m_scratchB, t);
	}
}

void AnimatorRuntime::RefreshPose(const Scene& scene)
{
	ComputeStatePose(scene, m_scratchA);
	if (m_fromValid && m_fadeElapsed < m_fadeDuration) {
		const float f = ApplyFadeEasing(m_fadeElapsed / m_fadeDuration, m_fadeEasing);
		m_pose = m_fromPose;
		BlendPoseInto(m_pose, m_scratchA, f);
	}
	else {
		m_fromValid = false;
		std::swap(m_pose, m_scratchA);
	}
	m_hasPose = true;
	PoseToLocalMatrices(m_pose, m_localMatrices);

	int a = -1, b = -1;
	const float t = BlendWeightOf(m_states[static_cast<size_t>(m_state)], a, b);
	m_primaryClip = (t < 0.5f) ? a : b;
	const Animation& clip = scene.animations[static_cast<size_t>(m_primaryClip)];
	m_primaryClipTime = clip.startTime + Phase() * clip.duration;
}

void AnimatorRuntime::FireMarkers(const State& state, const Scene& scene, double from, double to)
{
	if (state.events.empty() || !(to > from)) return;
	int a = -1, b = -1;
	const float t = BlendWeightOf(state, a, b);
	const int primary = (t < 0.5f) ? a : b;
	const std::string clipName = scene.animations[static_cast<size_t>(primary)].name;

	if (!state.loop) {
		// Non-looping: markers in (from, to] within the single cycle.
		for (const Event& e : state.events) {
			if (e.phase > from && e.phase <= to) {
				m_events.push_back(FiredEvent{ e.name, clipName, e.phase });
			}
		}
		return;
	}
	// Looping: a marker fires once per cycle, when the unwrapped playhead crosses cycle k + phase.
	for (long long k = static_cast<long long>(std::floor(from)); k <= static_cast<long long>(std::floor(to)); ++k) {
		for (const Event& e : state.events) {
			const double crossing = static_cast<double>(k) + e.phase;
			if (crossing > from && crossing <= to) {
				m_events.push_back(FiredEvent{ e.name, clipName, e.phase });
			}
		}
	}
}

void AnimatorRuntime::Update(float dt, const Scene& scene)
{
	Bind(scene);
	if (m_state < 0) return;

	if (m_fromValid) m_fadeElapsed += dt;

	State& s = m_states[static_cast<size_t>(m_state)];
	if (!m_finished) {
		const double cyclesPerSecond = static_cast<double>(s.speed) / static_cast<double>(EffectiveDuration(s, scene));
		const double from = m_phase;
		double to = from + static_cast<double>(dt) * cyclesPerSecond;
		if (!s.loop && to >= 1.0) {
			to = 1.0;
			m_finished = true;
		}
		m_phase = to;
		FireMarkers(s, scene, from, to);
	}

	RefreshPose(scene);

	if (!m_graphValid) return;
	// The first transition whose source, exit time and conditions all hold takes effect this step.
	for (const Transition& t : m_transitions) {
		if (t.to == m_state) continue;
		if (t.from != kAnyState && t.from != m_state) continue;
		if (t.exitTime >= 0.0f && Phase() < t.exitTime) continue;
		bool holds = true;
		for (const Condition& c : t.when) {
			if (!EvaluateCondition(c)) {
				holds = false;
				break;
			}
		}
		if (!holds) continue;

		for (const Condition& c : t.when) {
			Param& p = m_params[static_cast<size_t>(c.param)];
			if (p.type == ParamType::Trigger) p.value = 0.0f;   // consumed by the transition that uses it
		}
		// Crossfade from the pose on screen now, so interrupting a fade blends from where it is.
		BeginCrossfade(t.duration, t.easing, m_hasPose ? &m_pose : nullptr);
		SwitchTo(t.to);
		RefreshPose(scene);
		break;
	}
}

bool AnimatorRuntime::SetFloat(const std::string& name, float value)
{
	const int i = FindParam(name);
	if (i < 0 || m_params[static_cast<size_t>(i)].type != ParamType::Float) return false;
	m_params[static_cast<size_t>(i)].value = value;
	return true;
}

bool AnimatorRuntime::SetBool(const std::string& name, bool value)
{
	const int i = FindParam(name);
	if (i < 0 || m_params[static_cast<size_t>(i)].type != ParamType::Bool) return false;
	m_params[static_cast<size_t>(i)].value = value ? 1.0f : 0.0f;
	return true;
}

bool AnimatorRuntime::SetTrigger(const std::string& name)
{
	const int i = FindParam(name);
	if (i < 0 || m_params[static_cast<size_t>(i)].type != ParamType::Trigger) return false;
	m_params[static_cast<size_t>(i)].value = 1.0f;
	return true;
}

bool AnimatorRuntime::GetFloat(const std::string& name, float& out) const
{
	const int i = FindParam(name);
	if (i < 0 || m_params[static_cast<size_t>(i)].type != ParamType::Float) return false;
	out = m_params[static_cast<size_t>(i)].value;
	return true;
}

std::string AnimatorRuntime::CurrentStateName() const
{
	return (m_state < 0) ? std::string() : m_states[static_cast<size_t>(m_state)].name;
}

const glm::mat4& AnimatorRuntime::LocalMatrix(int node) const
{
	static const glm::mat4 kIdentity(1.0f);
	if (!m_hasPose || node < 0 || node >= static_cast<int>(m_localMatrices.size())) return kIdentity;
	return m_localMatrices[static_cast<size_t>(node)];
}

std::vector<AnimatorRuntime::FiredEvent> AnimatorRuntime::TakeEvents()
{
	std::vector<FiredEvent> out;
	out.swap(m_events);
	return out;
}
