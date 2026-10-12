// Procedural vehicle audio: offline synthesis (no device), cue names, and the Lua parameter/cue bindings
// (ctx:audio_set, ctx:audio_cue). The SDL callback is not exercised here; it only copies what Update/Play publish.
#include "TestFramework.h"

#include "SceneGraph.h"
#include "audio/ProceduralVehicleAudio.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {
	class MemorySource : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const auto it = files.find(logicalPath);
			if (it == files.end()) {
				error = "no such file";
				return false;
			}
			text = it->second;
			return true;
		}
	};

	const char* kEntry = "projects/t/scripts/main.lua";

	// Renders `seconds` of audio in stereo blocks; returns the RMS of the last `tailSeconds`.
	float RenderRms(const VehicleAudioState& state, float seconds, float tailSeconds, bool* finite, float* peak) {
		const float sr = 48000.0f;
		const int channels = 2;
		const int total = static_cast<int>(seconds * sr);
		std::vector<float> block(512 * channels);
		double tailSum = 0.0;
		long tailCount = 0;
		int done = 0;
		float maxAbs = 0.0f;
		bool allFinite = true;
		while (done < total) {
			const int frames = std::min(512, total - done);
			ProceduralVehicleRenderOffline(block.data(), frames, channels, sr, state);
			for (int i = 0; i < frames * channels; ++i) {
				const float v = block[static_cast<size_t>(i)];
				if (!std::isfinite(v)) allFinite = false;
				maxAbs = std::max(maxAbs, std::fabs(v));
				if (done + i / channels >= total - static_cast<int>(tailSeconds * sr)) {
					tailSum += static_cast<double>(v) * v;
					++tailCount;
				}
			}
			done += frames;
		}
		if (finite) *finite = allFinite;
		if (peak) *peak = maxAbs;
		return tailCount > 0 ? static_cast<float>(std::sqrt(tailSum / static_cast<double>(tailCount))) : 0.0f;
	}

	struct Harness {
		MemorySource source;
		SceneGraph scene;
		NoxLua::LuaWorld world;

		Harness()
			: world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source) {
		}

		bool Load(const std::string& body) {
			source.files[kEntry] = body;
			NoxLua::ProjectContent project;
			project.name = "t";
			project.entry = kEntry;
			project.scriptRoots = { "projects/t/scripts" };
			return world.Load(project, scene, nlohmann::json::object());
		}
	};
}

NOX_TEST(VehicleAudio, CueNamesRoundTrip) {
	for (size_t i = 0; i < kAudioCueCount; ++i) {
		const AudioCue cue = static_cast<AudioCue>(i);
		AudioCue back = AudioCue::Select;
		NOX_CHECK(AudioCueFromName(AudioCueName(cue), back));
		NOX_CHECK(back == cue);
	}
	AudioCue unused = AudioCue::Select;
	NOX_CHECK(!AudioCueFromName("Checkpoint", unused));   // names are lower case and generic
	NOX_CHECK(!AudioCueFromName("", unused));
	NOX_CHECK(std::string(AudioCueName(AudioCue::GearShift)) == "gear_shift");
}

NOX_TEST(VehicleAudio, EngineRendersFiniteAndBounded) {
	VehicleAudioState state;
	state.active = true;
	state.rpm = 3000.0f;
	state.throttle = 0.6f;
	state.speed01 = 0.3f;
	bool finite = false;
	float peak = 0.0f;
	const float rms = RenderRms(state, 0.5f, 0.2f, &finite, &peak);
	NOX_CHECK(finite);
	NOX_CHECK(rms > 0.001f);
	NOX_CHECK(peak <= 1.0f);
}

NOX_TEST(VehicleAudio, InactiveIsSilentAndPausedFadesOut) {
	// The offline synth is shared by the tests: let it settle to silence first.
	VehicleAudioState off;
	RenderRms(off, 4.0f, 0.1f, nullptr, nullptr);
	NOX_CHECK(RenderRms(off, 0.3f, 0.1f, nullptr, nullptr) < 1e-4f);

	VehicleAudioState on;
	on.active = true;
	on.rpm = 4000.0f;
	on.throttle = 0.5f;
	const float running = RenderRms(on, 0.6f, 0.2f, nullptr, nullptr);
	NOX_CHECK(running > 0.001f);

	// The same parameters, paused: the engine level falls away while rendering.
	VehicleAudioState paused = on;
	paused.paused = true;
	NOX_CHECK(RenderRms(paused, 1.5f, 0.2f, nullptr, nullptr) < running * 0.5f);
}

NOX_TEST(VehicleAudio, CueProducesSound) {
	VehicleAudioState silent;
	RenderRms(silent, 4.0f, 0.1f, nullptr, nullptr);
	NOX_CHECK(RenderRms(silent, 0.2f, 0.1f, nullptr, nullptr) < 1e-4f);
	ProceduralVehicleTriggerOffline(AudioCue::Complete);
	float peak = 0.0f;
	bool finite = false;
	RenderRms(silent, 0.3f, 0.0f, &finite, &peak);
	NOX_CHECK(finite);
	NOX_CHECK(peak > 0.01f);
}

NOX_TEST(VehicleAudio, DeviceFreeServiceIsSafe) {
	// No Init(): the service reports no device and every call is a no-op (headless machines and CI).
	ProceduralVehicleAudio audio;
	NOX_CHECK(!audio.IsAvailable());
	VehicleAudioState state;
	state.active = true;
	audio.Update(state);
	audio.Play(AudioCue::Tick);
	audio.Shutdown();
	NOX_CHECK(!audio.IsAvailable());
}

NOX_TEST(LuaAudio, PublishesParametersAndQueuesCues) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'drive', update = function(ctx, dt)\n"
		"  ctx:audio_set({ rpm = 5000, rpm_max = 7000, throttle = 0.75, speed01 = 2, slip01 = 0.1, boost = true, active = true })\n"
		"  ok_cue = ctx:audio_cue('gear_shift')\n"
		"  ran = true\n"
		"end }\n"));
	h.world.BeginPlay();
	h.world.Update(1.0f / 60.0f, true);

	const NoxLua::LuaServices& svc = h.world.Services();
	NOX_CHECK(svc.audioUsed);
	NOX_CHECK(svc.audio.active);
	NOX_CHECK(svc.audio.rpm == 5000.0f);
	NOX_CHECK(svc.audio.rpmMax == 7000.0f);
	NOX_CHECK(svc.audio.throttle == 0.75f);
	NOX_CHECK(svc.audio.speed01 == 1.0f);          // clamped to 0..1
	NOX_CHECK(svc.audio.boost);
	NOX_CHECK(svc.audioCues.size() == 1u);
	NOX_CHECK(svc.audioCues[0] == AudioCue::GearShift);
}

NOX_TEST(LuaAudio, RejectsUnknownNamesAndBadValues) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'drive', update = function(ctx, dt)\n"
		"  ok_key = pcall(ctx.audio_set, ctx, { rpmm = 3 })\n"
		"  ok_type = pcall(ctx.audio_set, ctx, { boost = 1 })\n"
		"  ok_num = pcall(ctx.audio_set, ctx, { rpm = 'fast' })\n"
		"  ok_cue = pcall(ctx.audio_cue, ctx, 'explosion')\n"
		"  ran = true\n"
		"end }\n"));
	h.world.BeginPlay();
	h.world.Update(1.0f / 60.0f, true);
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "ran");
	NOX_REQUIRE(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	const char* names[] = { "ok_key", "ok_type", "ok_num", "ok_cue" };
	for (const char* name : names) {
		lua_getglobal(L, name);
		NOX_CHECK(lua_toboolean(L, -1) == 0);
		lua_pop(L, 1);
	}
	NOX_CHECK(!h.world.Services().audioUsed);   // rejected calls publish nothing
}

NOX_TEST(LuaAudio, CueBudgetPerFrame) {
	Harness h;
	NOX_REQUIRE(h.Load(
		"nox.system{ name = 'drive', update = function(ctx, dt)\n"
		"  accepted = 0\n"
		"  for i = 1, 40 do if ctx:audio_cue('tick') then accepted = accepted + 1 end end\n"
		"end }\n"));
	h.world.BeginPlay();
	h.world.Update(1.0f / 60.0f, true);
	lua_State* L = h.world.GetRuntime().State();
	lua_getglobal(L, "accepted");
	NOX_CHECK(lua_tointeger(L, -1) == 16);
	lua_pop(L, 1);
}
