// Animation blending, crossfades, the Animator state machine and marker events (AnimationPose, AnimatorRuntime).
// Headless: scenes are built in code, no model files or GPU.
#include "TestFramework.h"

#include "Animation.h"
#include "AnimationPose.h"
#include "AnimatorRuntime.h"
#include "Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

// Clip with a translation channel on node 0 from `from` to `to` over `duration` seconds, and a constant rotation.
Animation MakeClip(const std::string& name, float duration, glm::vec3 from, glm::vec3 to,
	glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
{
	Animation clip;
	clip.name = name;
	clip.isLooping = true;

	Animation::Channel translation;
	translation.type = Animation::ChannelType::TRANSLATION;
	translation.path = "translation";
	translation.targetNode = 0;
	translation.keyframes = { 0.0f, duration };
	translation.values = { glm::vec4(from, 0.0f), glm::vec4(to, 0.0f) };
	clip.channels.push_back(translation);

	Animation::Channel spin;
	spin.type = Animation::ChannelType::ROTATION;
	spin.path = "rotation";
	spin.targetNode = 0;
	spin.keyframes = { 0.0f, duration };
	const glm::vec4 q(rotation.x, rotation.y, rotation.z, rotation.w);   // glTF order: x, y, z, w
	spin.values = { q, q };
	clip.channels.push_back(spin);

	clip.UpdateDuration();
	return clip;
}

// Two-node rig: node 0 is the root (identity), node 1 hangs off it one unit up.
std::shared_ptr<Scene> MakeRig(const std::vector<Animation>& clips)
{
	auto scene = std::make_shared<Scene>();
	scene->nodes.resize(2);
	scene->nodes[0].parent = -1;
	scene->nodes[0].localTransform = glm::mat4(1.0f);
	scene->nodes[1].parent = 0;
	scene->nodes[1].localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	scene->animations = clips;
	return scene;
}

bool NearMat(const glm::mat4& a, const glm::mat4& b, float eps)
{
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (std::fabs(a[c][r] - b[c][r]) > eps) return false;
		}
	}
	return true;
}

glm::mat4 TRS(glm::vec3 t, glm::quat r)
{
	return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r);
}

const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);
const glm::vec3 kAxisY(0.0f, 1.0f, 0.0f);

} // namespace

// ---- Blending math ------------------------------------------------------------------------------------

NOX_TEST(AnimationBlend, LinearCrossfadeMidpointIsFiftyFifty)
{
	const glm::quat turn = glm::angleAxis(glm::radians(90.0f), kAxisY);
	auto scene = MakeRig({ MakeClip("a", 1.0f, { 0, 0, 0 }, { 0, 0, 0 }),
		MakeClip("b", 1.0f, { 2, 0, 0 }, { 2, 0, 0 }, turn) });

	AnimatorRuntime anim;
	anim.Bind(*scene);
	anim.PlayClip(*scene, 0, true, 0.0f);
	anim.Update(0.0f, *scene);
	anim.PlayClip(*scene, 1, true, 0.2f);   // 0.2 s linear crossfade from clip "a" to clip "b"
	anim.Update(0.1f, *scene);              // t = 0.1 of 0.2 s

	NOX_CHECK(anim.IsFading());
	// The 50/50 blend of the two samples: translation (1,0,0) and rotation slerp(identity, turn, 0.5).
	const glm::mat4 expected = TRS(glm::vec3(1.0f, 0.0f, 0.0f), glm::slerp(kIdentity, turn, 0.5f));
	NOX_CHECK(NearMat(anim.LocalMatrix(0), expected, 1e-4f));

	// The fade ends at 0.2 s: the pose is then clip "b" alone.
	anim.Update(0.1f, *scene);
	NOX_CHECK(!anim.IsFading());
	NOX_CHECK(NearMat(anim.LocalMatrix(0), TRS(glm::vec3(2.0f, 0.0f, 0.0f), turn), 1e-4f));
}

NOX_TEST(AnimationBlend, QuaternionBlendTakesShortestArc)
{
	// +80 and -80 degrees about Y are 160 degrees apart the long way round; the short way passes through identity.
	// The second quaternion is stored in the opposite hemisphere (negated), which is the case blending must handle.
	const glm::quat plus80 = glm::angleAxis(glm::radians(80.0f), kAxisY);
	const glm::quat minus80Flipped = -glm::angleAxis(glm::radians(-80.0f), kAxisY);
	NOX_CHECK(glm::dot(plus80, minus80Flipped) < 0.0f);

	NodePose a{ glm::vec3(0.0f), plus80, glm::vec3(1.0f) };
	NodePose b{ glm::vec3(0.0f), minus80Flipped, glm::vec3(1.0f) };
	const NodePose mid = BlendNodePose(a, b, 0.5f);
	NOX_CHECK(glm::angle(mid.rotation) < 1e-4f);

	// Identity blended with the negated identity is identity, not zero.
	const NodePose same = BlendNodePose(NodePose{}, NodePose{ glm::vec3(0.0f), glm::quat(-1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) }, 0.5f);
	NOX_CHECK(glm::angle(same.rotation) < 1e-4f);
	NOX_CHECK(std::fabs(glm::length(same.rotation) - 1.0f) < 1e-5f);
}

NOX_TEST(AnimationBlend, InterruptedCrossfadeStartsFromCurrentPose)
{
	const glm::quat turn = glm::angleAxis(glm::radians(60.0f), kAxisY);
	auto scene = MakeRig({ MakeClip("a", 1.0f, { 0, 0, 0 }, { 0, 0, 0 }),
		MakeClip("b", 1.0f, { 4, 0, 0 }, { 4, 0, 0 }, turn),
		MakeClip("c", 1.0f, { -2, 0, 0 }, { -2, 0, 0 }) });

	AnimatorRuntime anim;
	anim.Bind(*scene);
	anim.PlayClip(*scene, 0, true, 0.0f);
	anim.Update(0.0f, *scene);
	anim.PlayClip(*scene, 1, true, 0.2f);
	anim.Update(0.1f, *scene);                 // halfway from "a" to "b"
	const glm::mat4 onScreen = anim.LocalMatrix(0);

	// Interrupt: crossfade to "c" starts from exactly what is on screen, so the next frame does not pop.
	anim.PlayClip(*scene, 2, true, 0.2f);
	anim.Update(0.0f, *scene);
	NOX_CHECK(NearMat(anim.LocalMatrix(0), onScreen, 1e-5f));

	// And it reaches "c" when the new fade is over.
	anim.Update(0.2f, *scene);
	NOX_CHECK(NearMat(anim.LocalMatrix(0), TRS(glm::vec3(-2.0f, 0.0f, 0.0f), kIdentity), 1e-4f));
}

// ---- Blend spaces -------------------------------------------------------------------------------------

NOX_TEST(AnimationBlend, OneDBlendSpaceIsPhaseSynchronised)
{
	// "walk" lasts 1 s and moves 0 -> 1; "run" lasts 2 s and moves 0 -> 4. Both share one normalised phase.
	auto scene = MakeRig({ MakeClip("walk", 1.0f, { 0, 0, 0 }, { 1, 0, 0 }),
		MakeClip("run", 2.0f, { 0, 0, 0 }, { 4, 0, 0 }) });

	AnimatorRuntime anim;
	anim.SetGraphJson(R"({
		"params": { "speed": "float" },
		"default": "move",
		"states": [ { "name": "move", "blend": { "param": "speed",
			"points": [ { "clip": "walk", "at": 0.0 }, { "clip": "run", "at": 1.0 } ] } } ]
	})");
	anim.Bind(*scene);
	NOX_REQUIRE(anim.GraphValid());
	NOX_CHECK(anim.SetFloat("speed", 0.5f));
	anim.StartGraph();

	// Effective cycle = 0.5 * 1 s + 0.5 * 2 s = 1.5 s, so 0.75 s is exactly half a cycle (phase 0.5).
	anim.Update(0.75f, *scene);
	NOX_CHECK(std::fabs(anim.Phase() - 0.5f) < 1e-6f);
	// walk at 0.5 s -> x = 0.5; run at 1.0 s -> x = 2.0; 50/50 -> x = 1.25.
	NOX_CHECK(NearMat(anim.LocalMatrix(0), TRS(glm::vec3(1.25f, 0.0f, 0.0f), kIdentity), 1e-4f));
}

// ---- State machine ------------------------------------------------------------------------------------

NOX_TEST(AnimationBlend, StateMachineFloatParameterDrivesThreeStates)
{
	auto scene = MakeRig({ MakeClip("idle", 1.0f, { 0, 0, 0 }, { 0, 0, 0 }),
		MakeClip("walk", 1.0f, { 1, 0, 0 }, { 1, 0, 0 }),
		MakeClip("run", 1.0f, { 2, 0, 0 }, { 2, 0, 0 }) });

	AnimatorRuntime anim;
	anim.SetGraphJson(R"({
		"params": { "speed": "float" },
		"default": "idle",
		"states": [ { "name": "idle", "clip": "idle" }, { "name": "walk", "clip": "walk" }, { "name": "run", "clip": "run" } ],
		"transitions": [
			{ "from": "idle", "to": "walk", "when": [ { "param": "speed", "op": ">", "value": 1 } ] },
			{ "from": "walk", "to": "run",  "when": [ { "param": "speed", "op": ">", "value": 5 } ], "duration": 0.25 },
			{ "from": "run",  "to": "walk", "when": [ { "param": "speed", "op": "<", "value": 5 } ] },
			{ "from": "walk", "to": "idle", "when": [ { "param": "speed", "op": "<", "value": 1 } ] }
		]
	})");
	anim.Bind(*scene);
	NOX_REQUIRE(anim.GraphValid());
	anim.StartGraph();
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("idle"));

	// One transition per update: each step moves at most one edge of the graph.
	anim.SetFloat("speed", 2.0f);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("walk"));

	anim.SetFloat("speed", 6.0f);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("run"));

	anim.SetFloat("speed", 3.0f);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("walk"));

	anim.SetFloat("speed", 0.5f);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("idle"));

	// Type checks: a float parameter is not a bool or a trigger.
	NOX_CHECK(!anim.SetBool("speed", true));
	NOX_CHECK(!anim.SetTrigger("speed"));
	NOX_CHECK(!anim.SetFloat("missing", 1.0f));
}

NOX_TEST(AnimationBlend, TriggerIsConsumedOnce)
{
	auto scene = MakeRig({ MakeClip("idle", 1.0f, { 0, 0, 0 }, { 0, 0, 0 }),
		MakeClip("jump", 1.0f, { 0, 2, 0 }, { 0, 2, 0 }) });

	AnimatorRuntime anim;
	anim.SetGraphJson(R"({
		"params": { "jump": "trigger", "grounded": "bool" },
		"default": "idle",
		"states": [ { "name": "idle", "clip": "idle" }, { "name": "jump", "clip": "jump" } ],
		"transitions": [
			{ "from": "idle", "to": "jump", "when": [ { "param": "jump" } ] },
			{ "from": "jump", "to": "idle", "when": [ { "param": "grounded", "value": true } ] }
		]
	})");
	anim.Bind(*scene);
	NOX_REQUIRE(anim.GraphValid());
	anim.StartGraph();

	NOX_CHECK(anim.SetTrigger("jump"));
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("jump"));

	// The trigger was consumed by that transition: nothing else re-fires it.
	anim.Update(0.0f, *scene);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("jump"));

	// Bool condition returns to idle, and the consumed trigger does not start another jump.
	NOX_CHECK(anim.SetBool("grounded", true));
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("idle"));
	NOX_CHECK(anim.SetBool("grounded", false));
	anim.Update(0.0f, *scene);
	anim.Update(0.0f, *scene);
	NOX_CHECK_EQ(anim.CurrentStateName(), std::string("idle"));
}

// ---- Events -------------------------------------------------------------------------------------------

NOX_TEST(AnimationBlend, MarkersFireOncePerLoopAtTheirTime)
{
	auto scene = MakeRig({ MakeClip("walk", 1.0f, { 0, 0, 0 }, { 1, 0, 0 }) });

	AnimatorRuntime anim;
	anim.SetGraphJson(R"({
		"default": "walk",
		"states": [ { "name": "walk", "clip": "walk", "loop": true,
			"events": [ { "name": "left", "t": 0.25 }, { "name": "right", "t": 0.75 } ] } ]
	})");
	anim.Bind(*scene);
	NOX_REQUIRE(anim.GraphValid());
	anim.StartGraph();

	// dt = 1/8 s on a 1 s clip: 20 steps are 2.5 cycles. Markers are crossed at steps 2, 6, 10, 14, 18.
	const float dt = 0.125f;
	const int expectedStep[5] = { 2, 6, 10, 14, 18 };
	const std::string expectedName[5] = { "left", "right", "left", "right", "left" };
	const float expectedPhase[5] = { 0.25f, 0.75f, 0.25f, 0.75f, 0.25f };
	std::vector<int> stepsSeen;
	for (int step = 1; step <= 20; ++step) {
		anim.Update(dt, *scene);
		for (const AnimatorRuntime::FiredEvent& e : anim.TakeEvents()) {
			NOX_CHECK_EQ(e.clip, std::string("walk"));
			const size_t index = stepsSeen.size();
			stepsSeen.push_back(step);
			if (index < 5) {
				NOX_CHECK_EQ(step, expectedStep[index]);
				NOX_CHECK_EQ(e.name, expectedName[index]);
				NOX_CHECK(std::fabs(e.phase - expectedPhase[index]) < 1e-6f);
			}
		}
	}
	NOX_CHECK_EQ(stepsSeen.size(), size_t(5));
	// Taking the events drains them.
	NOX_CHECK(anim.TakeEvents().empty());
}

// ---- Graph errors -------------------------------------------------------------------------------------

NOX_TEST(AnimationBlend, GraphWithUnknownClipIsRejectedWithAReason)
{
	auto scene = MakeRig({ MakeClip("idle", 1.0f, { 0, 0, 0 }, { 0, 0, 0 }) });

	AnimatorRuntime anim;
	anim.SetGraphJson(R"({ "default": "idle", "states": [ { "name": "idle", "clip": "does_not_exist" } ] })");
	anim.Bind(*scene);
	NOX_CHECK(!anim.GraphValid());
	NOX_CHECK(anim.LastError().find("does_not_exist") != std::string::npos);

	// Nothing plays from an invalid graph.
	anim.StartGraph();
	anim.Update(0.1f, *scene);
	NOX_CHECK(anim.CurrentStateName().empty());
	NOX_CHECK(!anim.HasPose());

	// Malformed JSON is reported too.
	anim.SetGraphJson("{ not json");
	anim.Bind(*scene);
	NOX_CHECK(!anim.GraphValid());
}
