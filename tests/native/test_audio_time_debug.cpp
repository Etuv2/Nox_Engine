// Audio buses and listener selection, the global time scale, debug draw lifetimes and the headless Lua bindings for
// audio, debug, time and input rebinding. Everything here runs without an audio device or a window: the mixer and
// listener helpers are pure, the Lua functions are installed on a bare state and must fail softly (nil plus an error)
// when no device or action map exists.
#include "TestFramework.h"

#include "AudioNode.h"
#include "SceneNode.h"
#include "TimeScale.h"
#include "audio/AudioListener.h"
#include "audio/AudioMixer.h"
#include "audio/ProceduralVehicleAudio.h"
#include "diagnostics/DebugDraw.h"
#include "reflect/Reflection.h"
#include "scripting/LuaAudio.h"
#include "scripting/LuaDebug.h"
#include "scripting/LuaInput.h"
#include "scripting/LuaInputBindings.h"
#include "scripting/LuaTime.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace nox::reflect;

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Restores the process-wide mixer and time scale when a test ends.
struct GlobalsGuard {
	GlobalsGuard() { AudioMixer::Get().Reset(); TimeScale::Reset(); DebugDraw::Clear(); AudioListenerPublish({}); }
	~GlobalsGuard() { AudioMixer::Get().Reset(); TimeScale::Reset(); DebugDraw::Clear(); AudioListenerPublish({}); }
};

// Bare Lua state with the audio, debug, time and input-control installers (no world, no device).
struct LuaHarness {
	lua_State* L;
	LuaHarness() : L(luaL_newstate()) {
		luaL_openlibs(L);
		NoxLua::InstallAudioApi(L);
		NoxLua::InstallDebugApi(L);
		lua_newtable(L);
		NoxLua::InstallTimeApi(L, lua_gettop(L));
		lua_setglobal(L, "ctx");
		NoxLua::InstallInputBindingsApi(L, nullptr);
	}
	~LuaHarness() { lua_close(L); }
	// Evaluates a Lua expression; false when it errors or is falsy.
	bool Expect(const std::string& expr) {
		const std::string chunk = "return (" + expr + ")";
		if (luaL_dostring(L, chunk.c_str()) != LUA_OK) {
			lua_pop(L, 1);
			return false;
		}
		const bool ok = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		return ok;
	}
	// Runs a statement block; false when it raises.
	bool Run(const std::string& code) {
		if (luaL_dostring(L, code.c_str()) != LUA_OK) {
			lua_pop(L, 1);
			return false;
		}
		return true;
	}
};

} // namespace

// ---- Mixer -------------------------------------------------------------------------------------------------

NOX_TEST(AudioTimeDebug, MixerGainIsSourceTimesBusTimesMaster) {
	NOX_CHECK(Near(AudioMixer::ComputeGain(1.0f, 1.0f, false, 1.0f, false), 1.0f));
	NOX_CHECK(Near(AudioMixer::ComputeGain(0.5f, 0.5f, false, 1.0f, false), 0.25f));
	NOX_CHECK(Near(AudioMixer::ComputeGain(0.8f, 0.5f, false, 0.5f, false), 0.2f));
	// Muted bus or muted master is silent whatever the volumes are.
	NOX_CHECK(AudioMixer::ComputeGain(1.0f, 1.0f, true, 1.0f, false) == 0.0f);
	NOX_CHECK(AudioMixer::ComputeGain(1.0f, 1.0f, false, 1.0f, true) == 0.0f);
	// Out-of-range and non-finite inputs are clamped or silent.
	NOX_CHECK(Near(AudioMixer::ComputeGain(2.0f, 1.0f, false, 1.0f, false), 1.0f));
	NOX_CHECK(Near(AudioMixer::ComputeGain(-1.0f, 1.0f, false, 1.0f, false), 0.0f));
	NOX_CHECK(AudioMixer::ComputeGain(std::numeric_limits<float>::quiet_NaN(), 1.0f, false, 1.0f, false) == 0.0f);
	NOX_CHECK(AudioMixer::ComputeGain(1.0f, 1.0f, false, std::numeric_limits<float>::infinity(), false) == 0.0f);
}

NOX_TEST(AudioTimeDebug, DecibelHelpersMatchLinearGain) {
	NOX_CHECK(Near(AudioMixer::DbToLinear(0.0f), 1.0f));
	NOX_CHECK(Near(AudioMixer::DbToLinear(-6.0206f), 0.5f, 1e-3f));
	NOX_CHECK(AudioMixer::DbToLinear(-80.0f) == 0.0f);
	NOX_CHECK(AudioMixer::DbToLinear(-200.0f) == 0.0f);
	NOX_CHECK(AudioMixer::DbToLinear(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
	NOX_CHECK(Near(AudioMixer::LinearToDb(1.0f), 0.0f));
	NOX_CHECK(Near(AudioMixer::LinearToDb(0.5f), -6.0206f, 1e-3f));
	NOX_CHECK(AudioMixer::LinearToDb(0.0f) == -80.0f);
	NOX_CHECK(Near(AudioMixer::LinearToDb(AudioMixer::DbToLinear(-12.0f)), -12.0f, 1e-3f));
}

NOX_TEST(AudioTimeDebug, MixerHasEngineBusesAndDeclaresProjectBuses) {
	GlobalsGuard guard;
	AudioMixer& m = AudioMixer::Get();
	for (const char* name : { "master", "music", "sfx", "ui", "voice" }) NOX_CHECK(m.HasBus(name));
	NOX_CHECK(!m.AddBus("master"));          // already exists
	NOX_CHECK(!m.AddBus(""));                // empty
	NOX_CHECK(!m.AddBus("Bad Name"));        // characters outside [a-z0-9_-]
	NOX_CHECK(!m.AddBus(std::string(40, 'a')));
	NOX_CHECK(m.AddBus("boss_music"));
	NOX_CHECK(m.HasBus("boss_music"));
	NOX_CHECK(m.BusVolume("boss_music") == 1.0f);
	NOX_CHECK(m.BusNames().size() == 6);
}

NOX_TEST(AudioTimeDebug, BusVolumeIsClampedAndMuteSilencesTheBus) {
	GlobalsGuard guard;
	AudioMixer& m = AudioMixer::Get();
	NOX_CHECK(m.SetBusVolume("music", 0.5f));
	NOX_CHECK(Near(m.BusVolume("music"), 0.5f));
	NOX_CHECK(m.SetBusVolume("music", 7.0f));
	NOX_CHECK(m.BusVolume("music") == 1.0f);
	NOX_CHECK(!m.SetBusVolume("music", std::numeric_limits<float>::quiet_NaN()));
	NOX_CHECK(!m.SetBusVolume("nope", 0.5f));

	NOX_CHECK(Near(m.BusGain("music"), 1.0f));
	NOX_CHECK(m.SetBusMuted("music", true));
	NOX_CHECK(m.BusMuted("music"));
	NOX_CHECK(m.BusGain("music") == 0.0f);
	NOX_CHECK(m.BusGain("sfx") == 1.0f);        // other buses are untouched
	NOX_CHECK(m.SetBusMuted("music", false));
	NOX_CHECK(Near(m.BusGain("music"), 1.0f));
}

NOX_TEST(AudioTimeDebug, MasterBusScalesEveryBusAndMutesAll) {
	GlobalsGuard guard;
	AudioMixer& m = AudioMixer::Get();
	NOX_CHECK(m.SetBusVolume("master", 0.5f));
	NOX_CHECK(Near(m.BusGain("sfx"), 0.5f));
	NOX_CHECK(m.SetBusVolume("sfx", 0.5f));
	NOX_CHECK(Near(m.BusGain("sfx"), 0.25f));
	NOX_CHECK(m.SetBusMuted("master", true));
	NOX_CHECK(m.BusGain("sfx") == 0.0f);
	NOX_CHECK(m.BusGain("ui") == 0.0f);
	// Unknown buses fall back to the master only.
	NOX_CHECK(m.SetBusMuted("master", false));
	NOX_CHECK(Near(m.BusGain("undeclared"), 0.5f));
}

NOX_TEST(AudioTimeDebug, MixerVersionChangesOnlyOnRealChanges) {
	GlobalsGuard guard;
	AudioMixer& m = AudioMixer::Get();
	const uint64_t v0 = m.Version();
	NOX_CHECK(m.SetBusVolume("sfx", 1.0f));   // already 1: no change
	NOX_CHECK(m.Version() == v0);
	NOX_CHECK(m.SetBusVolume("sfx", 0.25f));
	NOX_CHECK(m.Version() != v0);
}

NOX_TEST(AudioTimeDebug, SourceBusDefaultsToSfxAndRoutesThroughTheMixer) {
	GlobalsGuard guard;
	RegisterBuiltinComponents();
	AudioNode node;
	NOX_CHECK_EQ(node.getBus(), std::string("sfx"));
	node.setBus("");
	NOX_CHECK_EQ(node.getBus(), std::string("sfx"));
	node.setBus("music");
	NOX_CHECK_EQ(node.getBus(), std::string("music"));

	node.setVolume(0.5f);
	NOX_CHECK(AudioMixer::Get().SetBusVolume("music", 0.5f));
	NOX_CHECK(Near(node.outputGain(), 0.25f));
	NOX_CHECK(AudioMixer::Get().SetBusMuted("music", true));
	NOX_CHECK(node.outputGain() == 0.0f);

	// The same property through the registry (scene JSON key "bus").
	NOX_CHECK(AudioMixer::Get().SetBusMuted("music", false));
	PropValue value;
	NOX_CHECK(GetProperty(node, "AudioSource", "bus", value));
	NOX_CHECK(std::get<std::string>(value) == "music");
	NOX_CHECK(SetProperty(node, "AudioSource", "bus", PropValue(std::string("ui"))));
	NOX_CHECK_EQ(node.getBus(), std::string("ui"));
}

NOX_TEST(AudioTimeDebug, ProceduralVehicleAudioRoutesThroughADefaultSfxBus) {
	ProceduralVehicleAudio audio;
	NOX_CHECK_EQ(audio.Bus(), std::string("sfx"));
	audio.SetBus("music");
	NOX_CHECK_EQ(audio.Bus(), std::string("music"));
	audio.SetBus("");
	NOX_CHECK_EQ(audio.Bus(), std::string("sfx"));
}

// ---- Listener ------------------------------------------------------------------------------------------------

NOX_TEST(AudioTimeDebug, ListenerSelectsTheFirstCurrentCandidate) {
	NOX_CHECK(AudioListenerSelect({}) == -1);
	NOX_CHECK(AudioListenerSelect({ false, false }) == -1);
	NOX_CHECK(AudioListenerSelect({ false, true, true }) == 1);
	NOX_CHECK(AudioListenerSelect({ true, false }) == 0);
}

NOX_TEST(AudioTimeDebug, ListenerPoseFollowsTheNodeTransform) {
	// Identity: forward is -Z, so the yaw in the camera convention (atan2(z, x)) is -90 degrees.
	const AudioListenerPose identity = AudioListenerPoseFromWorld(glm::mat4(1.0f));
	NOX_CHECK(Near(identity.angleDegrees, -90.0f));
	NOX_CHECK(Near(identity.position.x, 0.0f) && Near(identity.position.y, 0.0f) && Near(identity.position.z, 0.0f));

	const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, -2.0f));
	const AudioListenerPose p = AudioListenerPoseFromWorld(moved);
	NOX_CHECK(Near(p.position.x, 3.0f) && Near(p.position.y, 1.0f) && Near(p.position.z, -2.0f));

	// Turning 90 degrees about Y: forward (0,0,-1) becomes (-1,0,0), a yaw of 180 degrees.
	const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const AudioListenerPose q = AudioListenerPoseFromWorld(turned);
	NOX_CHECK(std::fabs(std::fabs(q.angleDegrees) - 180.0f) < 1e-2f);

	// A non-finite transform gives the default pose instead of NaN.
	glm::mat4 bad(1.0f);
	bad[3][0] = std::numeric_limits<float>::quiet_NaN();
	const AudioListenerPose r = AudioListenerPoseFromWorld(bad);
	NOX_CHECK(r.position.x == 0.0f && r.angleDegrees == 0.0f);
}

NOX_TEST(AudioTimeDebug, ListenerFacetIsAddedAndCurrentFlagRoundTrips) {
	RegisterBuiltinComponents();
	SceneNode node;
	NOX_CHECK(!node.HasAudioListener());
	const ComponentDesc* desc = Registry::Get().Find("AudioListener");
	NOX_REQUIRE(desc != nullptr);
	NOX_CHECK(desc->category == "Audio");
	NOX_CHECK(desc->add(node));
	NOX_CHECK(!desc->add(node));   // already present
	NOX_CHECK(node.HasAudioListener());
	NOX_CHECK(!node.IsCurrentAudioListener());
	NOX_CHECK(SetProperty(node, "AudioListener", "current", PropValue(true)));
	NOX_CHECK(node.IsCurrentAudioListener());
	NOX_CHECK(desc->remove(node));
	NOX_CHECK(!node.HasAudioListener());
	NOX_CHECK(!node.IsCurrentAudioListener());
	NOX_CHECK(!SetProperty(node, "AudioListener", "current", PropValue(true)));   // no facet, no set
}

// ---- Time scale ----------------------------------------------------------------------------------------------

NOX_TEST(AudioTimeDebug, TimeScaleIsClampedAndAppliedToDeltaTime) {
	GlobalsGuard guard;
	NOX_CHECK(Near(TimeScale::Get(), 1.0f));
	NOX_CHECK(TimeScale::Set(0.5f));
	NOX_CHECK(Near(TimeScale::Apply(0.1f), 0.05f));
	NOX_CHECK(TimeScale::Set(100.0f));
	NOX_CHECK(TimeScale::Get() == TimeScale::kMax);
	NOX_CHECK(TimeScale::Set(-3.0f));
	NOX_CHECK(TimeScale::Get() == TimeScale::kMin);
	NOX_CHECK(TimeScale::Apply(0.1f) == 0.0f);
	NOX_CHECK(!TimeScale::Set(std::numeric_limits<float>::quiet_NaN()));
	NOX_CHECK(TimeScale::Get() == TimeScale::kMin);   // unchanged by the rejected value
	NOX_CHECK(TimeScale::Apply(-1.0f) == 0.0f);
}

NOX_TEST(AudioTimeDebug, ScaledTimeDrivesTheFixedStepAccumulator) {
	GlobalsGuard guard;
	// Binary-exact values so the step counts are exact: step 1/64, frame 1/32.
	const float step = 1.0f / 64.0f;
	const float frame = 1.0f / 32.0f;
	auto stepsFor = [&](float scale, int frames) {
		TimeScale::Set(scale);
		TimeScale::FixedStepAccumulator acc(step);
		int total = 0;
		for (int i = 0; i < frames; ++i) total += acc.Advance(TimeScale::Apply(frame));
		return total;
	};
	NOX_CHECK(stepsFor(1.0f, 10) == 20);
	NOX_CHECK(stepsFor(0.5f, 10) == 10);
	NOX_CHECK(stepsFor(2.0f, 10) == 40);
	NOX_CHECK(stepsFor(0.0f, 10) == 0);
	// Same scale and frames always give the same count (determinism).
	NOX_CHECK(stepsFor(0.75f, 33) == stepsFor(0.75f, 33));
}

// ---- Debug draw ----------------------------------------------------------------------------------------------

NOX_TEST(AudioTimeDebug, DebugDrawTimedItemsExpireAndFrameItemsLastOneFrame) {
	GlobalsGuard guard;
	DebugDraw::AddLine(glm::vec3(0.0f), glm::vec3(1.0f), 0xFFFFFFFFu, 1.0f);
	DebugDraw::AddText(glm::vec3(0.0f), "frame", 0xFFFFFFFFu, 0.0f);
	NOX_CHECK(DebugDraw::Count() == 2);
	DebugDraw::Age(0.0f);                  // the frame item is consumed, the timed one stays
	NOX_CHECK(DebugDraw::Count() == 1);
	DebugDraw::Age(0.5f);
	NOX_CHECK(DebugDraw::Count() == 1);
	DebugDraw::Age(0.6f);                  // 1.0 s spent
	NOX_CHECK(DebugDraw::Count() == 0);
}

NOX_TEST(AudioTimeDebug, DebugDrawBoxAndSphereQueueOneItemEach) {
	GlobalsGuard guard;
	DebugDraw::AddBox(glm::vec3(0.0f), glm::vec3(1.0f), 0xFF00FF00u, 2.0f);
	DebugDraw::AddSphere(glm::vec3(0.0f), 2.0f, 0xFF0000FFu, 2.0f);
	const std::vector<DebugDraw::Item> items = DebugDraw::Items();
	NOX_REQUIRE(items.size() == 2);
	NOX_CHECK(items[0].kind == DebugDraw::Kind::Box);
	NOX_CHECK(items[1].kind == DebugDraw::Kind::Sphere);
	NOX_CHECK(items[1].radius == 2.0f);
	NOX_CHECK(items[0].timed && Near(items[0].remaining, 2.0f));
}

NOX_TEST(AudioTimeDebug, DebugDrawQueueIsBoundedAndRejectsNonFiniteDurationsAsFrameItems) {
	GlobalsGuard guard;
	for (size_t i = 0; i < DebugDraw::kMaxItems + 10; ++i) DebugDraw::AddLine(glm::vec3(0.0f), glm::vec3(1.0f), 0xFFFFFFFFu, 0.0f);
	NOX_CHECK(DebugDraw::Count() == DebugDraw::kMaxItems);
	DebugDraw::Clear();
	DebugDraw::AddLine(glm::vec3(0.0f), glm::vec3(1.0f), 0xFFFFFFFFu, std::numeric_limits<float>::infinity());
	NOX_REQUIRE(DebugDraw::Count() == 1);
	NOX_CHECK(!DebugDraw::Items()[0].timed);
}

NOX_TEST(AudioTimeDebug, DebugDrawSegmentsCoverEachPrimitive) {
	GlobalsGuard guard;
	DebugDraw::AddLine(glm::vec3(0.0f), glm::vec3(1.0f), 0xFFFFFFFFu, 0.0f);
	DebugDraw::AddBox(glm::vec3(0.0f), glm::vec3(1.0f), 0xFFFFFFFFu, 0.0f);
	DebugDraw::AddSphere(glm::vec3(0.0f), 1.0f, 0xFFFFFFFFu, 0.0f);
	DebugDraw::AddText(glm::vec3(0.0f), "t", 0xFFFFFFFFu, 0.0f);
	const std::vector<DebugDraw::Item> items = DebugDraw::Items();
	NOX_REQUIRE(items.size() == 4);
	NOX_CHECK(DebugDraw::Segments(items[0]).size() == 1);
	NOX_CHECK(DebugDraw::Segments(items[1]).size() == 12);
	NOX_CHECK(DebugDraw::Segments(items[2]).size() == 3 * 32);
	NOX_CHECK(DebugDraw::Segments(items[3]).empty());

	// A projector that rejects every point draws nothing and must not crash on a null list.
	int calls = 0;
	DebugDraw::Draw(nullptr, [&](const glm::vec3&, ImVec2&) { ++calls; return false; });
	NOX_CHECK(calls == 0);
	DebugDraw::Clear();
}

NOX_TEST(AudioTimeDebug, ProjectViewPlacesFrontPointsAtTheRectangle) {
	ImVec2 out;
	const glm::mat4 view(1.0f);   // camera at the origin looking down -Z
	NOX_CHECK(DebugDraw::ProjectView(view, 90.0f, ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), glm::vec3(0.0f, 0.0f, -5.0f), out));
	NOX_CHECK(Near(out.x, 50.0f, 1e-3f) && Near(out.y, 50.0f, 1e-3f));
	NOX_CHECK(!DebugDraw::ProjectView(view, 90.0f, ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), glm::vec3(0.0f, 0.0f, 5.0f), out));
	NOX_CHECK(!DebugDraw::ProjectView(view, 90.0f, ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -5.0f), out));
}

// ---- Headless Lua ----------------------------------------------------------------------------------------------

NOX_TEST(AudioTimeDebug, LuaAudioFailsSoftlyWithoutDeviceOrFile) {
	GlobalsGuard guard;
	LuaHarness h;
	// Missing file or no device: nil plus a message, never an exception.
	NOX_CHECK(h.Expect("select('#', audio.play_one_shot('sounds/does_not_exist.wav')) == 2"));
	NOX_CHECK(h.Expect("(audio.play_one_shot('sounds/does_not_exist.wav')) == nil"));
	NOX_CHECK(h.Expect("select(2, audio.play_one_shot('sounds/does_not_exist.wav', { position = {1, 2, 3}, bus = 'sfx', volume = 0.5, pitch = 1.2 })) ~= nil"));
	NOX_CHECK(h.Expect("select(2, pcall(audio.play_one_shot)) ~= nil"));   // bad arguments raise (a usage error)
}

NOX_TEST(AudioTimeDebug, LuaBusFunctionsReadAndSetTheMixer) {
	GlobalsGuard guard;
	LuaHarness h;
	NOX_CHECK(h.Expect("audio.bus_volume('sfx') == 1"));
	NOX_CHECK(h.Expect("audio.bus_volume('sfx', 0.25) == 0.25"));
	NOX_CHECK(Near(AudioMixer::Get().BusVolume("sfx"), 0.25f));
	NOX_CHECK(h.Expect("audio.bus_volume('nope') == nil"));
	NOX_CHECK(h.Expect("audio.bus_mute('music', true) == true"));
	NOX_CHECK(AudioMixer::Get().BusMuted("music"));
	NOX_CHECK(h.Expect("audio.bus_mute('music') == true"));
	NOX_CHECK(h.Expect("audio.bus_mute('music', false) == false"));
	NOX_CHECK(h.Expect("audio.bus_mute('nope', true) == nil"));
	NOX_CHECK(h.Expect("#audio.buses() >= 5"));
}

NOX_TEST(AudioTimeDebug, LuaDebugFunctionsQueuePrimitives) {
	GlobalsGuard guard;
	LuaHarness h;
	NOX_CHECK(h.Run("debug.clear()"));
	NOX_CHECK(h.Run("debug.line({0, 0, 0}, {1, 2, 3}, 0xFF00FF00, 0.5)"));
	NOX_CHECK(h.Run("debug.box({x = 1, y = 1, z = 1}, 0.5, 0xFF0000FF)"));
	NOX_CHECK(h.Run("debug.sphere({0, 0, 0}, 2.0, 0x80FFFFFF, 1)"));
	NOX_CHECK(h.Run("debug.text({0, 3, 0}, 'label', 0xFFFFFF00)"));
	NOX_CHECK(h.Expect("debug.count() == 4"));
	NOX_CHECK(h.Run("debug.clear()"));
	NOX_CHECK(h.Expect("debug.count() == 0"));
	// Bad input raises a Lua error (no crash), and the queue is unchanged.
	NOX_CHECK(h.Expect("select(1, pcall(debug.line, {0, 0}, {1, 1, 1})) == false"));
	NOX_CHECK(h.Expect("select(1, pcall(debug.sphere, {0, 0, 0}, 1, 0x1FFFFFFFF)) == false"));
	NOX_CHECK(h.Expect("debug.count() == 0"));
	// A colour without alpha is opaque.
	NOX_CHECK(h.Run("debug.clear(); debug.line({0,0,0}, {1,1,1}, 0x00FF0000)"));
	NOX_CHECK(DebugDraw::Items()[0].color == 0xFFFF0000u);
}

NOX_TEST(AudioTimeDebug, LuaTimeScaleGetsAndSetsTheGlobalScale) {
	GlobalsGuard guard;
	LuaHarness h;
	NOX_CHECK(h.Expect("ctx:time_scale() == 1"));
	NOX_CHECK(h.Expect("ctx:time_scale(2) == 2"));
	NOX_CHECK(Near(TimeScale::Get(), 2.0f));
	NOX_CHECK(h.Expect("ctx:time_scale(50) == 10"));   // clamped
	NOX_CHECK(h.Expect("select(1, pcall(ctx.time_scale, ctx, 0/0)) == false"));
	NOX_CHECK(Near(TimeScale::Get(), 10.0f));
}

NOX_TEST(AudioTimeDebug, LuaInputControlIsSafeWithoutAnActionMap) {
	GlobalsGuard guard;
	LuaHarness h;
	NOX_CHECK(h.Expect("select(2, input.rebind('jump', 'W')) ~= nil"));
	NOX_CHECK(h.Expect("(input.bindings('jump')) == nil"));
	NOX_CHECK(h.Expect("input.save_bindings() == false"));
	NOX_CHECK(h.Expect("input.rumble(0.5, 0.5, 100) == false"));
}

NOX_TEST(AudioTimeDebug, InputRebindChangesKeysOfButtonActionsOnly) {
	NoxLua::InputMap map;
	std::vector<std::string> errors;
	const std::string json = R"({"format":1,"actions":{
		"jump":{"type":"button","keys":["Space"],"pad_buttons":["a"]},
		"steer":{"type":"axis","keys_negative":["A"],"keys_positive":["D"]}}})";
	NOX_REQUIRE(map.Parse(json, errors));

	std::string error;
	NOX_CHECK(map.Rebind("jump", "W", error));
	std::vector<std::string> keys, pads;
	NOX_REQUIRE(map.KeyNames("jump", keys, pads));
	NOX_REQUIRE(keys.size() == 1);
	NOX_CHECK_EQ(keys[0], std::string("W"));
	NOX_CHECK_EQ(pads.size(), size_t(1));           // pad buttons untouched

	NOX_CHECK(!map.Rebind("jump", "NotAKey", error));
	NOX_CHECK(!map.Rebind("steer", "Q", error));    // axis actions are not rebound here
	NOX_CHECK(!map.Rebind("missing", "Q", error));
	NOX_CHECK(!map.KeyNames("missing", keys, pads));
}
