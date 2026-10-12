// DriveAudio.cpp - racing-facing wrapper over the engine's procedural vehicle audio (src/audio/).
// The synthesis, the SDL device and the cue table live in ProceduralVehicleAudio; this file only maps the
// legacy racing names (UiSound, DriveAudioState) onto the generic ones. Kept until NoxRacingLegacy retires.

#include "DriveAudio.h"

#include "audio/ProceduralVehicleAudio.h"

namespace {

ProceduralVehicleAudio& Device() {
    static ProceduralVehicleAudio device;
    return device;
}

AudioCue ToCue(UiSound sound) {
    switch (sound) {
    case UiSound::Select: return AudioCue::Select;
    case UiSound::Back: return AudioCue::Back;
    case UiSound::Countdown: return AudioCue::Tick;
    case UiSound::Go: return AudioCue::Start;
    case UiSound::Checkpoint: return AudioCue::Marker;
    case UiSound::Finish: return AudioCue::Complete;
    case UiSound::Missed: return AudioCue::Fail;
    case UiSound::Record: return AudioCue::Record;
    case UiSound::Shift: return AudioCue::GearShift;
    }
    return AudioCue::Select;
}

VehicleAudioState ToVehicle(const DriveAudioState& s) {
    VehicleAudioState v;
    v.active = s.active;
    v.paused = s.paused;
    v.rpm = s.rpm;
    v.rpmMax = s.rpmMax;
    v.throttle = s.throttle;
    v.speed01 = s.speed01;
    v.slip01 = s.slip01;
    v.boost = s.nitro;
    v.limiter = s.limiter;
    v.masterVolume = s.masterVolume;
    return v;
}

}  // namespace

DriveAudio& DriveAudio::Get() {
    static DriveAudio instance;
    return instance;
}

bool DriveAudio::Init() {
    return Device().Init();
}

void DriveAudio::Shutdown() {
    Device().Shutdown();
}

void DriveAudio::Update(const DriveAudioState& state) {
    Device().Update(ToVehicle(state));
}

void DriveAudio::Play(UiSound sound) {
    Device().Play(ToCue(sound));
}

bool DriveAudio::IsAvailable() const {
    return Device().IsAvailable();
}

void DriveAudioOfflineRender(float* out, int frames, int channels, float sampleRate, const DriveAudioState& st) {
    ProceduralVehicleRenderOffline(out, frames, channels, sampleRate, ToVehicle(st));
}

void DriveAudioOfflineTrigger(UiSound sound) {
    ProceduralVehicleTriggerOffline(ToCue(sound));
}
