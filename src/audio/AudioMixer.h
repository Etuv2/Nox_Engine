#pragma once
// Audio buses (engine service, game-agnostic). Every audible source names a bus; the final gain of a channel is
//   source volume * bus volume * master volume
// and a muted bus (or a muted master) is silent. "master", "music", "sfx", "ui" and "voice" always exist; a project
// may declare more in its descriptor ("audio": { "buses": [...] }). Sources that name an undeclared bus play through
// the master bus only.
//
// Threading: the mixer is shared by the gameplay thread (AudioNode, Lua) and procedural audio callbacks that read
// the gain through BusGain() under their own device lock; every method takes the mixer mutex briefly.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class AudioMixer {
public:
    static constexpr const char* kMasterBus = "master";
    static constexpr const char* kDefaultSourceBus = "sfx";

    // Process-wide mixer (the engine has one output mix).
    static AudioMixer& Get();

    // ---- Pure math (no state) ----------------------------------------------------------------------
    // Decibels to linear gain: 0 dB = 1, anything at or below -80 dB is silent (0). Non-finite input is silent.
    static float DbToLinear(float db);
    // Inverse of DbToLinear; silence (and anything below -80 dB) maps to -80.
    static float LinearToDb(float linear);
    // Final channel gain: source * bus * master, zero when either the bus or the master is muted.
    static float ComputeGain(float sourceVolume, float busVolume, bool busMuted, float masterVolume, bool masterMuted);

    // ---- Buses -----------------------------------------------------------------------------------------
    // Declares a bus with volume 1 and unmuted. False when the name is empty, too long, not [a-z0-9_-], or exists.
    bool AddBus(const std::string& name);
    bool HasBus(const std::string& name) const;
    // Volume is clamped to 0..1. False for an unknown bus or a non-finite value.
    bool SetBusVolume(const std::string& name, float volume);
    float BusVolume(const std::string& name) const;   // 1 for an unknown bus
    bool SetBusMuted(const std::string& name, bool muted);
    bool BusMuted(const std::string& name) const;
    // Gain applied on top of a source's own volume: bus * master, zero when muted. Unknown buses use the master only.
    float BusGain(const std::string& name) const;
    std::vector<std::string> BusNames() const;

    // Bumped on every change; lets a consumer skip work when nothing changed.
    uint64_t Version() const;

    // Restores the built-in buses to their defaults and drops declared extras (tests, reload).
    void Reset();

private:
    AudioMixer();
    struct Bus {
        std::string name;
        float volume = 1.0f;
        bool muted = false;
    };
    int Find(const std::string& name) const;   // index or -1; caller holds the mutex
    mutable std::mutex m_mutex;
    std::vector<Bus> m_buses;
    uint64_t m_version = 0;
};
