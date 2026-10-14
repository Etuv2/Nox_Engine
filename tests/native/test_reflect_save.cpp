// Scene save/load round trips for data that used to be dropped (GUI elements, LPV volumes, audio
// keys, light shadow settings) and the "components" block of the registry. Headless: no GL device
// (GuiNode itself needs one, so its element JSON is tested as a pure function) and no audio device
// (the audio clips used here never load, and autoplay is off).
#include "reflect/Reflection.h"
#include "TestFramework.h"

#include "AudioNode.h"
#include "DirectionalLight.h"
#include "GuiElementJson.h"
#include "LightNode.h"
#include "PhysicsEngine.h"
#include "PointLight.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "SpotLight.h"
#include "reflect/ReflectServices.h"

#include <cmath>
#include <memory>

using namespace nox::reflect;

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Restores the global engine services on scope exit so tests cannot leak a physics pointer.
struct ServicesGuard {
	PhysicsEngine* physics;
	ServicesGuard() : physics(Services().physics) {}
	~ServicesGuard() { Services().physics = physics; }
};

// Loads a node from its saved JSON into a fresh graph, the way the scene loader restores a subtree.
std::shared_ptr<SceneNode> Restore(SceneLoader& loader, const nlohmann::json& json, SceneGraph& graph) {
	return loader.InstantiateNode(graph, json, graph.GetRoot(), "scenes/_test_reflect_save.json");
}

} // namespace

NOX_TEST(ReflectSave, GuiElementsRoundTripThroughJson)
{
	GuiNode::GuiElement text;
	text.type = GuiNode::GuiType::TEXT;
	text.content = "Lap 1/3";
	text.x = 12.5f;
	text.y = 40.0f;
	text.color = { 10, 20, 30, 200 };

	GuiNode::GuiElement solid;
	solid.type = GuiNode::GuiType::RECT_SOLID;
	solid.x = 1; solid.y = 2; solid.width = 300; solid.height = 24;
	solid.color = { 1, 2, 3, 4 };
	solid.isResizable = true;
	solid.minWidth = 50; solid.minHeight = 8; solid.maxWidth = 900; solid.maxHeight = -1;
	solid.anchor = GuiNode::GuiElement::Anchor::BOTTOM_RIGHT;
	solid.relativePosition = glm::vec2(0.25f, 0.75f);
	solid.useRelativePositioning = true;

	GuiNode::GuiElement gradient;
	gradient.type = GuiNode::GuiType::RECT_GRADIENT;
	gradient.x = 5; gradient.y = 6; gradient.width = 70; gradient.height = 80;
	gradient.gradient.type = GuiNode::GradientType::RADIAL;
	gradient.gradient.startColor = { 255, 0, 0, 255 };
	gradient.gradient.endColor = { 0, 0, 255, 128 };
	gradient.gradient.center = glm::vec2(0.3f, 0.6f);
	gradient.gradient.radius = 2.5f;
	gradient.relativeSize = glm::vec2(0.5f, 0.1f);
	gradient.useRelativeSize = true;

	GuiNode::GuiElement bevel;
	bevel.type = GuiNode::GuiType::RECT_BEVEL;
	bevel.x = 9; bevel.y = 10; bevel.width = 11; bevel.height = 12;
	bevel.color = { 100, 110, 120, 255 };
	bevel.bevel.radius = 4.0f;
	bevel.bevel.bevelSize = 3.0f;
	bevel.bevel.highlightColor = { 255, 255, 255, 90 };
	bevel.bevel.shadowColor = { 0, 0, 0, 60 };

	GuiNode::GuiElement progress;
	progress.type = GuiNode::GuiType::PROGRESS_BAR;
	progress.x = 20; progress.y = 30; progress.width = 400; progress.height = 18;
	progress.progressStyle.backgroundColor = { 9, 9, 9, 255 };
	progress.progressStyle.fillColor = { 200, 180, 20, 255 };
	progress.progressStyle.borderColor = { 1, 1, 1, 255 };
	progress.progressStyle.borderWidth = 3.0f;
	progress.progressStyle.cornerRadius = 5.0f;
	progress.progressStyle.showLabel = false;
	progress.progressStyle.labelFontSize = 12;
	progress.progressValue = 0.4f;
	progress.progressLabel = "fuel";

	const GuiNode::GuiElement* all[] = { &text, &solid, &gradient, &bevel, &progress };
	for (const GuiNode::GuiElement* source : all) {
		const nlohmann::json json = GuiElementToJson(*source);
		GuiNode::GuiElement back;
		std::string kind;
		NOX_REQUIRE(GuiElementFromJson(json, back, &kind));
		NOX_CHECK(back.type == source->type);
		// Relative elements store no derived geometry (see GuiElementToJson): their x/y are resolved per frame.
		if (!source->useRelativePositioning) NOX_CHECK(back.x == source->x && back.y == source->y);
		NOX_CHECK(back.content == source->content);
		const bool usesColor = source->type == GuiNode::GuiType::TEXT || source->type == GuiNode::GuiType::RECT_SOLID
			|| source->type == GuiNode::GuiType::RECT_BEVEL;
		if (usesColor) NOX_CHECK(back.color.r == source->color.r && back.color.a == source->color.a);
		NOX_CHECK(back.isResizable == source->isResizable);
		NOX_CHECK(back.anchor == source->anchor);
		NOX_CHECK(back.useRelativePositioning == source->useRelativePositioning);
		NOX_CHECK(back.useRelativeSize == source->useRelativeSize);
	}

	// Kind-specific fields.
	{
		GuiNode::GuiElement back;
		NOX_REQUIRE(GuiElementFromJson(GuiElementToJson(solid), back));
		NOX_CHECK(back.width == 300.0f && back.height == 24.0f);
		NOX_CHECK(back.isResizable && back.minWidth == 50.0f && back.maxWidth == 900.0f);
		NOX_CHECK(back.anchor == GuiNode::GuiElement::Anchor::BOTTOM_RIGHT);
		NOX_CHECK(back.relativePosition == glm::vec2(0.25f, 0.75f));
	}
	{
		GuiNode::GuiElement back;
		NOX_REQUIRE(GuiElementFromJson(GuiElementToJson(gradient), back));
		NOX_CHECK(back.gradient.type == GuiNode::GradientType::RADIAL);
		NOX_CHECK(back.gradient.endColor.b == 255 && back.gradient.endColor.a == 128);
		NOX_CHECK(back.gradient.center == glm::vec2(0.3f, 0.6f));
		NOX_CHECK(back.gradient.radius == 2.5f);
		NOX_CHECK(back.relativeSize == glm::vec2(0.5f, 0.1f));
	}
	{
		GuiNode::GuiElement back;
		NOX_REQUIRE(GuiElementFromJson(GuiElementToJson(bevel), back));
		NOX_CHECK(back.bevel.radius == 4.0f && back.bevel.bevelSize == 3.0f);
		NOX_CHECK(back.bevel.highlightColor.a == 90 && back.bevel.shadowColor.a == 60);
	}
	{
		GuiNode::GuiElement back;
		NOX_REQUIRE(GuiElementFromJson(GuiElementToJson(progress), back));
		NOX_CHECK(back.progressStyle.fillColor.r == 200 && back.progressStyle.showLabel == false);
		NOX_CHECK(back.progressStyle.labelFontSize == 12 && back.progressStyle.borderWidth == 3.0f);
		NOX_CHECK(Near(back.progressValue, 0.4f));
		NOX_CHECK(back.progressLabel == "fuel");
	}

	// Layout keys are only written when they differ from the defaults.
	const nlohmann::json plain = GuiElementToJson(text);
	NOX_CHECK(!plain.contains("anchor") && !plain.contains("resizable") && !plain.contains("relative_position"));

	// Legacy scene JSON keeps loading with the old defaults.
	GuiNode::GuiElement legacy;
	NOX_REQUIRE(GuiElementFromJson(nlohmann::json{ {"kind", "solid_rect"}, {"x", 3}, {"y", 4} }, legacy));
	NOX_CHECK(legacy.width == 100.0f && legacy.height == 100.0f);
	NOX_CHECK(legacy.color.r == 128 && legacy.color.a == 255);

	NOX_CHECK(!GuiElementFromJson(nlohmann::json{ {"kind", "hologram"} }, legacy));
}

NOX_TEST(ReflectSave, LpvVolumeRoundTripsThroughSceneFormat)
{
	SceneLoader loader(nullptr, nullptr, 1280, 720);
	SceneGraph source;
	auto lpv = std::make_shared<SceneNode>(source.GetComponentManager(), source.GetTransformSystem());
	lpv->SetNodeType(SceneNode::LPV_VOLUME);
	lpv->SetName("gi_volume");
	SceneNode::LPVVolumeData data;
	data.center = glm::vec3(1.0f, 2.0f, 3.0f);
	data.extent = glm::vec3(10.0f, 20.0f, 30.0f);
	data.voxelSize = 0.25f;
	data.gridResolution = 64;
	data.orientation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
	lpv->SetLPVVolumeData(data);

	const nlohmann::json json = loader.SerializeNodeForEditor(lpv);
	NOX_REQUIRE(json.contains("lpv_data"));

	SceneGraph restoredGraph;
	auto restored = Restore(loader, json, restoredGraph);
	NOX_REQUIRE(restored != nullptr);
	NOX_CHECK(restored->GetNodeType() == SceneNode::LPV_VOLUME);
	const auto& back = restored->GetLPVVolumeData();
	NOX_CHECK(back.center == data.center);
	NOX_CHECK(back.extent == data.extent);
	NOX_CHECK(Near(back.voxelSize, 0.25f));
	NOX_CHECK(back.gridResolution == 64);
	NOX_CHECK(Near(glm::dot(back.orientation, data.orientation), 1.0f));

	// The reflection descriptor reads the same values.
	RegisterBuiltinComponents();
	PropValue grid;
	NOX_REQUIRE(GetProperty(*restored, "LightPropagationVolume", "grid_resolution", grid));
	NOX_CHECK(std::get<int64_t>(grid) == 64);
}

NOX_TEST(ReflectSave, AudioKeysRoundTripThroughSceneFormat)
{
	SceneLoader loader(nullptr, nullptr, 1280, 720);
	SceneGraph source;
	auto audio = std::make_shared<AudioNode>();
	audio->SetNodeType(SceneNode::AUDIO);
	audio->SetName("engine_loop");
	audio->setVolume(0.4f);
	audio->setPitch(1.5f);
	audio->setHearingDistance(25.0f);
	audio->setLooping(true);
	audio->setIs3D(false);
	audio->setAutoplay(false);
	audio->initAudio("sounds/_missing_for_test.wav");   // fails to load; the path is still kept

	const nlohmann::json json = loader.SerializeNodeForEditor(audio);
	NOX_CHECK(json.value("sound", "") == "sounds/_missing_for_test.wav");
	NOX_CHECK(json.value("loop", false) == true);
	NOX_CHECK(json.value("autoplay", true) == false);
	NOX_CHECK(json.value("is3d", true) == false);
	NOX_CHECK(Near(json.value("volume", 0.0f), 0.4f));

	SceneGraph restoredGraph;
	auto restored = std::dynamic_pointer_cast<AudioNode>(Restore(loader, json, restoredGraph));
	NOX_REQUIRE(restored != nullptr);
	NOX_CHECK(restored->getSoundFilePath() == "sounds/_missing_for_test.wav");
	NOX_CHECK(restored->isLooping());
	NOX_CHECK(!restored->getAutoplay());
	NOX_CHECK(!restored->is3D());
	NOX_CHECK(!restored->isPlaying());   // autoplay off: the loader does not start it
	NOX_CHECK(Near(restored->getVolume(), 0.4f));
	NOX_CHECK(Near(restored->getPitch(), 1.5f));
}

NOX_TEST(ReflectSave, LightShadowSettingsAreWrittenFromTheLights)
{
	SceneLoader loader(nullptr, nullptr, 1280, 720);
	SceneGraph graph;

	auto dir = std::make_shared<DirectionalLight>();
	dir->InitializeCascades(1024, 0.7f);
	auto dirNode = std::make_shared<LightNode>(dir);
	dirNode->SetNodeType(SceneNode::LIGHT);
	const nlohmann::json dirJson = loader.SerializeNodeForEditor(dirNode);
	NOX_REQUIRE(dirJson.contains("light"));
	NOX_CHECK(dirJson["light"].value("shadowSize", 0) == 1024);
	NOX_CHECK(Near(dirJson["light"].value("splitLambda", 0.0f), 0.7f));

	auto spot = std::make_shared<SpotLight>();
	spot->InitializeShadowMap(512);
	auto spotNode = std::make_shared<LightNode>(spot);
	spotNode->SetNodeType(SceneNode::LIGHT);
	const nlohmann::json spotJson = loader.SerializeNodeForEditor(spotNode);
	NOX_CHECK(spotJson["light"].value("shadowResolution", 0) == 512);

	auto point = std::make_shared<PointLight>();
	point->InitializeShadowMap(256);
	auto pointNode = std::make_shared<LightNode>(point);
	pointNode->SetNodeType(SceneNode::LIGHT);
	const nlohmann::json pointJson = loader.SerializeNodeForEditor(pointNode);
	NOX_CHECK(pointJson["light"].value("shadowResolution", 0) == 256);

	// The shadow values survive a reload.
	SceneGraph restoredGraph;
	auto restored = Restore(loader, dirJson, restoredGraph);
	auto restoredLight = std::dynamic_pointer_cast<LightNode>(restored);
	NOX_REQUIRE(restoredLight != nullptr && restoredLight->GetLight() != nullptr);
	auto restoredDir = std::dynamic_pointer_cast<DirectionalLight>(restoredLight->GetLight());
	NOX_REQUIRE(restoredDir != nullptr);
	NOX_CHECK(restoredDir->GetShadowSize() == 1024);
	NOX_CHECK(Near(restoredDir->GetSplitLambda(), 0.7f));
}

NOX_TEST(ReflectSave, ComponentsBlockAgreesWithLegacyLightKeys)
{
	RegisterBuiltinComponents();
	SceneLoader loader(nullptr, nullptr, 1280, 720);
	auto light = std::make_shared<PointLight>();
	light->SetIntensity(3.5f);
	light->SetColor(glm::vec3(0.2f, 0.4f, 0.6f));
	auto node = std::make_shared<LightNode>(light);
	node->SetNodeType(SceneNode::LIGHT);

	const nlohmann::json json = loader.SerializeNodeForEditor(node);
	NOX_REQUIRE(json.contains("components"));
	NOX_REQUIRE(json["components"].contains("Light"));
	NOX_CHECK(Near(json["components"]["Light"].value("intensity", 0.0f), json["light"].value("intensity", 1.0f)));
	NOX_CHECK(Near(json["components"]["Light"].value("intensity", 0.0f), 3.5f));

	// A registry value wins over the legacy key when both are present.
	nlohmann::json edited = json;
	edited["components"]["Light"]["intensity"] = 7.0;
	SceneGraph graph;
	auto restored = std::dynamic_pointer_cast<LightNode>(Restore(loader, edited, graph));
	NOX_REQUIRE(restored != nullptr && restored->GetLight() != nullptr);
	NOX_CHECK(Near(restored->GetLight()->GetIntensity(), 7.0f));
}

NOX_TEST(ReflectSave, RigidBodyAddAndRemoveNeedPhysics)
{
	RegisterBuiltinComponents();
	ServicesGuard guard;
	const ComponentDesc* rbDesc = Registry::Get().Find("RigidBody");
	const ComponentDesc* colliderDesc = Registry::Get().Find("Collider");
	NOX_REQUIRE(rbDesc != nullptr && colliderDesc != nullptr);
	NOX_REQUIRE(rbDesc->add && rbDesc->remove);

	auto node = std::make_shared<SceneNode>();

	// Without a physics world the add fails and nothing is attached.
	Services().physics = nullptr;
	NOX_CHECK(!rbDesc->add(*node));
	NOX_CHECK(!node->GetRigidBody());

	PhysicsEngine physics;
	Services().physics = &physics;
	NOX_CHECK(!rbDesc->has(*node));
	NOX_REQUIRE(rbDesc->add(*node));
	NOX_REQUIRE(node->GetRigidBody() != nullptr);
	NOX_CHECK(physics.GetBodies().size() == 1);
	NOX_CHECK(colliderDesc->has(*node));

	NOX_CHECK(SetProperty(*node, "RigidBody", "mass", PropValue(2.5)));
	NOX_CHECK(SetProperty(*node, "RigidBody", "body_type", PropValue(int64_t(2))));   // kinematic
	NOX_CHECK(SetProperty(*node, "Collider", "shape", PropValue(int64_t(1))));        // box
	NOX_CHECK(SetProperty(*node, "Collider", "half_extents", PropValue(glm::vec3(1.0f, 2.0f, 3.0f))));
	NOX_CHECK(!SetProperty(*node, "Collider", "half_extents", PropValue(glm::vec3(0.0f, 1.0f, 1.0f))));
	PropValue shape;
	NOX_REQUIRE(GetProperty(*node, "Collider", "shape", shape));
	NOX_CHECK(std::get<int64_t>(shape) == 1);
	PropValue mass;
	NOX_REQUIRE(GetProperty(*node, "RigidBody", "mass", mass));
	NOX_CHECK(std::get<double>(mass) == 2.5);

	NOX_CHECK(rbDesc->remove(*node));
	NOX_CHECK(!node->GetRigidBody());
	NOX_CHECK(physics.GetBodies().empty());
	Services().physics = nullptr;
}

NOX_TEST(ReflectSave, RigidBodyComponentSurvivesSceneRoundTrip)
{
	RegisterBuiltinComponents();
	ServicesGuard guard;
	auto physics = std::make_shared<PhysicsEngine>();
	SceneLoader loader(nullptr, physics, 1280, 720);

	SceneGraph source;
	auto node = std::make_shared<SceneNode>(source.GetComponentManager(), source.GetTransformSystem());
	node->SetName("crate");
	Services().physics = physics.get();
	NOX_REQUIRE(Registry::Get().Find("RigidBody")->add(*node));
	NOX_CHECK(SetProperty(*node, "RigidBody", "mass", PropValue(4.0)));
	NOX_CHECK(SetProperty(*node, "RigidBody", "body_type", PropValue(int64_t(1))));   // dynamic
	NOX_CHECK(SetProperty(*node, "Collider", "radius", PropValue(0.75)));
	Services().physics = nullptr;

	const nlohmann::json json = loader.SerializeNodeForEditor(node);
	NOX_REQUIRE(json.contains("components"));
	NOX_CHECK(json["components"].contains("RigidBody"));
	NOX_CHECK(json["components"]["Collider"].contains("radius"));

	SceneGraph restoredGraph;
	auto restored = Restore(loader, json, restoredGraph);
	NOX_REQUIRE(restored != nullptr);
	auto body = restored->GetRigidBody();
	NOX_REQUIRE(body != nullptr);
	NOX_CHECK(Near(body->getMass(), 4.0f));
	NOX_CHECK(Near(body->getBoundingRadius(), 0.75f));
	NOX_CHECK(body->GetBodyType() == RigidBody::BodyType::DYNAMIC);
}
