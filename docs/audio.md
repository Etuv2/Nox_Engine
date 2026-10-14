# Audio: buses, listener, one-shots

The engine mixes every sound through named buses, like the mixer of a game engine. This page is the short
reference; the Lua functions are in `docs/lua_api.md` ("Audio").

## Buses

| Bus | Use |
| --- | --- |
| `master` | scales every other bus |
| `music` | background music |
| `sfx` | default for every source (AudioNode, procedural vehicle audio) |
| `ui` | interface sounds |
| `voice` | dialogue and narration |

A project can declare more buses in its `project.json`:

```json
{
  "audio": { "buses": ["boss_music", "ambience"] }
}
```

Names use lower-case letters, digits, `_` and `-` (up to 32 characters). A source that names an undeclared bus is
heard through `master` only.

The gain of a channel is:

    final = source volume * bus volume * master volume

A muted bus, or a muted `master`, is silent. Volumes are linear, 0..1 (values outside are clamped).
`AudioMixer::DbToLinear` and `AudioMixer::LinearToDb` convert to decibels (0 dB = 1, -80 dB and below = silent).

### Scene and inspector

Each `AudioSource` has a `bus` property (default `sfx`). In a scene file it is stored in the node's `components`
block as `"AudioSource": { "bus": "music" }`.

### Lua

```lua
audio.bus_volume("music", 0.6)     -- set; returns the new volume
audio.bus_mute("music", true)      -- mute; returns the flag
audio.buses()                      -- { "master", "music", "sfx", "ui", "voice", ... }
```

The procedural vehicle audio is routed through its bus (`sfx` by default) and sounds exactly the same while every bus
is at 1.

## Listener

3D sources are heard from the **listener**: the first node in scene order with an `AudioListener` component whose
`current` flag is set. With no current listener, the camera is the listener, as before.

```json
{ "name": "driver_ears", "components": { "AudioListener": { "current": true } } }
```

- The listener's position is its world position. Its facing is its local `-Z` axis, using the camera's yaw
  convention, so a listener on a camera node hears exactly what the camera hears.
- Several current listeners: the first in scene order wins. Keep one.
- The inspector shows `AudioListener` in the Audio category; `current` is a checkbox.

## One-shots

```lua
local h = audio.play_one_shot("sounds/impact.wav", {
  position = { 4, 0, -2 },   -- optional: 3D, heard from the current listener
  bus = "sfx",               -- optional, default "sfx"
  volume = 0.8,              -- optional, 0..1
  pitch = 1.1,               -- optional, 0.1..10
})
if h then
  -- h:stop() halts it early; h.playing is true while it plays
end
```

Without a position the sound is played unspatialised. The gain is set when the sound starts, so a later
`audio.bus_volume` change does not reach a one-shot already playing (sources do follow bus changes).

Sounds are read through AssetIO, so they come from the project's files or packs like any other asset. A missing
file, or a machine without an audio device, returns `nil, error` instead of raising.

## Notes

- Bus changes take effect on the next frame for every playing `AudioNode` and for the procedural vehicle audio.
- The mixer is process-wide (`AudioMixer::Get()`); the engine has one output mix.
- Source: `src/audio/AudioMixer.*`, `src/audio/AudioListener.*`, `src/scripting/LuaAudio.*`.
