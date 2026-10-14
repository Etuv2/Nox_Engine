// Environment descriptors: WorldEnvironment (scene-wide render settings, Godot WorldEnvironment) and the
// light propagation volume (LPV) bounds and grid of an LPV_VOLUME node.
//
// WorldEnvironment lives on the scene root only. Its properties read and write the live RenderContext
// (Services().renderContext); the scene JSON "render_settings" object stays the source of truth, written by
// the scene saver through SceneRenderSettings::ToJson. Every property is PropNoSave so it never lands in
// the node's "components" block. The table of settings is src/render/RenderSettingsTable.h.
#include "../ComponentBuilders.h"
#include "../ReflectServices.h"
#include "../../FrameBuffer.h"
#include "../../RenderContext.h"
#include "../../ScreenQuad.h"
#include "../../SceneGraph.h"
#include "../../SceneNode.h"
#include "../../render/RenderSettingsTable.h"

#include <cmath>

namespace nox::reflect {

namespace {

bool IsSceneRoot(const SceneNode& n) {
	if (!Services().sceneGraph) return false;
	SceneGraph* graph = Services().sceneGraph();
	return graph && graph->GetRoot().get() == &n;
}

RenderContext* LiveRenderContext() {
	return Services().renderContext ? Services().renderContext() : nullptr;
}

void RegisterWorldEnvironment() {
	ComponentDesc c;
	c.name = "WorldEnvironment";
	c.category = "Rendering";
	c.tooltip = "Scene-wide render settings (bloom, exposure, grade, AO, SSR, fog, weather ...). Saved in the scene's render_settings.";
	c.order = 10;
	c.has = [](const SceneNode& n) { return IsSceneRoot(n); };

	static const RenderContext defaults;
	for (const nox::render::RenderSetting& setting : nox::render::RenderSettingTable()) {
		const nox::render::RenderSetting* s = &setting;
		PropertyDesc p = Prop(s->key.c_str(), s->type,
			[s](const SceneNode&) {
				const RenderContext* live = LiveRenderContext();
				return s->get(live ? *live : defaults);
			},
			[s](SceneNode& n, const PropValue& v) {
				if (!IsSceneRoot(n)) return false;
				RenderContext* live = LiveRenderContext();
				if (!live) return false;
				if (s->type == PropType::Enum && (std::get<int64_t>(v) < 0 ||
					std::get<int64_t>(v) >= static_cast<int64_t>(s->enumNames.size()))) {
					return false;
				}
				if (!s->set(*live, v)) return false;
				NotifySceneChanged();
				return true;
			}, PropNoSave | s->flags);
		p.label = s->label;
		p.tooltip = s->tooltip;
		p.minValue = s->minValue;
		p.maxValue = s->maxValue;
		p.step = s->step;
		p.enumNames = s->enumNames;
		c.properties.push_back(std::move(p));
	}
	Registry::Get().Register(std::move(c));
}


bool IsLpvNode(const SceneNode& n) { return n.GetNodeType() == SceneNode::LPV_VOLUME; }

void RegisterLightPropagationVolume() {
	ComponentDesc c;
	c.name = "LightPropagationVolume";
	c.category = "Environment";
	c.tooltip = "Light propagation volume bounds and voxel grid of an LPV volume node";
	c.order = 50;
	c.has = [](const SceneNode& n) { return IsLpvNode(n); };

	c.properties = {
		Prop("center", PropType::Vec3,
			[](const SceneNode& n) { return PropValue(n.GetLPVVolumeData().center); },
			[](SceneNode& n, const PropValue& v) {
				if (!IsLpvNode(n)) return false;
				n.GetLPVVolumeData().center = std::get<glm::vec3>(v);
				NotifySceneChanged();
				return true;
			}),
		Prop("extent", PropType::Vec3,
			[](const SceneNode& n) { return PropValue(n.GetLPVVolumeData().extent); },
			[](SceneNode& n, const PropValue& v) {
				if (!IsLpvNode(n)) return false;
				const glm::vec3 extent = std::get<glm::vec3>(v);
				if (!(extent.x > 0.0f && extent.y > 0.0f && extent.z > 0.0f) || !std::isfinite(extent.x + extent.y + extent.z)) {
					return false;
				}
				n.GetLPVVolumeData().extent = extent;
				NotifySceneChanged();
				return true;
			}),
		Ranged(Prop("voxel_size", PropType::Float,
			[](const SceneNode& n) { return PropValue(static_cast<double>(n.GetLPVVolumeData().voxelSize)); },
			[](SceneNode& n, const PropValue& v) {
				if (!IsLpvNode(n)) return false;
				n.GetLPVVolumeData().voxelSize = static_cast<float>(std::get<double>(v));
				NotifySceneChanged();
				return true;
			}), 0.01, 100.0, 0.01),
		Ranged(Prop("grid_resolution", PropType::Int,
			[](const SceneNode& n) { return PropValue(static_cast<int64_t>(n.GetLPVVolumeData().gridResolution)); },
			[](SceneNode& n, const PropValue& v) {
				if (!IsLpvNode(n)) return false;
				n.GetLPVVolumeData().gridResolution = static_cast<int>(std::get<int64_t>(v));
				NotifySceneChanged();
				return true;
			}), 1.0, 1024.0, 1.0),
	};

	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterEnvironmentComponents() {
	RegisterWorldEnvironment();
	RegisterLightPropagationVolume();
}

} // namespace nox::reflect
