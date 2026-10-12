#pragma once
// Procedural vehicle audio (engine service, game-agnostic). Everything is synthesised at runtime in an SDL audio
// callback, so no sound files are needed: an engine (rpm/load driven harmonic stack with intake noise and a limiter
// bounce), tyre squeal (slip driven), wind (speed), a boost hiss and short cues.
//
// Threading: Update() and Play() run on the gameplay thread (a project publishes its parameters once per frame, never
// from the audio callback). The SDL callback only reads the last published state and the queued cues. Nothing here
// calls Lua or game code.
//
// Parameters are plain values; the owner maps its own state onto them. The cue names are generic ("select", "tick",
// "gear_shift", ...) and are the same names the Lua API accepts (ctx:audio_cue).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class AudioCue : uint8_t { Select, Back, Tick, Start, Marker, Complete, Fail, Record, GearShift };
constexpr size_t kAudioCueCount = 9;

// Stable lower-case name ("gear_shift"). AudioCueFromName is false for unknown names.
const char* AudioCueName(AudioCue cue);
bool AudioCueFromName(const std::string& name, AudioCue& out);

struct VehicleAudioState {
	bool active = false;       // engine audible (the vehicle is running)
	bool paused = false;       // paused: engine/tyres/wind fade out; cues still play
	float rpm = 900.0f;
	float rpmMax = 8000.0f;
	float throttle = 0.0f;     // 0..1
	float speed01 = 0.0f;      // 0..1 of top speed
	float slip01 = 0.0f;       // 0..1 tyre slip
	bool boost = false;        // boost/nitrous hiss
	bool limiter = false;      // rev limiter cut this frame
	float masterVolume = 0.8f; // 0..1
};

class ProceduralVehicleAudio {
public:
	ProceduralVehicleAudio();
	~ProceduralVehicleAudio();
	ProceduralVehicleAudio(const ProceduralVehicleAudio&) = delete;
	ProceduralVehicleAudio& operator=(const ProceduralVehicleAudio&) = delete;

	// Opens the output device. False (with a log line) when there is no usable device; every call is then a no-op.
	bool Init();
	void Shutdown();
	bool IsAvailable() const;

	// Publishes the parameters the next callbacks render with.
	void Update(const VehicleAudioState& state);
	// Queues a cue (at most 16 pending; extra cues are dropped).
	void Play(AudioCue cue);

private:
	static void RenderCallback(void* userdata, unsigned char* stream, int len);
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Offline synthesis for tests and tooling: renders the same synth without a device.
void ProceduralVehicleRenderOffline(float* out, int frames, int channels, float sampleRate, const VehicleAudioState& state);
void ProceduralVehicleTriggerOffline(AudioCue cue);
