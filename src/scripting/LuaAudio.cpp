#include "LuaAudio.h"

#include "../AudioPlayer.h"
#include "../audio/AudioListener.h"
#include "../audio/AudioMixer.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace NoxLua {

namespace {

constexpr const char* kHandleMeta = "nox.AudioHandle";
constexpr size_t kMaxCachedSounds = 128;
constexpr float kOneShotHearingDistance = 1000.0f;   // same default as AudioNode

struct OneShotHandle {
	int channel;
	Mix_Chunk* chunk;
};

// One-shot sounds share one device and a cache of decoded chunks (kept for the process lifetime, like
// AudioNode's chunks are freed only with their node; one-shot paths are few and repeated).
struct OneShotState {
	AudioPlayer player;
	bool deviceTried = false;
	bool deviceReady = false;
	std::map<std::string, Mix_Chunk*> chunks;
};

OneShotState& State() {
	static OneShotState state;
	return state;
}

bool EnsureDevice() {
	OneShotState& s = State();
	if (!s.deviceTried) {
		s.deviceTried = true;
		s.deviceReady = s.player.init();
	}
	return s.deviceReady;
}

// Returns nil plus an error string (the soft-failure convention of this API).
int NilError(lua_State* L, const std::string& message) {
	lua_pushnil(L);
	lua_pushstring(L, message.c_str());
	return 2;
}

Mix_Chunk* LoadCached(const std::string& path, std::string& error) {
	OneShotState& s = State();
	const auto it = s.chunks.find(path);
	if (it != s.chunks.end()) return it->second;
	if (s.chunks.size() >= kMaxCachedSounds) {
		error = "too many distinct one-shot sounds (limit " + std::to_string(kMaxCachedSounds) + ")";
		return nullptr;
	}
	Mix_Chunk* chunk = s.player.loadSound(path);
	if (!chunk) {
		error = "could not load sound '" + path + "'";
		return nullptr;
	}
	s.chunks[path] = chunk;
	return chunk;
}

glm::vec3 ReadPosition(lua_State* L, int idx) {
	if (!lua_istable(L, idx)) luaL_error(L, "position must be a table {x, y, z}");
	float v[3] = { 0.0f, 0.0f, 0.0f };
	static const char* kKeys[3] = { "x", "y", "z" };
	for (int i = 0; i < 3; ++i) {
		lua_rawgeti(L, idx, i + 1);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_getfield(L, idx, kKeys[i]);
		}
		v[i] = static_cast<float>(luaL_checknumber(L, -1));
		lua_pop(L, 1);
	}
	return glm::vec3(v[0], v[1], v[2]);
}

// audio.play_one_shot(path, { position, bus, volume, pitch })
int PlayOneShot(lua_State* L) {
	const std::string path = luaL_checkstring(L, 1);
	bool spatial = false;
	glm::vec3 position(0.0f);
	std::string bus = AudioMixer::kDefaultSourceBus;
	float volume = 1.0f;
	float pitch = 1.0f;
	if (!lua_isnoneornil(L, 2)) {
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_getfield(L, 2, "position");
		if (!lua_isnil(L, -1)) {
			position = ReadPosition(L, lua_gettop(L));
			spatial = true;
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "bus");
		if (!lua_isnil(L, -1)) bus = luaL_checkstring(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 2, "volume");
		if (!lua_isnil(L, -1)) volume = static_cast<float>(luaL_checknumber(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, 2, "pitch");
		if (!lua_isnil(L, -1)) pitch = static_cast<float>(luaL_checknumber(L, -1));
		lua_pop(L, 1);
	}
	if (path.empty()) return NilError(L, "play_one_shot: empty sound path");
	if (!std::isfinite(volume) || !std::isfinite(pitch)) return NilError(L, "play_one_shot: volume and pitch must be finite");
	if (!EnsureDevice()) return NilError(L, "audio device unavailable");

	std::string error;
	Mix_Chunk* chunk = LoadCached(path, error);
	if (!chunk) return NilError(L, error);

	// Gain: the one-shot's volume times its bus (bus * master). Set once at start; one-shots are short.
	const float gain = std::clamp(volume, 0.0f, 1.0f) * AudioMixer::Get().BusGain(bus);
	const float pitchFactor = std::clamp(pitch, 0.1f, 10.0f);
	OneShotState& s = State();
	int channel = -1;
	if (spatial) {
		const AudioListenerPose listener = AudioListenerCurrent();
		channel = s.player.playSound(chunk, false, true, position, listener.position, listener.angleDegrees,
			pitchFactor, std::clamp(gain, 0.0f, 1.0f), kOneShotHearingDistance);
	}
	else {
		channel = s.player.playSound(chunk, false, false, glm::vec3(0.0f), glm::vec3(0.0f), 0.0f,
			pitchFactor, std::clamp(gain, 0.0f, 1.0f), kOneShotHearingDistance);
	}
	if (channel < 0) return NilError(L, "no free mixer channel");

	OneShotHandle* handle = static_cast<OneShotHandle*>(lua_newuserdatauv(L, sizeof(OneShotHandle), 0));
	handle->channel = channel;
	handle->chunk = chunk;
	luaL_getmetatable(L, kHandleMeta);
	lua_setmetatable(L, -2);
	return 1;
}

OneShotHandle* CheckHandle(lua_State* L, int idx) {
	return static_cast<OneShotHandle*>(luaL_checkudata(L, idx, kHandleMeta));
}

bool HandlePlaying(const OneShotHandle* h) {
	return Mix_GetChunk(h->channel) == h->chunk && Mix_Playing(h->channel) != 0;
}

// handle:stop(). Halts only while the channel still plays this sound (the channel may have been reused).
int HandleStop(lua_State* L) {
	OneShotHandle* h = CheckHandle(L, 1);
	if (HandlePlaying(h)) Mix_HaltChannel(h->channel);
	return 0;
}

// handle.playing and handle.stop
int HandleIndex(lua_State* L) {
	OneShotHandle* h = CheckHandle(L, 1);
	const std::string key = luaL_checkstring(L, 2);
	if (key == "playing") {
		lua_pushboolean(L, HandlePlaying(h) ? 1 : 0);
		return 1;
	}
	if (key == "stop") {
		lua_pushcfunction(L, HandleStop);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

// audio.bus_volume(name [, v]) -> volume
int BusVolume(lua_State* L) {
	const std::string name = luaL_checkstring(L, 1);
	AudioMixer& mixer = AudioMixer::Get();
	if (!lua_isnoneornil(L, 2)) {
		const float v = static_cast<float>(luaL_checknumber(L, 2));
		if (!mixer.SetBusVolume(name, v)) return NilError(L, "unknown bus '" + name + "' or invalid volume");
	}
	if (!mixer.HasBus(name)) return NilError(L, "unknown bus '" + name + "'");
	lua_pushnumber(L, static_cast<lua_Number>(mixer.BusVolume(name)));
	return 1;
}

// audio.bus_mute(name [, muted]) -> muted
int BusMute(lua_State* L) {
	const std::string name = luaL_checkstring(L, 1);
	AudioMixer& mixer = AudioMixer::Get();
	if (!lua_isnoneornil(L, 2)) {
		const bool muted = lua_toboolean(L, 2) != 0;
		if (!mixer.SetBusMuted(name, muted)) return NilError(L, "unknown bus '" + name + "'");
	}
	if (!mixer.HasBus(name)) return NilError(L, "unknown bus '" + name + "'");
	lua_pushboolean(L, mixer.BusMuted(name) ? 1 : 0);
	return 1;
}

// audio.buses() -> { "master", "music", ... }
int Buses(lua_State* L) {
	const std::vector<std::string> names = AudioMixer::Get().BusNames();
	lua_createtable(L, static_cast<int>(names.size()), 0);
	for (size_t i = 0; i < names.size(); ++i) {
		lua_pushstring(L, names[i].c_str());
		lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

} // namespace

void InstallAudioApi(lua_State* L) {
	if (luaL_newmetatable(L, kHandleMeta)) {
		lua_pushcfunction(L, HandleIndex);
		lua_setfield(L, -2, "__index");
	}
	lua_pop(L, 1);

	lua_newtable(L);
	const struct { const char* name; lua_CFunction fn; } fns[] = {
		{ "play_one_shot", PlayOneShot }, { "bus_volume", BusVolume }, { "bus_mute", BusMute }, { "buses", Buses },
	};
	for (const auto& f : fns) {
		lua_pushcfunction(L, f.fn);
		lua_setfield(L, -2, f.name);
	}
	lua_setglobal(L, "audio");
}

} // namespace NoxLua
