#pragma once
// Lua audio (docs/lua_api.md, "Audio" and docs/audio.md): a global `audio` table with
//   audio.play_one_shot(path, { position = {x,y,z}, bus = "sfx", volume = 1, pitch = 1 }) -> handle | nil, error
//   handle:stop(), handle.playing
//   audio.bus_volume(name [, v]) -> volume | nil     audio.bus_mute(name [, muted]) -> muted | nil
//   audio.buses() -> { names }
// Sounds are read through AssetIO like AudioNode sounds. Without an audio device every call fails softly (nil and an
// error string); nothing here throws or crashes on a missing device.

struct lua_State;

namespace NoxLua {

void InstallAudioApi(lua_State* L);

} // namespace NoxLua
