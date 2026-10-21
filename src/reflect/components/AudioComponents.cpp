// Audio descriptors: playback settings of an audio node and its transport methods.
#include "../ComponentBuilders.h"
#include "../../AudioNode.h"
#include "../../SceneNode.h"
#include "../../audio/AudioSpatial.h"

namespace nox::reflect {

namespace {

AudioNode* AudioOf(SceneNode& n) { return dynamic_cast<AudioNode*>(&n); }
const AudioNode* AudioOf(const SceneNode& n) { return dynamic_cast<const AudioNode*>(&n); }

void RegisterAudioSource() {
	ComponentDesc c;
	c.name = "AudioSource";
	c.category = "Audio";
	c.tooltip = "Sound playback owned by an audio node. Loop and spatial changes apply on the next play.";
	c.order = 20;
	c.has = [](const SceneNode& n) { return AudioOf(n) != nullptr; };

	PropertyDesc sound = Prop("sound", PropType::AssetPath,
		[](const SceneNode& n) {
			const AudioNode* a = AudioOf(n);
			return PropValue(a ? a->getSoundFilePath() : std::string());
		},
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			const std::string& path = std::get<std::string>(v);
			if (!a || path.empty()) return false;
			if (a->getSoundFilePath() == path && a->hasSound()) return true;   // already loaded (scene load)
			return a->initAudio(path);
		});
	sound.assetFilter = { ".wav", ".ogg", ".mp3", ".flac" };
	sound.tooltip = "Sound file; the path is kept even when it fails to load";

	PropertyDesc volume = Ranged(Prop("volume", PropType::Float,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(static_cast<double>(a ? a->getVolume() : 0.0f)); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setVolume(static_cast<float>(std::get<double>(v)));
			return true;
		}, PropSlider), 0.0, 1.0, 0.01);

	PropertyDesc pitch = Ranged(Prop("pitch", PropType::Float,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(static_cast<double>(a ? a->getPitch() : 1.0f)); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setPitch(static_cast<float>(std::get<double>(v)));
			return true;
		}), 0.1, 10.0, 0.01);

	PropertyDesc hearing = Ranged(Prop("hearing_distance", PropType::Float,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(static_cast<double>(a ? a->getHearingDistance() : 0.0f)); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setHearingDistance(static_cast<float>(std::get<double>(v)));
			return true;
		}), 1.0, 100000.0, 0.5);
	hearing.tooltip = "Distance at which a 3D source falls silent";

	PropertyDesc loop = Prop("loop", PropType::Bool,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a && a->isLooping()); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setLooping(std::get<bool>(v));
			return true;
		});

	PropertyDesc spatial = Prop("spatial", PropType::Bool,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a && a->is3D()); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setIs3D(std::get<bool>(v));
			return true;
		});
	spatial.tooltip = "3D positioning relative to the listener";

	PropertyDesc autoplay = Prop("autoplay", PropType::Bool,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a && a->getAutoplay()); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setAutoplay(std::get<bool>(v));
			return true;
		});
	autoplay.tooltip = "Start playing when the scene loads";

	PropertyDesc playing = RuntimeProp("playing", PropType::Bool,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a && a->isPlaying()); });

	PropertyDesc bus = Prop("bus", PropType::String,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a ? a->getBus() : std::string()); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setBus(std::get<std::string>(v));
			return true;
		});
	bus.tooltip = "Mixer bus (master, music, sfx, ui, voice, or a project bus); the gain is source * bus * master";

	PropertyDesc attenuation = Prop("attenuation", PropType::String,
		[](const SceneNode& n) {
			const AudioNode* a = AudioOf(n);
			return PropValue(std::string(AudioAttenuationName(a ? a->getAttenuation() : AudioAttenuation::Linear)));
		},
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			AudioAttenuation curve;
			if (!a || !ParseAudioAttenuation(std::get<std::string>(v), curve)) return false;
			a->setAttenuation(curve);
			return true;
		});
	attenuation.tooltip = "Distance curve for 3D sources: linear (default), inverse, or power. Maximum distance is hearing_distance";

	PropertyDesc minDistance = Ranged(Prop("min_distance", PropType::Float,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(static_cast<double>(a ? a->getMinDistance() : 0.0f)); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setMinDistance(static_cast<float>(std::get<double>(v)));
			return true;
		}), 0.0, 100000.0, 0.5);
	minDistance.tooltip = "Full volume inside this distance (inverse: reference distance)";

	PropertyDesc rolloff = Ranged(Prop("rolloff", PropType::Float,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(static_cast<double>(a ? a->getRolloff() : 1.0f)); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setRolloff(static_cast<float>(std::get<double>(v)));
			return true;
		}), 0.01, 16.0, 0.01);
	rolloff.tooltip = "Inverse falloff factor, or the power curve exponent";

	PropertyDesc occlusion = Prop("occlusion", PropType::Bool,
		[](const SceneNode& n) { const AudioNode* a = AudioOf(n); return PropValue(a && a->getOcclusion()); },
		[](SceneNode& n, const PropValue& v) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->setOcclusion(std::get<bool>(v));
			return true;
		});
	occlusion.tooltip = "Attenuate the source when level geometry blocks the line to the listener";

	c.properties = { sound, volume, pitch, hearing, loop, spatial, autoplay, bus, playing,
		attenuation, minDistance, rolloff, occlusion };

	c.methods.push_back(VoidMethod("play", "Start playback with the current loop and spatial settings",
		[](SceneNode& n) {
			AudioNode* a = AudioOf(n);
			if (!a || !a->hasSound()) return false;
			a->play(a->isLooping(), a->is3D());
			return true;
		}));
	c.methods.push_back(VoidMethod("pause", "Pause playback; play() starts the sound again from the beginning",
		[](SceneNode& n) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->pause();
			return true;
		}));
	c.methods.push_back(VoidMethod("stop", "Stop playback",
		[](SceneNode& n) {
			AudioNode* a = AudioOf(n);
			if (!a) return false;
			a->stop();
			return true;
		}));

	Registry::Get().Register(std::move(c));
}

void RegisterAudioListener() {
	ComponentDesc c;
	c.name = "AudioListener";
	c.category = "Audio";
	c.tooltip = "Ears of the scene: 3D audio is heard from the current listener instead of the camera.";
	c.order = 30;
	c.has = [](const SceneNode& n) { return n.HasAudioListener(); };
	c.add = [](SceneNode& n) {
		if (n.HasAudioListener()) return false;
		n.SetHasAudioListener(true);
		return true;
	};
	c.remove = [](SceneNode& n) {
		if (!n.HasAudioListener()) return false;
		n.SetHasAudioListener(false);
		n.SetCurrentAudioListener(false);
		return true;
	};

	PropertyDesc current = Prop("current", PropType::Bool,
		[](const SceneNode& n) { return PropValue(n.IsCurrentAudioListener()); },
		[](SceneNode& n, const PropValue& v) {
			if (!n.HasAudioListener()) return false;
			n.SetCurrentAudioListener(std::get<bool>(v));
			return true;
		});
	current.tooltip = "Use this node for 3D audio. When several nodes are current, the first in scene order wins.";

	c.properties = { current };
	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterAudioComponents() {
	RegisterAudioSource();
	RegisterAudioListener();
}

} // namespace nox::reflect
