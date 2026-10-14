#include "AudioMixer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr size_t kMaxBusNameLength = 32;
constexpr float kSilenceDb = -80.0f;

bool ValidBusName(const std::string& name) {
    if (name.empty() || name.size() > kMaxBusNameLength) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace

AudioMixer::AudioMixer() {
    Reset();
}

AudioMixer& AudioMixer::Get() {
    static AudioMixer mixer;
    return mixer;
}

float AudioMixer::DbToLinear(float db) {
    if (!std::isfinite(db) || db <= kSilenceDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

float AudioMixer::LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.0f) return kSilenceDb;
    return std::max(kSilenceDb, 20.0f * std::log10(linear));
}

float AudioMixer::ComputeGain(float sourceVolume, float busVolume, bool busMuted, float masterVolume, bool masterMuted) {
    if (busMuted || masterMuted) return 0.0f;
    const float s = std::isfinite(sourceVolume) ? std::clamp(sourceVolume, 0.0f, 1.0f) : 0.0f;
    const float b = std::isfinite(busVolume) ? std::clamp(busVolume, 0.0f, 1.0f) : 0.0f;
    const float m = std::isfinite(masterVolume) ? std::clamp(masterVolume, 0.0f, 1.0f) : 0.0f;
    return s * b * m;
}

bool AudioMixer::AddBus(const std::string& name) {
    if (!ValidBusName(name)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Find(name) >= 0) return false;
    Bus bus;
    bus.name = name;
    m_buses.push_back(bus);
    ++m_version;
    return true;
}

bool AudioMixer::HasBus(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return Find(name) >= 0;
}

bool AudioMixer::SetBusVolume(const std::string& name, float volume) {
    if (!std::isfinite(volume)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const int i = Find(name);
    if (i < 0) return false;
    const float clamped = std::clamp(volume, 0.0f, 1.0f);
    if (m_buses[i].volume != clamped) {
        m_buses[i].volume = clamped;
        ++m_version;
    }
    return true;
}

float AudioMixer::BusVolume(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int i = Find(name);
    return i < 0 ? 1.0f : m_buses[i].volume;
}

bool AudioMixer::SetBusMuted(const std::string& name, bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int i = Find(name);
    if (i < 0) return false;
    if (m_buses[i].muted != muted) {
        m_buses[i].muted = muted;
        ++m_version;
    }
    return true;
}

bool AudioMixer::BusMuted(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int i = Find(name);
    return i >= 0 && m_buses[i].muted;
}

float AudioMixer::BusGain(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int master = Find(kMasterBus);
    const int bus = Find(name);
    const float masterVolume = master >= 0 ? m_buses[master].volume : 1.0f;
    const bool masterMuted = master >= 0 && m_buses[master].muted;
    if (bus < 0) return ComputeGain(1.0f, 1.0f, false, masterVolume, masterMuted);
    return ComputeGain(1.0f, m_buses[bus].volume, m_buses[bus].muted, masterVolume, masterMuted);
}

std::vector<std::string> AudioMixer::BusNames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_buses.size());
    for (const Bus& b : m_buses) names.push_back(b.name);
    return names;
}

uint64_t AudioMixer::Version() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_version;
}

void AudioMixer::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buses.clear();
    for (const char* name : { kMasterBus, "music", kDefaultSourceBus, "ui", "voice" }) {
        Bus bus;
        bus.name = name;
        m_buses.push_back(bus);
    }
    ++m_version;
}

int AudioMixer::Find(const std::string& name) const {
    for (size_t i = 0; i < m_buses.size(); ++i) {
        if (m_buses[i].name == name) return static_cast<int>(i);
    }
    return -1;
}
