// Animation reflection descriptors. See src/reflect/Reflection.h and the Light descriptor in
// src/reflect/BuiltinComponents.cpp for the pattern.
//
// The Animator facet is the node's ECS AnimationComponent, the state the editor and scene runtime play
// through AnimationSystem. The clip list comes from the renderable's model. The AnimationComponent::controller
// pointer is not used by the editor path yet, so it is not exposed.

#include "../ComponentBuilders.h"
#include "../ReflectServices.h"

#include <algorithm>
#include "../../Animation.h"
#include "../../AnimatorRuntime.h"
#include "../../AnimationSystem.h"
#include "../../ComponentManager.h"
#include "../../Scene.h"
#include "../../SceneGraph.h"
#include "../../SceneNode.h"

namespace nox::reflect {

namespace {

ComponentManager* ActiveComponentManager() {
	if (!Services().sceneGraph) return nullptr;
	SceneGraph* graph = Services().sceneGraph();
	return graph ? graph->GetComponentManager() : nullptr;
}

// Playback is driven by the scene graph's AnimationSystem; null when no scene is loaded.
AnimationSystem* ActiveAnimationSystem() {
	if (!Services().sceneGraph) return nullptr;
	SceneGraph* graph = Services().sceneGraph();
	return graph ? graph->GetAnimationSystem() : nullptr;
}

std::shared_ptr<Scene> ModelOf(const SceneNode& node) {
	const RenderableComponent* renderable = node.GetRenderableComponent();
	return renderable ? renderable->model : nullptr;
}

// Index of the named clip in the model, or -1.
int ClipIndex(const Scene& model, const std::string& name) {
	for (size_t i = 0; i < model.animations.size(); ++i) {
		if (model.animations[i].name == name) return static_cast<int>(i);
	}
	return -1;
}

// The clip the component currently points at, or null when none is selected.
Animation* CurrentClip(const SceneNode& node) {
	const AnimationComponent* anim = node.GetAnimationComponent();
	const std::shared_ptr<Scene> model = ModelOf(node);
	if (!anim || !model) return nullptr;
	if (anim->currentAnimationIndex < 0 || anim->currentAnimationIndex >= static_cast<int>(model->animations.size())) {
		return nullptr;
	}
	return &model->animations[static_cast<size_t>(anim->currentAnimationIndex)];
}

// Starts a clip by index through the system. The clip's own looping flag applies (as in the animation window).
bool PlayClipIndex(SceneNode& node, int index) {
	const std::shared_ptr<Scene> model = ModelOf(node);
	AnimationSystem* system = ActiveAnimationSystem();
	if (!model || !system || !node.HasECSEntity()) return false;
	if (index < 0 || index >= static_cast<int>(model->animations.size())) return false;
	Animation& clip = model->animations[static_cast<size_t>(index)];
	system->PlayAnimation(node.GetEntityID(), index, clip.isLooping);
	return true;
}

// Largest morph target count across the model's meshes.
int MorphTargetCount(const Scene& model) {
	int count = 0;
	for (const MeshComponent& mesh : model.meshes) count = std::max(count, mesh.morphTargetCount);
	return count;
}

void RegisterAnimator() {
	ComponentDesc c;
	c.name = "Animator";
	c.category = "Animation";
	c.tooltip = "Plays the clips of the node's model. Needs a model with animations.";
	c.order = 30;
	c.has = [](const SceneNode& n) { return n.GetAnimationComponent() != nullptr; };
	c.add = [](SceneNode& n) {
		ComponentManager* cm = ActiveComponentManager();
		const std::shared_ptr<Scene> model = ModelOf(n);
		if (!cm || !n.HasECSEntity() || !model || model->animations.empty()) return false;
		if (cm->HasAnimation(n.GetEntityID())) return false;
		return cm->AddAnimation(n.GetEntityID()) != nullptr;
	};
	c.remove = [](SceneNode& n) {
		ComponentManager* cm = ActiveComponentManager();
		if (!cm || !n.HasECSEntity() || !cm->HasAnimation(n.GetEntityID())) return false;
		cm->RemoveAnimation(n.GetEntityID());
		return true;
	};

	PropertyDesc clip = Prop("clip", PropType::String,
		[](const SceneNode& n) {
			const AnimationComponent* anim = n.GetAnimationComponent();
			const std::shared_ptr<Scene> model = ModelOf(n);
			if (!anim || !model || anim->currentAnimationIndex < 0 ||
				anim->currentAnimationIndex >= static_cast<int>(model->animations.size())) {
				return PropValue(std::string());
			}
			return PropValue(model->animations[static_cast<size_t>(anim->currentAnimationIndex)].name);
		},
		[](SceneNode& n, const PropValue& v) {
			AnimationComponent* anim = n.GetAnimationComponent();
			const std::shared_ptr<Scene> model = ModelOf(n);
			if (!anim || !model) return false;
			const int index = ClipIndex(*model, std::get<std::string>(v));
			if (index < 0) return false;
			// Selecting a clip rewinds it; whether it plays is left to play()/resume().
			anim->currentAnimationIndex = index;
			anim->animationTime = 0.0f;
			return true;
		});
	clip.tooltip = "Name of the selected clip";

	PropertyDesc speed = Ranged(Prop("speed", PropType::Float,
		[](const SceneNode& n) {
			const AnimationComponent* anim = n.GetAnimationComponent();
			return PropValue(static_cast<double>(anim ? anim->speed : 1.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			AnimationComponent* anim = n.GetAnimationComponent();
			if (!anim) return false;
			anim->speed = static_cast<float>(std::get<double>(v));
			return true;
		}), 0.0, 10.0, 0.01);

	// Looping is stored on the clip, which the model shares with every node that uses it.
	PropertyDesc loop = Prop("loop", PropType::Bool,
		[](const SceneNode& n) {
			const Animation* clip = CurrentClip(n);
			return PropValue(clip ? clip->isLooping : true);
		},
		[](SceneNode& n, const PropValue& v) {
			Animation* clip = CurrentClip(n);
			if (!clip) return false;
			clip->isLooping = std::get<bool>(v);
			return true;
		});
	loop.tooltip = "Loop the selected clip (shared by every node using this model)";

	// The state machine graph (JSON text, docs/animation.md). Empty = plain clip playback.
	PropertyDesc graph = Prop("graph", PropType::String,
		[](const SceneNode& n) {
			const AnimationComponent* anim = n.GetAnimationComponent();
			return PropValue(anim && anim->animator ? anim->animator->GraphJson() : std::string());
		},
		[](SceneNode& n, const PropValue& v) {
			AnimationComponent* anim = n.GetAnimationComponent();
			if (!anim) return false;
			const std::string& text = std::get<std::string>(v);
			if (!anim->animator) {
				if (text.empty()) return true;
				anim->animator = std::make_shared<AnimatorRuntime>();
			}
			// The graph binds to the model on the first update, so loading order does not matter.
			anim->animator->SetGraphJson(text);
			anim->isPlaying = !text.empty();
			anim->isPaused = false;
			return true;
		});
	graph.tooltip = "State machine as JSON: params, states (clip or 1D blend), transitions. Empty = plain clip playback.";

	PropertyDesc state = Prop("state", PropType::String,
		[](const SceneNode& n) {
			const AnimationComponent* anim = n.GetAnimationComponent();
			return PropValue(anim && anim->animator ? anim->animator->CurrentStateName() : std::string());
		}, nullptr, PropNoSave);
	state.tooltip = "Current state of the state machine";

	c.properties = {
		clip,
		speed,
		loop,
		graph,
		state,
		Prop("playing", PropType::Bool,
			[](const SceneNode& n) {
				const AnimationComponent* anim = n.GetAnimationComponent();
				return PropValue(anim && anim->isPlaying);
			}, nullptr, PropNoSave),
		Prop("time", PropType::Float,
			[](const SceneNode& n) {
				const AnimationComponent* anim = n.GetAnimationComponent();
				return PropValue(static_cast<double>(anim ? anim->animationTime : 0.0f));
			},
			[](SceneNode& n, const PropValue& v) {
				AnimationComponent* anim = n.GetAnimationComponent();
				if (!anim) return false;
				anim->animationTime = std::max(0.0f, static_cast<float>(std::get<double>(v)));
				return true;
			}, PropNoSave),
		Prop("clip_count", PropType::Int,
			[](const SceneNode& n) {
				const std::shared_ptr<Scene> model = ModelOf(n);
				return PropValue(static_cast<int64_t>(model ? model->animations.size() : 0));
			}, nullptr, PropNoSave),
	};

	c.methods = {
		Method("play", { PropType::String },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				const std::shared_ptr<Scene> model = ModelOf(n);
				if (!model) return false;
				return PlayClipIndex(n, ClipIndex(*model, std::get<std::string>(args[0])));
			}),
		Method("play_index", { PropType::Int },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				return PlayClipIndex(n, static_cast<int>(std::get<int64_t>(args[0])));
			}),
		Method("stop", {},
			[](SceneNode& n, const std::vector<PropValue>&, PropValue&) {
				AnimationSystem* system = ActiveAnimationSystem();
				if (!system || !n.HasECSEntity()) return false;
				system->StopAnimation(n.GetEntityID());
				return true;
			}, true),
		Method("pause", {},
			[](SceneNode& n, const std::vector<PropValue>&, PropValue&) {
				const AnimationComponent* anim = n.GetAnimationComponent();
				AnimationSystem* system = ActiveAnimationSystem();
				if (!anim || !anim->isPlaying || !system) return false;
				system->PauseAnimation(n.GetEntityID());
				return true;
			}, true),
		Method("resume", {},
			[](SceneNode& n, const std::vector<PropValue>&, PropValue&) {
				const AnimationComponent* anim = n.GetAnimationComponent();
				AnimationSystem* system = ActiveAnimationSystem();
				if (!anim || !anim->isPaused || !system) return false;
				system->ResumeAnimation(n.GetEntityID());
				return true;
			}, true),
		Method("set_float", { PropType::String, PropType::Float },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				AnimationSystem* system = ActiveAnimationSystem();
				if (!system || !n.HasECSEntity()) return false;
				return system->SetAnimatorFloat(n.GetEntityID(), std::get<std::string>(args[0]),
					static_cast<float>(std::get<double>(args[1])));
			}),
		Method("set_bool", { PropType::String, PropType::Bool },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				AnimationSystem* system = ActiveAnimationSystem();
				if (!system || !n.HasECSEntity()) return false;
				return system->SetAnimatorBool(n.GetEntityID(), std::get<std::string>(args[0]), std::get<bool>(args[1]));
			}),
		Method("set_trigger", { PropType::String },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				AnimationSystem* system = ActiveAnimationSystem();
				if (!system || !n.HasECSEntity()) return false;
				return system->SetAnimatorTrigger(n.GetEntityID(), std::get<std::string>(args[0]));
			}),
		Method("current_state", {},
			[](SceneNode& n, const std::vector<PropValue>&, PropValue& out) {
				AnimationSystem* system = ActiveAnimationSystem();
				if (!system || !n.HasECSEntity()) return false;
				out = PropValue(system->AnimatorState(n.GetEntityID()));
				return true;
			}),
		Method("set_morph_weight", { PropType::Int, PropType::Float },
			[](SceneNode& n, const std::vector<PropValue>& args, PropValue&) {
				const std::shared_ptr<Scene> model = ModelOf(n);
				AnimationSystem* system = ActiveAnimationSystem();
				if (!model || !system || !n.HasECSEntity() || !n.GetAnimationComponent()) return false;
				const int count = MorphTargetCount(*model);
				const int64_t index = std::get<int64_t>(args[0]);
				if (index < 0 || index >= count) return false;
				std::vector<float> weights = system->GetMorphWeights(n.GetEntityID());
				weights.resize(static_cast<size_t>(count), 0.0f);
				weights[static_cast<size_t>(index)] = static_cast<float>(std::get<double>(args[1]));
				system->SetMorphWeights(n.GetEntityID(), weights);
				return true;
			}),
	};
	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterAnimationComponents() {
	RegisterAnimator();
}

} // namespace nox::reflect
