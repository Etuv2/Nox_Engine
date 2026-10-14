// Reflection descriptors for MeshRenderer, Camera and Animator: headless (no GL, no model files).
// Nodes are ECS-backed through a bare ComponentManager; models are empty Scene objects.
#include "TestFramework.h"

// Reflection.h first: Scene.h pulls in a second nlohmann json copy (include/JSON), and the first one included
// wins under the shared include guard, so this must match Reflection.cpp.
#include "reflect/Reflection.h"
#include "reflect/ReflectServices.h"

#include "Animation.h"
#include "Camera.h"
#include "ComponentManager.h"
#include "ComponentTypes.h"
#include "Scene.h"
#include "SceneNode.h"

#include <memory>
#include <string>

using namespace nox::reflect;

namespace {

// Restores the engine services a test replaced.
struct ServicesGuard {
	EngineServices saved = Services();
	~ServicesGuard() { Services() = saved; }
};

const ComponentDesc& Desc(const char* name) {
	const ComponentDesc* desc = Registry::Get().Find(name);
	if (!desc) std::abort();
	return *desc;
}

} // namespace

// ---- Camera -----------------------------------------------------------------------------------------

NOX_TEST(ReflectRender, CameraNeedsCameraNodeAndEcsComponent)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto plain = SceneNode::CreateWithECS(&cm, nullptr, "plain", NodeType::NODE);
	auto cam = SceneNode::CreateWithECS(&cm, nullptr, "cam", NodeType::CAMERA);
	NOX_REQUIRE(plain && cam);

	NOX_CHECK(!Desc("Camera").has(*plain));
	NOX_CHECK(!Desc("Camera").add(*plain));       // not a camera node
	NOX_CHECK(!Desc("Camera").has(*cam));
	NOX_CHECK(Desc("Camera").add(*cam));
	NOX_CHECK(Desc("Camera").has(*cam));
	NOX_CHECK(!Desc("Camera").add(*cam));         // already present
	NOX_CHECK(cam->GetCameraComponent() != nullptr);

	// A camera node without an ECS entity cannot carry the component.
	SceneNode bare;
	bare.SetNodeType(SceneNode::CAMERA);
	NOX_CHECK(!Desc("Camera").add(bare));
	NOX_CHECK(!Desc("Camera").has(bare));

	NOX_CHECK(Desc("Camera").remove(*cam));
	NOX_CHECK(!Desc("Camera").has(*cam));
	NOX_CHECK(!Desc("Camera").remove(*cam));
}

NOX_TEST(ReflectRender, CameraPropertiesValidateAndRoundTrip)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto cam = SceneNode::CreateWithECS(&cm, nullptr, "cam", NodeType::CAMERA);
	NOX_REQUIRE(cam && Desc("Camera").add(*cam));

	NOX_CHECK(SetProperty(*cam, "Camera", "fov", PropValue(60.0)));
	PropValue value;
	NOX_REQUIRE(GetProperty(*cam, "Camera", "fov", value));
	NOX_CHECK(std::abs(std::get<double>(value) - 60.0) < 1e-6);

	NOX_CHECK(SetProperty(*cam, "Camera", "fov", PropValue(500.0)));   // clamped to 179
	NOX_REQUIRE(GetProperty(*cam, "Camera", "fov", value));
	NOX_CHECK(std::abs(std::get<double>(value) - 179.0) < 1e-6);

	NOX_CHECK(!SetProperty(*cam, "Camera", "near_plane", PropValue(200.0)));  // would pass the far plane
	NOX_CHECK(!SetProperty(*cam, "Camera", "far_plane", PropValue(0.05)));    // would pass the near plane
	NOX_CHECK(SetProperty(*cam, "Camera", "far_plane", PropValue(250.0)));
	NOX_CHECK(SetProperty(*cam, "Camera", "near_plane", PropValue(0.5)));
	NOX_CHECK(std::abs(cam->GetCameraComponent()->nearPlane - 0.5f) < 1e-6f);
	NOX_CHECK(std::abs(cam->GetCameraComponent()->farPlane - 250.0f) < 1e-6f);

	NOX_CHECK(SetProperty(*cam, "Camera", "projection", PropValue(int64_t(1))));
	NOX_REQUIRE(GetProperty(*cam, "Camera", "projection", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(1));
	NOX_CHECK(!SetProperty(*cam, "Camera", "projection", PropValue(int64_t(7))));   // enum index out of range
	NOX_CHECK(!SetProperty(*cam, "Camera", "aspect_ratio", PropValue(1.0)));        // read-only

	const nlohmann::json saved = SerializeComponents(*cam);
	NOX_REQUIRE(saved.contains("Camera"));
	NOX_CHECK_EQ(saved["Camera"]["projection"].get<std::string>(), std::string("orthographic"));
	NOX_CHECK(saved["Camera"].contains("fov"));
	NOX_CHECK(!saved["Camera"].contains("aspect_ratio"));   // runtime state is not saved

	// The saved values come back on a fresh camera node.
	auto copy = SceneNode::CreateWithECS(&cm, nullptr, "copy", NodeType::CAMERA);
	NOX_REQUIRE(copy);
	std::vector<std::string> warnings;
	DeserializeComponents(*copy, saved, &warnings);
	NOX_CHECK(warnings.empty());
	NOX_REQUIRE(GetProperty(*copy, "Camera", "projection", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(1));
}

// ---- MeshRenderer -----------------------------------------------------------------------------------

NOX_TEST(ReflectRender, MeshRendererIsAbsentWithoutModel)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto plain = SceneNode::CreateWithECS(&cm, nullptr, "plain", NodeType::NODE);
	NOX_REQUIRE(plain);

	NOX_CHECK(!Desc("MeshRenderer").has(*plain));
	NOX_CHECK(Desc("MeshRenderer").add == nullptr);   // needs a model; assigned through the model property
	NOX_CHECK(!Desc("MeshRenderer").remove(*plain));
	PropValue value;
	NOX_CHECK(!GetProperty(*plain, "MeshRenderer", "culling", value));
	NOX_CHECK(!SerializeComponents(*plain).contains("MeshRenderer"));
}

NOX_TEST(ReflectRender, MeshRendererModelPathNeedsLoaderAndExistingModel)
{
	RegisterBuiltinComponents();
	ServicesGuard guard;
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);

	// The model property only exists on nodes that already have a model: a bare node is refused.
	Services().loadModel = [](const std::string&) { return std::make_shared<Scene>(); };
	std::string error;
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "model", PropValue(std::string("props/crate.glb")), &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(node->GetModel() == nullptr);

	node->SetModel(std::make_shared<Scene>());
	NOX_REQUIRE(Desc("MeshRenderer").has(*node));

	Services().loadModel = nullptr;
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "model", PropValue(std::string("props/crate.glb"))));

	Services().loadModel = [](const std::string&) { return std::shared_ptr<Scene>(); };
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "model", PropValue(std::string("props/missing.glb"))));

	Services().loadModel = [](const std::string&) { return std::make_shared<Scene>(); };
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "model", PropValue(std::string(""))));   // empty path
	NOX_CHECK(SetProperty(*node, "MeshRenderer", "model", PropValue(std::string("props/crate.glb"))));
	NOX_CHECK(Desc("MeshRenderer").has(*node));

	PropValue path;
	NOX_REQUIRE(GetProperty(*node, "MeshRenderer", "model", path));
	NOX_CHECK(std::get<std::string>(path).empty());   // no reverse path service in tests

	Services().modelPath = [](const Scene&) { return std::string("props/crate.glb"); };
	NOX_REQUIRE(GetProperty(*node, "MeshRenderer", "model", path));
	NOX_CHECK_EQ(std::get<std::string>(path), std::string("props/crate.glb"));
	Services().modelPath = nullptr;
}

NOX_TEST(ReflectRender, MeshRendererSettingsRoundTripOnModelNode)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);
	node->SetModel(std::make_shared<Scene>());
	NOX_REQUIRE(Desc("MeshRenderer").has(*node));

	NOX_CHECK(SetProperty(*node, "MeshRenderer", "culling", PropValue(int64_t(2))));   // force_disable
	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "MeshRenderer", "culling", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(2));
	NOX_CHECK(node->GetCullingOverride() == SceneNode::CULLING_FORCE_DISABLE);
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "culling", PropValue(int64_t(4))));

	NOX_CHECK(SetProperty(*node, "MeshRenderer", "render_whole_model", PropValue(false)));
	NOX_CHECK(!node->renderWholeModel);
	NOX_CHECK(SetProperty(*node, "MeshRenderer", "exclude_from_reflection_capture", PropValue(true)));
	NOX_CHECK(node->GetModel()->excludeFromReflectionCapture);

	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "bounding_radius", PropValue(2.0)));   // read-only
	NOX_CHECK(!SetProperty(*node, "MeshRenderer", "mesh_count", PropValue(int64_t(3))));   // read-only
	NOX_REQUIRE(GetProperty(*node, "MeshRenderer", "mesh_count", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(0));
	NOX_REQUIRE(GetProperty(*node, "MeshRenderer", "material_count", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(0));

	// Saved: the editable settings. Not saved: runtime-derived values.
	const nlohmann::json saved = SerializeComponents(*node);
	NOX_REQUIRE(saved.contains("MeshRenderer"));
	NOX_CHECK_EQ(saved["MeshRenderer"]["culling"].get<std::string>(), std::string("force_disable"));
	NOX_CHECK_EQ(saved["MeshRenderer"]["exclude_from_reflection_capture"].get<bool>(), true);
	NOX_CHECK(!saved["MeshRenderer"].contains("mesh_count"));
	NOX_CHECK(!saved["MeshRenderer"].contains("bounding_radius"));
	NOX_CHECK(!saved["MeshRenderer"].contains("material_count"));
}

NOX_TEST(ReflectRender, MeshRendererRemoveDropsModel)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);
	node->SetModel(std::make_shared<Scene>());

	NOX_CHECK(Desc("MeshRenderer").remove(*node));
	NOX_CHECK(node->GetModel() == nullptr);
	NOX_CHECK(!Desc("MeshRenderer").has(*node));
	NOX_CHECK(!Desc("MeshRenderer").remove(*node));
}

// ---- Animator ---------------------------------------------------------------------------------------

NOX_TEST(ReflectRender, AnimatorMethodsFailCleanlyWithoutController)
{
	RegisterBuiltinComponents();
	ServicesGuard guard;
	Services().sceneGraph = nullptr;

	// Plain node: no Animator facet at all.
	SceneNode bare;
	std::string error;
	PropValue result;
	NOX_CHECK(!Desc("Animator").has(bare));
	NOX_CHECK(!InvokeMethod(bare, "Animator", "play", { PropValue(std::string("walk")) }, result, &error));
	NOX_CHECK(!error.empty());

	// ECS node with no AnimationComponent.
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);
	NOX_CHECK(!Desc("Animator").has(*node));
	NOX_CHECK(!Desc("Animator").add(*node));   // no model with animations

	// An AnimationComponent without a model or scene graph: present, but nothing can play.
	cm.AddAnimation(node->GetEntityID());
	NOX_REQUIRE(Desc("Animator").has(*node));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "play", { PropValue(std::string("walk")) }, result));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "play_index", { PropValue(int64_t(0)) }, result));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "stop", {}, result));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "pause", {}, result));   // not playing
	NOX_CHECK(!InvokeMethod(*node, "Animator", "resume", {}, result));  // not paused
	NOX_CHECK(!InvokeMethod(*node, "Animator", "set_morph_weight", { PropValue(int64_t(0)), PropValue(0.5) }, result));

	// Wrong argument count and unknown method are rejected before any work.
	NOX_CHECK(!InvokeMethod(*node, "Animator", "play", {}, result));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "fly", {}, result));
}

NOX_TEST(ReflectRender, AnimatorPropertiesRoundTripAndClipsFromModel)
{
	RegisterBuiltinComponents();
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);
	cm.AddAnimation(node->GetEntityID());

	// The clip list comes from the renderable's model.
	auto model = std::make_shared<Scene>();
	Animation idle;
	idle.name = "idle";
	Animation walk;
	walk.name = "walk";
	walk.isLooping = true;
	model->animations = { idle, walk };
	RenderableComponent renderable;
	renderable.model = model;
	cm.AddRenderable(node->GetEntityID(), renderable);

	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Animator", "clip_count", value));
	NOX_CHECK_EQ(std::get<int64_t>(value), int64_t(2));
	NOX_CHECK(!SetProperty(*node, "Animator", "clip", PropValue(std::string("run"))));   // unknown clip
	NOX_CHECK(SetProperty(*node, "Animator", "clip", PropValue(std::string("walk"))));
	NOX_REQUIRE(GetProperty(*node, "Animator", "clip", value));
	NOX_CHECK_EQ(std::get<std::string>(value), std::string("walk"));
	NOX_CHECK(cm.GetAnimation(node->GetEntityID())->currentAnimationIndex == 1);

	// loop is stored on the clip the model shares.
	NOX_REQUIRE(GetProperty(*node, "Animator", "loop", value));
	NOX_CHECK(std::get<bool>(value));
	NOX_CHECK(SetProperty(*node, "Animator", "loop", PropValue(false)));
	NOX_CHECK(!model->animations[1].isLooping);

	NOX_CHECK(SetProperty(*node, "Animator", "speed", PropValue(2.5)));
	NOX_REQUIRE(GetProperty(*node, "Animator", "speed", value));
	NOX_CHECK(std::abs(std::get<double>(value) - 2.5) < 1e-9);
	NOX_CHECK(SetProperty(*node, "Animator", "speed", PropValue(50.0)));   // clamped to the range
	NOX_REQUIRE(GetProperty(*node, "Animator", "speed", value));
	NOX_CHECK(std::get<double>(value) <= 10.0 + 1e-9);

	NOX_CHECK(SetProperty(*node, "Animator", "time", PropValue(1.5)));
	NOX_CHECK(SetProperty(*node, "Animator", "time", PropValue(-3.0)));
	NOX_REQUIRE(GetProperty(*node, "Animator", "time", value));
	NOX_CHECK(std::get<double>(value) == 0.0);

	NOX_CHECK(!SetProperty(*node, "Animator", "playing", PropValue(true)));   // read-only
	NOX_REQUIRE(GetProperty(*node, "Animator", "playing", value));
	NOX_CHECK(!std::get<bool>(value));

	// Saved fields: clip, speed, loop. Runtime state stays out of the scene file.
	const nlohmann::json saved = SerializeComponents(*node);
	NOX_REQUIRE(saved.contains("Animator"));
	NOX_CHECK_EQ(saved["Animator"]["clip"].get<std::string>(), std::string("walk"));
	NOX_CHECK_EQ(saved["Animator"]["loop"].get<bool>(), false);
	NOX_CHECK(!saved["Animator"].contains("playing"));
	NOX_CHECK(!saved["Animator"].contains("time"));
	NOX_CHECK(!saved["Animator"].contains("clip_count"));
}

NOX_TEST(ReflectRender, AnimatorMorphWeightRejectsOutOfRangeTargets)
{
	RegisterBuiltinComponents();
	ServicesGuard guard;
	Services().sceneGraph = nullptr;
	ComponentManager cm;
	auto node = SceneNode::CreateWithECS(&cm, nullptr, "node", NodeType::MODEL);
	NOX_REQUIRE(node);
	cm.AddAnimation(node->GetEntityID());

	auto model = std::make_shared<Scene>();
	RenderableComponent renderable;
	renderable.model = model;
	cm.AddRenderable(node->GetEntityID(), renderable);

	// No mesh morph targets: every index is out of range.
	PropValue result;
	NOX_CHECK(!InvokeMethod(*node, "Animator", "set_morph_weight", { PropValue(int64_t(0)), PropValue(0.5) }, result));
	NOX_CHECK(!InvokeMethod(*node, "Animator", "set_morph_weight", { PropValue(int64_t(-1)), PropValue(0.5) }, result));
}
