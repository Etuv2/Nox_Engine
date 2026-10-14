// Reflection registry: built-in descriptors, typed get/set with validation, JSON round trip.
#include "TestFramework.h"

#include "LightNode.h"
#include "PointLight.h"
#include "SceneNode.h"
#include "reflect/Reflection.h"

#include <cmath>
#include <memory>

using namespace nox::reflect;

NOX_TEST(Reflection, BuiltinsRegisterOnceAndCoverEveryNode)
{
	RegisterBuiltinComponents();
	const size_t count = Registry::Get().All().size();
	RegisterBuiltinComponents();
	NOX_CHECK_EQ(Registry::Get().All().size(), count);

	SceneNode node;
	const auto present = Registry::Get().ComponentsOf(node);
	NOX_REQUIRE(present.size() >= 2);
	NOX_CHECK_EQ(present[0]->name, std::string("Node"));
	NOX_CHECK_EQ(present[1]->name, std::string("Transform"));
}

NOX_TEST(Reflection, TransformSetValidatesAndCoerces)
{
	RegisterBuiltinComponents();
	SceneNode node;
	std::string error;
	NOX_CHECK(SetProperty(node, "Transform", "position", PropValue(glm::vec3(1.0f, 2.0f, 3.0f)), &error));
	PropValue value;
	NOX_REQUIRE(GetProperty(node, "Transform", "position", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(1.0f, 2.0f, 3.0f));

	NOX_CHECK(!SetProperty(node, "Transform", "position", PropValue(std::string("x")), &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(!SetProperty(node, "Transform", "world_position", PropValue(glm::vec3(0.0f)), &error));
	NOX_CHECK(!SetProperty(node, "Transform", "nope", PropValue(1.0), &error));
	NOX_CHECK(!SetProperty(node, "Light", "intensity", PropValue(1.0), &error));   // plain node has no Light
	NOX_CHECK(!SetProperty(node, "Transform", "position", PropValue(glm::vec3(NAN, 0.0f, 0.0f)), &error));
}

NOX_TEST(Reflection, LightRoundTripsThroughJson)
{
	RegisterBuiltinComponents();
	LightNode source(std::make_shared<PointLight>());
	NOX_CHECK(SetProperty(source, "Light", "intensity", PropValue(int64_t(4))));   // int coerces to float
	NOX_CHECK(SetProperty(source, "Light", "color", PropValue(glm::vec3(0.5f, 0.25f, 1.0f))));
	NOX_CHECK(SetProperty(source, "Light", "cast_shadows", PropValue(false)));
	NOX_CHECK(!SetProperty(source, "Light", "spot_inner_angle", PropValue(10.0)));   // point light: rejected
	NOX_CHECK(SetProperty(source, "Light", "range", PropValue(-5.0)));               // clamped to the minimum
	PropValue range;
	NOX_REQUIRE(GetProperty(source, "Light", "range", range));
	NOX_CHECK(std::get<double>(range) > 0.0);

	const nlohmann::json saved = SerializeComponents(source);
	NOX_REQUIRE(saved.contains("Light"));
	NOX_CHECK(!saved.contains("Transform"));       // builtin facets are skipped
	NOX_CHECK(!saved["Light"].contains("kind"));   // no-save property

	LightNode target(std::make_shared<PointLight>());
	std::vector<std::string> warnings;
	nlohmann::json withUnknown = saved;
	withUnknown["Light"]["future_key"] = 1;
	withUnknown["FutureComponent"] = nlohmann::json::object();
	DeserializeComponents(target, withUnknown, &warnings);
	NOX_CHECK_EQ(warnings.size(), size_t(2));
	NOX_CHECK(SerializeComponents(target) == saved);
}
