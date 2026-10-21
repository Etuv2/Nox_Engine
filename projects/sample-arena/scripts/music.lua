-- Music, fades and a 3D loop (docs/audio.md). Generic API only: the track and the speaker are project-independent assets.
-- The track streams from disk and loops. It fades down while a menu is open (the same condition that pauses the engine
-- sound in hud.lua) and back up when the menu closes. The speaker is an instanced prefab with an inverse falloff and
-- occlusion, looping through its AudioSource component.

local Menu = require("menu")

local TRACK = "sounds/gundam.wav"   -- about five minutes of WAV: streamed, never loaded whole
local MUSIC_LEVEL = 0.6
local MUSIC_DUCKED = 0.12

local demo = { speakerSpawned = false, wasBlocking = false }

nox.on("begin_play", function(ctx)
  demo.speakerSpawned = false
  demo.wasBlocking = Menu.blocking()
  local ok, reason = audio.play_music(TRACK, {
    loop = true,
    volume = demo.wasBlocking and MUSIC_DUCKED or MUSIC_LEVEL,
    fade_in = 2.0,
    bus = "music",
  })
  if not ok then ctx:log("music: " .. tostring(reason)) end
end)

nox.on("end_play", function(ctx)
  audio.stop_music({ fade_out = 0.5 })
end)

nox.system{
  name = "music_and_speaker",
  order = -9,
  update = function(ctx, dt)
    -- Instancing needs a writable callback, so the speaker is created from a system, once per Play.
    if not demo.speakerSpawned then
      demo.speakerSpawned = true
      local speaker = ctx:instantiate("projects/sample-arena/prefabs/speaker.prefab.json", {
        name = "Speaker",
        position = { x = 6.0, y = 1.0, z = 0.0 },
      })
      local source = speaker and ctx:component(speaker, "AudioSource")
      if source then source:play() end
      ctx:log("music playing: " .. tostring(audio.music_playing()) .. ", speaker: " .. tostring(source ~= nil))
    end

    local blocking = Menu.blocking()
    if blocking ~= demo.wasBlocking then
      demo.wasBlocking = blocking
      audio.set_music_volume(blocking and MUSIC_DUCKED or MUSIC_LEVEL, 0.4)
    end
  end,
}
