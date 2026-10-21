-- Music, fades and a 3D loop (docs/audio.md). Generic API only: the track and the speaker are project assets.
-- The track streams from disk and loops. It fades down while a menu is open: hud.lua reports the menu state every frame
-- through Music.menuChanged (the same condition that pauses the engine sound there). The speaker is an instanced
-- prefab with an inverse falloff and occlusion, looping through its AudioSource component.

local Menu = require("menu")

local TRACK = "sounds/gundam.wav"   -- about five minutes of WAV: streamed, never loaded whole
local SPEAKER = "projects/sample-arena/prefabs/speaker.prefab.json"
local MUSIC_LEVEL = 0.6
local MUSIC_DUCKED = 0.12

local Music = {}
local demo = { wasBlocking = false }

-- Called by hud.lua's input system every frame with whether a menu is open.
function Music.menuChanged(blocking)
  if blocking ~= demo.wasBlocking then
    demo.wasBlocking = blocking
    audio.set_music_volume(blocking and MUSIC_DUCKED or MUSIC_LEVEL, 0.4)
  end
end

nox.on("begin_play", function(ctx)
  demo.wasBlocking = Menu.blocking()
  local ok, reason = audio.play_music(TRACK, {
    loop = true,
    volume = demo.wasBlocking and MUSIC_DUCKED or MUSIC_LEVEL,
    fade_in = 2.0,
    bus = "music",
  })
  if not ok then ctx:log("music: " .. tostring(reason)) end

  -- Instancing needs a scene host; hosts without one (the test harness) play the music only.
  local placed, speaker = pcall(ctx.instantiate, ctx, SPEAKER, {
    name = "Speaker",
    position = { x = 6.0, y = 1.0, z = 0.0 },
  })
  local source = placed and speaker and ctx:component(speaker, "AudioSource") or nil
  if source then source:play() end
  ctx:log("music playing: " .. tostring(audio.music_playing()) .. ", speaker: " .. tostring(source ~= nil))
end)

nox.on("end_play", function(ctx)
  audio.stop_music({ fade_out = 0.5 })
end)

return Music
