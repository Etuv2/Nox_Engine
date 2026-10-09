#pragma once

/*
* Procedural driving audio (docs/nfs2015/game.md). Everything is synthesised at runtime in an SDL
* audio callback, so no third-party sound files are needed: engine (rpm/load driven harmonic stack
* with intake noise and limiter bounce), tyre squeal (slip driven), wind (speed), nitrous hiss and
* short interface cues.
*/
struct DriveAudioState {
	bool active = false;     // drive mode running (engine audible)
	bool paused = false;     // game paused: engine/tyres/wind fade out, UI cues still play
	float rpm = 900.0f;
	float rpmMax = 8000.0f;
	float throttle = 0.0f;   // 0..1
	float speed01 = 0.0f;    // 0..1 of top speed
	float slip01 = 0.0f;     // 0..1 tyre slip (drift / handbrake / hard cornering)
	bool nitro = false;
	bool limiter = false;    // rev limiter cut this frame
	float masterVolume = 0.8f;
};

enum class UiSound { Select, Back, Countdown, Go, Checkpoint, Finish, Missed, Record, Shift };

class DriveAudio {
public:
	static DriveAudio& Get();
	bool Init();
	void Shutdown();
	void Update(const DriveAudioState& state);
	void Play(UiSound sound);
	bool IsAvailable() const;
};
