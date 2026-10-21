#pragma once
// Lua audio (docs/lua_api.md, "Audio" and docs/audio.md): a global `audio` table with
//   audio.play_one_shot(path, { position = {x,y,z}, bus = "sfx", volume = 1, pitch = 1, attenuation, min_distance,
//     max_distance, rolloff, occlusion }) -> handle | nil, error
//   handle:stop([fade]), handle:set_volume(v [, s]), handle:set_pitch(p [, s]), handle:is_playing(), handle.playing
//   audio.bus_volume(name [, v [, seconds]]) -> volume | nil     audio.bus_mute(name [, muted]) -> muted | nil
//   audio.duck(bus, level [, fade]) / audio.unduck(bus [, fade])     audio.buses() -> { names }
//   audio.play_music(path, { loop, volume, fade_in, bus }) / stop_music([{ fade_out }]) / crossfade_music(path, s [, opts])
//   audio.set_music_volume(v [, s]) / music_playing() / music_path() / music_position() / music_seek(s)
// The playback runtime (music, voices, fades) lives in src/audio/AudioPlayback.*; this file only binds it to Lua.
// Sounds are read through AssetIO like AudioNode sounds. Without an audio device every call fails softly (nil and an
// error string); nothing here throws or crashes on a missing device.

struct lua_State;

namespace NoxLua {

void InstallAudioApi(lua_State* L);

} // namespace NoxLua
