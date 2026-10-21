# Audio: buses, music, fades, 3D sound, one-shots

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
audio.bus_volume("music", 0.6)        -- set; returns the new volume
audio.bus_volume("music", 0.0, 1.5)   -- ramp to silence over 1.5 s
audio.bus_mute("music", true)         -- mute; returns the flag
audio.buses()                         -- { "master", "music", "sfx", "ui", "voice", ... }
audio.duck("music", 0.2, 0.25)        -- lower the bus to 0.2 over 0.25 s, remembering its volume
audio.unduck("music", 0.5)            -- restore the volume it had before the duck
```

Ducking is for dialogue: duck `music` while a `voice` line plays, then unduck when it ends. A second `duck` keeps the
first remembered volume. A manual `bus_volume` cancels the duck.

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
  h:stop(0.3)                -- fades out over 0.3 s, then stops
end
```

Without a position the sound is played unspatialised. A one-shot follows bus changes like a source does, and its
handle fades, re-pitches and stops it (below).

Sounds are read through AssetIO, so they come from the project's files or packs like any other asset. A missing
file, or a machine without an audio device, returns `nil, error` instead of raising.

## Handles and fades

`audio.play_one_shot` returns a handle. Its methods:

| Method | Effect |
| --- | --- |
| `h:stop([fade])` | stops the sound; with `fade` (seconds) it fades to silence first |
| `h:set_volume(v [, seconds])` | sets the sound's volume (0..1); with `seconds`, ramps to it |
| `h:set_pitch(p [, seconds])` | sets the pitch factor (0.1..10); with `seconds`, ramps to it |
| `h:is_playing()` / `h.playing` | true while the sound plays (and while it fades out) |

A handle names the sound, not the mixer channel: once the sound ends, the handle is stale. Its methods then do
nothing and `playing` is false, even if the channel has started another sound.

Ramps are linear in gain (or pitch factor) and run on the audio stage's clock, which is unscaled frame time. A fade
therefore finishes while the game is paused. The sample arena fades its music down while a menu is open
(`projects/sample-arena/scripts/music.lua`).

## Music

Music is one streamed track at a time. The file is decoded from disk as it plays (WAV, OGG, MP3 and FLAC as SDL_mixer
supports them), so a five-minute track is not loaded into memory. Music plays on the `music` bus.

```lua
audio.play_music("sounds/gundam.wav", { loop = true, volume = 0.6, fade_in = 2.0 })  -- true, or nil, error
audio.set_music_volume(0.12, 0.4)     -- ramp the track's level over 0.4 s
audio.stop_music({ fade_out = 1.0 })  -- fades out, then stops (audio.stop_music() stops at once)
audio.crossfade_music("sounds/other.wav", 3.0, { loop = true })
audio.music_playing()                 -- true while a track plays, fades, or waits for a crossfade
audio.music_position()                -- seconds, or nil when there is no track or the format cannot report it
audio.music_seek(30.0)                -- true when the format can seek
audio.music_path()                    -- the current track's path, or nil
```

- `play_music` replaces the current track at once (use `crossfade_music` for a smooth change) and fades the new track in
  over `fade_in`.
- `crossfade_music` is a dip, not an overlap. SDL_mixer plays one music stream at a time, so the current track fades
  out over half of `seconds`, then the new one fades in over the other half. With nothing playing it is a plain fade-in.
- A looping track is restarted by the decoder. A track that is not looping ends and is cleared.
- Music fades are ramps on the audio clock, applied on top of the `music` bus gain, so `audio.duck("music", ...)` and
  `audio.bus_mute("music", true)` work on the track.

## 3D sound

A 3D source (`AudioSource`, or a `position` in `audio.play_one_shot`) is heard from the listener with a distance
curve, a stereo balance, and optional occlusion.

| Curve | Gain at distance `d` (silent at and beyond the maximum) |
| --- | --- |
| `linear` (default) | `1 - t`, where `t` runs from `min_distance` (0) to the maximum (`hearing_distance`) |
| `inverse` | `min / (min + rolloff * (d - min))`: full volume inside `min_distance`, then the classic 1/d falloff |
| `power` | `(1 - t) ^ rolloff`: `rolloff` is the exponent (1 is linear) |

- The default (`linear`, `min_distance` 0) is the falloff that sources always had, so existing scenes sound the same.
- `AudioSource` properties: `attenuation` (`"linear"`, `"inverse"`, `"power"`), `min_distance`, `rolloff`,
  `occlusion`. The maximum distance is `hearing_distance`. In a scene file they are keys of the audio node, next to
  `hearing_distance` and `loop`. They are written only when they differ from the defaults.
- One-shots take the same options: `attenuation`, `min_distance`, `max_distance`, `rolloff`, `occlusion`.

```lua
audio.play_one_shot("sounds/impact.wav", {
  position = { 4, 0, -2 },
  attenuation = "inverse", min_distance = 2.0, max_distance = 60.0,
  occlusion = true,
})
```

**Occlusion** (`occlusion = true`): the engine casts a physics ray from the listener to the source each frame. When level
geometry is in the way, the source's gain is multiplied by 0.35, and the change eases over 0.15 s so toggling line of
sight does not click. Hits within 0.5 m of either end are ignored, so the listener's own body and the source's do not
occlude it. The ray uses every physics layer, so trigger bodies also count. Without a physics engine, sources are never
occluded.

**Not implemented:** reverb zones (SDL_mixer has no effect bus to send to), a low-pass filter for occlusion (the
channel effect slot is used by pitch shifting), and a Doppler effect from source velocity.

**Known limit:** the stereo balance is computed from the source offset in x and y (the original formula, kept so
racing audio is unchanged). In a y-up world the horizontal offset is x and z, so a source the same distance left or
right of the listener can pan wrongly when it sits above or below the listener's level. Fixing it changes the mix of
every 3D source, so it is a separate decision.

## Notes

- Bus changes take effect on the next frame for every playing `AudioNode`, one-shot and the procedural vehicle audio.
- The mixer is process-wide (`AudioMixer::Get()`); the engine has one output mix.
- The audio stage runs once per frame in `Core::Update` (`AudioPlaybackTick`) with unscaled frame time.
- Source: `src/audio/AudioMixer.*` (buses, ramps, ducking), `src/audio/AudioRamp.*`, `src/audio/AudioSpatial.*`
  (curves, occlusion state), `src/audio/AudioVoices.*` (one-shot handles), `src/audio/AudioMusic.*` (music state),
  `src/audio/AudioPlayback.*` (SDL_mixer backend and the per-frame tick), `src/scripting/LuaAudio.*` (the Lua API),
  `src/AudioPlayer.h` (channels, 3D gain, panning), `src/AudioNode.*` (the AudioSource component).
- Tests: `tests/native/test_audio_runtime.cpp` (fade math, curves, occlusion with a mock ray test, crossfade timing,
  handle lifetime, ducking). They run without a device.
