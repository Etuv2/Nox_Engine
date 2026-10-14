// Model reflection descriptors. See src/reflect/Reflection.h and the Light descriptor in
// src/reflect/BuiltinComponents.cpp for the pattern.
//
// MeshRenderer is a node's model facet. The renderer draws ECS RenderableComponents, not the node, so
// every change here is mirrored into the node's renderable (SyncRenderable) before the scene is marked
// changed.

#include "../ComponentBuilders.h"
#include "../ReflectServices.h"

#include <algorithm>
#include <set>
#include "../../ComponentManager.h"
#include "../../Scene.h"
#include "../../SceneGraph.h"
#include "../../SceneNode.h"

namespace nox::reflect {

namespace {

// The scene graph that owns the ECS data for the active scene; null in headless tools and tests.
ComponentManager* ActiveComponentManager() {
	if (!Services().sceneGraph) return nullptr;
	SceneGraph* graph = Services().sceneGraph();
	return graph ? graph->GetComponentManager() : nullptr;
}

// Rebuilds the node's RenderableComponent from its current model and render settings. Removing and
// re-adding (rather than editing in place) bumps the renderable revision the renderer caches by.
void SyncRenderable(SceneNode& node) {
	ComponentManager* cm = ActiveComponentManager();
	if (!cm || !node.HasECSEntity()) return;
	const EntityID entity = node.GetEntityID();
	if (cm->HasRenderable(entity)) cm->RemoveRenderable(entity);

	const std::shared_ptr<Scene> model = node.GetModel();
	if (!model || (!node.renderWholeModel && node.renderMeshIndices.empty())) return;

	RenderableComponent renderable;
	renderable.model = model;
	renderable.shaderID = node.GetShader();
	renderable.boundingRadius = node.boundingRadius;
	renderable.isSkinned = node.isSkinned;
	renderable.renderWholeModel = node.renderWholeModel;
	renderable.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(node.GetCullingOverride()));
	renderable.meshIndices = node.renderMeshIndices;
	renderable.boneNodes = node.boneNodes;
	renderable.boneInverseBindMatrices = node.boneInverseBindMatrices;
	renderable.nodeIndex = node.nodeIndex;
	for (size_t i = 0; i < model->meshes.size(); ++i) {
		const bool drawn = node.renderWholeModel ||
			std::find(node.renderMeshIndices.begin(), node.renderMeshIndices.end(), static_cast<uint32_t>(i)) != node.renderMeshIndices.end();
		if (drawn && model->meshes[i].RequiresAlphaBlending()) renderable.hasAlpha = true;
	}
	cm->AddRenderable(entity, renderable);
}

// Culling values follow SceneNode::CullingOverride order.
PropertyDesc CullingProp() {
	PropertyDesc p = Prop("culling", PropType::Enum,
		[](const SceneNode& n) { return PropValue(static_cast<int64_t>(n.GetCullingOverride())); },
		[](SceneNode& n, const PropValue& v) {
			n.SetCullingOverride(static_cast<SceneNode::CullingOverride>(std::get<int64_t>(v)));
			SyncRenderable(n);
			NotifySceneChanged();
			return true;
		});
	p.enumNames = { "inherit", "force_enable", "force_disable", "force_front" };
	p.tooltip = "Per-node culling override applied to the model's meshes";
	return p;
}

void RegisterMeshRenderer() {
	ComponentDesc c;
	c.name = "MeshRenderer";
	c.category = "Rendering";
	c.tooltip = "Draws a model (glTF) at this node. Set the model property to attach one.";
	c.order = 12;
	c.has = [](const SceneNode& n) { return n.GetModel() != nullptr; };
	// No add: a MeshRenderer needs a model, so the editor assigns one through the model property on
	// nodes that already have one.
	c.remove = [](SceneNode& n) {
		if (!n.GetModel()) return false;
		n.SetModel(nullptr);
		SyncRenderable(n);
		NotifySceneChanged();
		return true;
	};

	PropertyDesc model = Prop("model", PropType::AssetPath,
		[](const SceneNode& n) {
			const std::shared_ptr<Scene> m = n.GetModel();
			if (!m || !Services().modelPath) return PropValue(std::string());
			return PropValue(Services().modelPath(*m));
		},
		[](SceneNode& n, const PropValue& v) {
			const std::string& path = std::get<std::string>(v);
			if (path.empty() || !Services().loadModel) return false;
			std::shared_ptr<Scene> loaded = Services().loadModel(path);
			if (!loaded) return false;
			// Skeleton state belongs to the previous model; rebuild it for the new one.
			n.isSkinned = false;
			n.boneNodes.clear();
			n.boneInverseBindMatrices.clear();
			n.SetModel(loaded);
			if (loaded->hasSkin) n.BuildSkeleton(*loaded);
			SyncRenderable(n);
			NotifySceneChanged();
			return true;
		});
	model.assetFilter = { ".glb", ".gltf" };
	model.tooltip = "Logical asset path of the model file";

	// exclude_from_reflection_capture lives on the model object (Scene), which is shared by every node
	// that loaded the same file. It is kept there for now; editing it here edits all those nodes.
	PropertyDesc excludeCapture = Prop("exclude_from_reflection_capture", PropType::Bool,
		[](const SceneNode& n) {
			const std::shared_ptr<Scene> m = n.GetModel();
			return PropValue(m && m->excludeFromReflectionCapture);
		},
		[](SceneNode& n, const PropValue& v) {
			const std::shared_ptr<Scene> m = n.GetModel();
			if (!m) return false;
			m->excludeFromReflectionCapture = std::get<bool>(v);
			NotifySceneChanged();
			return true;
		});
	excludeCapture.tooltip = "Skip this model in reflection probe captures (shared by all nodes using the model)";

	c.properties = {
		model,
		CullingProp(),
		Prop("render_whole_model", PropType::Bool,
			[](const SceneNode& n) { return PropValue(n.renderWholeModel); },
			[](SceneNode& n, const PropValue& v) {
				n.renderWholeModel = std::get<bool>(v);
				SyncRenderable(n);
				NotifySceneChanged();
				return true;
			}),
		Prop("bounding_radius", PropType::Float,
			[](const SceneNode& n) { return PropValue(static_cast<double>(n.boundingRadius)); },
			nullptr, PropNoSave),
		Prop("mesh_count", PropType::Int,
			[](const SceneNode& n) {
				const std::shared_ptr<Scene> m = n.GetModel();
				return PropValue(static_cast<int64_t>(m ? m->meshes.size() : 0));
			}, nullptr, PropNoSave),
		Prop("material_count", PropType::Int,
			[](const SceneNode& n) {
				// Distinct source material names; per-node overrides live in material_overrides.
				const std::shared_ptr<Scene> m = n.GetModel();
				std::set<std::string> names;
				if (m) for (const MeshComponent& mesh : m->meshes) names.insert(mesh.materialName);
				return PropValue(static_cast<int64_t>(names.size()));
			}, nullptr, PropNoSave),
		excludeCapture,
	};
	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterModelComponents() {
	RegisterMeshRenderer();
}

} // namespace nox::reflect
