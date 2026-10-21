#include "LuaAudio.h"

#include "../audio/AudioMixer.h"
#include "../audio/AudioMusic.h"
#include "../audio/AudioPlayback.h"
#include "../audio/AudioSpatial.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace NoxLua {

namespace {

constexpr const char* kHandleMeta = "nox.AudioHandle";
constexpr float kOneShotHearingDistance = 1000.0f;   // same default as AudioNode

struct OneShotHandle {
    uint64_t id;
};

// Returns nil plus an error string (the soft-failure convention of this API).
int NilError(lua_State* L, const std::string& message) {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
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

// Optional fields of an options table (idx must be a table). Each returns true when the field is present.
bool OptNumber(lua_State* L, int idx, const char* key, float& out) {
    lua_getfield(L, idx, key);
    const bool present = !lua_isnil(L, -1);
    if (present) out = static_cast<float>(luaL_checknumber(L, -1));
    lua_pop(L, 1);
    return present;
}

bool OptBool(lua_State* L, int idx, const char* key, bool& out) {
    lua_getfield(L, idx, key);
    const bool present = !lua_isnil(L, -1);
    if (present) out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return present;
}

bool OptString(lua_State* L, int idx, const char* key, std::string& out) {
    lua_getfield(L, idx, key);
    const bool present = !lua_isnil(L, -1);
    if (present) out = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return present;
}

// A number argument, or a table field of that name when the argument is a table (for "fade" style options).
float FadeArgument(lua_State* L, int idx, const char* key) {
    if (lua_istable(L, idx)) {
        float v = 0.0f;
        OptNumber(L, idx, key, v);
        return v;
    }
    if (lua_isnoneornil(L, idx)) return 0.0f;
    return static_cast<float>(luaL_checknumber(L, idx));
}

// audio.play_one_shot(path, { position, bus, volume, pitch, attenuation, min_distance, max_distance, rolloff, occlusion })
int PlayOneShot(lua_State* L) {
    OneShotRequest request;
    request.path = luaL_checkstring(L, 1);
    request.spatial.params = AudioSpatialParams::Hearing(kOneShotHearingDistance);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "position");
        if (!lua_isnil(L, -1)) {
            request.spatial.position = ReadPosition(L, lua_gettop(L));
            request.spatial.spatial = true;
        }
        lua_pop(L, 1);
        OptString(L, 2, "bus", request.bus);
        OptNumber(L, 2, "volume", request.volume);
        OptNumber(L, 2, "pitch", request.pitch);
        std::string curve;
        if (OptString(L, 2, "attenuation", curve)) {
            if (!ParseAudioAttenuation(curve, request.spatial.params.curve)) {
                return luaL_error(L, "play_one_shot: unknown attenuation '%s' (linear, inverse, power)", curve.c_str());
            }
        }
        OptNumber(L, 2, "min_distance", request.spatial.params.minDistance);
        OptNumber(L, 2, "max_distance", request.spatial.params.maxDistance);
        OptNumber(L, 2, "rolloff", request.spatial.params.rolloff);
        OptBool(L, 2, "occlusion", request.spatial.occlusion);
    }
    if (request.path.empty()) return NilError(L, "play_one_shot: empty sound path");
    if (!std::isfinite(request.volume) || !std::isfinite(request.pitch)) {
        return NilError(L, "play_one_shot: volume and pitch must be finite");
    }
    std::string error;
    const uint64_t id = AudioPlaybackPlayOneShot(request, error);
    if (id == 0) return NilError(L, error);

    OneShotHandle* handle = static_cast<OneShotHandle*>(lua_newuserdatauv(L, sizeof(OneShotHandle), 0));
    handle->id = id;
    luaL_getmetatable(L, kHandleMeta);
    lua_setmetatable(L, -2);
    return 1;
}

OneShotHandle* CheckHandle(lua_State* L, int idx) {
    return static_cast<OneShotHandle*>(luaL_checkudata(L, idx, kHandleMeta));
}

// handle:stop([fade]) : fades out over `fade` seconds (default 0 = immediately).
int HandleStop(lua_State* L) {
    OneShotHandle* h = CheckHandle(L, 1);
    AudioPlaybackVoiceStop(h->id, FadeArgument(L, 2, "fade"));
    return 0;
}

// handle:set_volume(v [, seconds]) -> boolean (false when the sound has already finished)
int HandleSetVolume(lua_State* L) {
    OneShotHandle* h = CheckHandle(L, 1);
    const float v = static_cast<float>(luaL_checknumber(L, 2));
    const float seconds = lua_isnoneornil(L, 3) ? 0.0f : static_cast<float>(luaL_checknumber(L, 3));
    lua_pushboolean(L, AudioPlaybackVoiceSetVolume(h->id, v, seconds) ? 1 : 0);
    return 1;
}

// handle:set_pitch(p [, seconds]) -> boolean
int HandleSetPitch(lua_State* L) {
    OneShotHandle* h = CheckHandle(L, 1);
    const float p = static_cast<float>(luaL_checknumber(L, 2));
    const float seconds = lua_isnoneornil(L, 3) ? 0.0f : static_cast<float>(luaL_checknumber(L, 3));
    lua_pushboolean(L, AudioPlaybackVoiceSetPitch(h->id, p, seconds) ? 1 : 0);
    return 1;
}

int HandleIsPlaying(lua_State* L) {
    OneShotHandle* h = CheckHandle(L, 1);
    lua_pushboolean(L, AudioPlaybackVoiceIsPlaying(h->id) ? 1 : 0);
    return 1;
}

// handle.playing, handle.stop, handle.set_volume, handle.set_pitch, handle.is_playing
int HandleIndex(lua_State* L) {
    OneShotHandle* h = CheckHandle(L, 1);
    const std::string key = luaL_checkstring(L, 2);
    if (key == "playing") {
        lua_pushboolean(L, AudioPlaybackVoiceIsPlaying(h->id) ? 1 : 0);
        return 1;
    }
    if (key == "stop") { lua_pushcfunction(L, HandleStop); return 1; }
    if (key == "set_volume") { lua_pushcfunction(L, HandleSetVolume); return 1; }
    if (key == "set_pitch") { lua_pushcfunction(L, HandleSetPitch); return 1; }
    if (key == "is_playing") { lua_pushcfunction(L, HandleIsPlaying); return 1; }
    lua_pushnil(L);
    return 1;
}

// audio.bus_volume(name [, v [, seconds]]) -> volume | nil, error. With seconds, the change is a ramp.
int BusVolume(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    AudioMixer& mixer = AudioMixer::Get();
    if (!lua_isnoneornil(L, 2)) {
        const float v = static_cast<float>(luaL_checknumber(L, 2));
        const float seconds = lua_isnoneornil(L, 3) ? 0.0f : static_cast<float>(luaL_checknumber(L, 3));
        if (!mixer.FadeBus(name, v, seconds)) return NilError(L, "unknown bus '" + name + "' or invalid volume");
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

// audio.duck(bus, level [, fade]) -> true | nil, error. Lowers the bus; audio.unduck restores the volume it had.
int Duck(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const float level = static_cast<float>(luaL_checknumber(L, 2));
    const float fade = lua_isnoneornil(L, 3) ? 0.0f : static_cast<float>(luaL_checknumber(L, 3));
    if (!AudioMixer::Get().Duck(name, level, fade)) return NilError(L, "duck: unknown bus '" + name + "' or invalid level");
    lua_pushboolean(L, 1);
    return 1;
}

// audio.unduck(bus [, fade]) -> true | nil, error (nil when the bus is not ducked)
int Unduck(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const float fade = lua_isnoneornil(L, 2) ? 0.0f : static_cast<float>(luaL_checknumber(L, 2));
    if (!AudioMixer::Get().Unduck(name, fade)) return NilError(L, "unduck: bus '" + name + "' is not ducked");
    lua_pushboolean(L, 1);
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

MusicPlayOptions ReadMusicOptions(lua_State* L, int idx) {
    MusicPlayOptions o;
    if (lua_isnoneornil(L, idx)) return o;
    luaL_checktype(L, idx, LUA_TTABLE);
    OptBool(L, idx, "loop", o.loop);
    OptNumber(L, idx, "volume", o.volume);
    OptNumber(L, idx, "fade_in", o.fadeIn);
    OptString(L, idx, "bus", o.bus);
    return o;
}

// audio.play_music(path, { loop, volume, fade_in, bus = "music" }) -> true | nil, error
int PlayMusic(lua_State* L) {
    const std::string path = luaL_checkstring(L, 1);
    const MusicPlayOptions o = ReadMusicOptions(L, 2);
    std::string error;
    if (!AudioPlaybackMusic().Play(path, o, error)) return NilError(L, error);
    lua_pushboolean(L, 1);
    return 1;
}

// audio.stop_music([{ fade_out }] | fade) -> true
int StopMusic(lua_State* L) {
    AudioPlaybackMusic().Stop(FadeArgument(L, 1, "fade_out"));
    lua_pushboolean(L, 1);
    return 1;
}

// audio.crossfade_music(path, seconds [, { loop, volume, bus }]) -> true | nil, error
int CrossfadeMusic(lua_State* L) {
    const std::string path = luaL_checkstring(L, 1);
    const float seconds = static_cast<float>(luaL_checknumber(L, 2));
    const MusicPlayOptions o = ReadMusicOptions(L, 3);
    std::string error;
    if (!AudioPlaybackMusic().Crossfade(path, seconds, o, error)) return NilError(L, error);
    lua_pushboolean(L, 1);
    return 1;
}

// audio.set_music_volume(v [, seconds]) -> true
int SetMusicVolume(lua_State* L) {
    const float v = static_cast<float>(luaL_checknumber(L, 1));
    const float seconds = lua_isnoneornil(L, 2) ? 0.0f : static_cast<float>(luaL_checknumber(L, 2));
    AudioPlaybackMusic().SetVolume(v, seconds);
    lua_pushboolean(L, 1);
    return 1;
}

// audio.music_position() -> seconds | nil (no track, or the format cannot report a position)
int MusicPosition(lua_State* L) {
    const double pos = AudioPlaybackMusic().Position();
    if (pos < 0.0) {
        lua_pushnil(L);
    }
    else {
        lua_pushnumber(L, static_cast<lua_Number>(pos));
    }
    return 1;
}

// audio.music_seek(seconds) -> boolean
int MusicSeek(lua_State* L) {
    const double s = luaL_checknumber(L, 1);
    lua_pushboolean(L, AudioPlaybackMusic().Seek(s) ? 1 : 0);
    return 1;
}

// audio.music_playing() -> boolean (includes a track that is fading or waiting for a crossfade)
int MusicPlaying(lua_State* L) {
    lua_pushboolean(L, AudioPlaybackMusic().IsPlaying() ? 1 : 0);
    return 1;
}

// audio.music_path() -> path | nil
int MusicPath(lua_State* L) {
    const std::string& path = AudioPlaybackMusic().CurrentPath();
    if (path.empty()) {
        lua_pushnil(L);
    }
    else {
        lua_pushstring(L, path.c_str());
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
        { "duck", Duck }, { "unduck", Unduck },
        { "play_music", PlayMusic }, { "stop_music", StopMusic }, { "crossfade_music", CrossfadeMusic },
        { "set_music_volume", SetMusicVolume }, { "music_position", MusicPosition }, { "music_seek", MusicSeek },
        { "music_playing", MusicPlaying }, { "music_path", MusicPath },
    };
    for (const auto& f : fns) {
        lua_pushcfunction(L, f.fn);
        lua_setfield(L, -2, f.name);
    }
    lua_setglobal(L, "audio");
}

} // namespace NoxLua
