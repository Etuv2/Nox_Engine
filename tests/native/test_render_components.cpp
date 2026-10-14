// Render components: CPU particle simulation (headless), the WorldEnvironment settings through the registry
// with a stub RenderContext, the render_settings JSON round trip, Decal and ParticleEmitter add/remove and
// SerializeComponents round trips, and the Camera.current node driving the view.
#include "reflect/Reflection.h"
#include "TestFramework.h"

#include "Camera.h"
#include "FrameBuffer.h"
#include "ComponentTypes.h"
#include "RenderContext.h"
#include "ScreenQuad.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "SceneRenderSettings.h"
#include "reflect/ReflectServices.h"
#include "render/ActiveCamera.h"
#include "render/DecalComponent.h"
#include "render/ParticleSystem.h"
#include "render/RenderSettingsTable.h"

#include <glm/gtc/epsilon.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <set>

using namespace nox::reflect;
using nox::render::ParticleEmitterSettings;
using nox::render::ParticleSystem;

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

const glm::mat4 kIdentity(1.0f);

// Restores the global services so one test cannot leak a stub graph or render context into another.
struct ServicesGuard {
	std::function<SceneGraph*()> graph;
	std::function<RenderContext*()> render;
	ServicesGuard() : graph(Services().sceneGraph), render(Services().renderContext) {}
	~ServicesGuard() {
		Services().sceneGraph = graph;
		Services().renderContext = render;
	}
};

// Runs n steps of dt through Update with the given settings.
void Run(ParticleSystem& system, const ParticleEmitterSettings& s, float dt, int steps) {
	for (int i = 0; i < steps; ++i) system.Update(dt, kIdentity, s);
}

} // namespace

// ---- ParticleSystem (headless) ---------------------------------------------------------------------

NOX_TEST(RenderComponents, EmissionCountFollowsRateOverTime)
{
	ParticleEmitterSettings s;
	s.rate = 10.0f;
	s.lifetimeMin = s.lifetimeMax = 100.0f;   // nothing expires during the run
	s.maxParticles = 10000;
	ParticleSystem system(7u);
	Run(system, s, 1.0f / 60.0f, 180);        // 3 seconds
	NOX_CHECK(system.TotalSpawned() >= 29 && system.TotalSpawned() <= 31);
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(system.TotalSpawned()));
}

NOX_TEST(RenderComponents, ParticlesExpireAfterTheirLifetime)
{
	ParticleEmitterSettings s;
	s.rate = 20.0f;
	s.lifetimeMin = s.lifetimeMax = 0.5f;
	ParticleSystem system(3u);
	Run(system, s, 1.0f / 60.0f, 60);         // one second of emission
	NOX_CHECK(system.Count() > 0);

	s.emitting = false;
	Run(system, s, 1.0f / 60.0f, 40);         // 0.67 s without emission: every particle is past 0.5 s
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(0));
	for (const nox::render::Particle& p : system.Particles()) NOX_CHECK(p.age < p.lifetime);
}

NOX_TEST(RenderComponents, BurstSpawnsExactlyAndRespectsTheCap)
{
	ParticleEmitterSettings s;
	s.emitting = false;
	s.maxParticles = 5;
	ParticleSystem system(11u);
	system.Burst(3, kIdentity, s);
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(3));
	system.Burst(10, kIdentity, s);            // only two more fit under the cap
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(5));
	system.Restart();
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(0));
	NOX_CHECK_EQ(system.TotalSpawned(), static_cast<uint64_t>(0));
}

NOX_TEST(RenderComponents, MaxParticlesCapsEmission)
{
	ParticleEmitterSettings s;
	s.rate = 1000.0f;
	s.lifetimeMin = s.lifetimeMax = 10.0f;
	s.maxParticles = 50;
	ParticleSystem system(5u);
	for (int i = 0; i < 60; ++i) {
		system.Update(1.0f / 60.0f, kIdentity, s);
		NOX_CHECK(system.Count() <= 50);
	}
	NOX_CHECK_EQ(system.Count(), static_cast<size_t>(50));
}

NOX_TEST(RenderComponents, LongFramesDoNotFloodEmission)
{
	ParticleEmitterSettings s;
	s.rate = 10.0f;
	s.lifetimeMin = s.lifetimeMax = 10.0f;
	ParticleSystem system(9u);
	system.Update(10.0f, kIdentity, s);        // a 10 s hitch is clamped to 0.1 s: one particle
	NOX_CHECK_EQ(system.TotalSpawned(), static_cast<uint64_t>(1));
}

NOX_TEST(RenderComponents, WorldSpaceParticlesStayAtSpawn)
{
	ParticleEmitterSettings s;
	s.emitting = false;
	s.worldSpace = true;
	s.startSpeed = 0.0f;
	ParticleSystem system(2u);
	glm::mat4 at(1.0f);
	at[3] = glm::vec4(10.0f, 0.0f, 0.0f, 1.0f);
	system.Burst(1, at, s);
	glm::mat4 moved(1.0f);
	moved[3] = glm::vec4(50.0f, 0.0f, 0.0f, 1.0f);
	system.Update(0.1f, moved, s);
	NOX_REQUIRE(system.Count() == 1);
	NOX_CHECK(Near(system.Particles()[0].position.x, 10.0f));
}

// ---- WorldEnvironment ------------------------------------------------------------------------------

NOX_TEST(RenderComponents, WorldEnvironmentIsOnlyOnTheSceneRoot)
{
	ServicesGuard guard;
	RegisterBuiltinComponents();
	SceneGraph graph;
	RenderContext ctx;
	Services().sceneGraph = [&graph] { return &graph; };
	Services().renderContext = [&ctx] { return &ctx; };

	const ComponentDesc* env = Registry::Get().Find("WorldEnvironment");
	NOX_REQUIRE(env != nullptr);
	NOX_CHECK(env->has(*graph.GetRoot()));
	SceneNode other;
	NOX_CHECK(!env->has(other));
	NOX_CHECK(!env->add);
	NOX_CHECK(!env->remove);
}

NOX_TEST(RenderComponents, WorldEnvironmentReadsAndWritesTheLiveSettings)
{
	ServicesGuard guard;
	RegisterBuiltinComponents();
	SceneGraph graph;
	RenderContext ctx;
	Services().sceneGraph = [&graph] { return &graph; };
	Services().renderContext = [&ctx] { return &ctx; };
	SceneNode& root = *graph.GetRoot();

	PropValue value;
	NOX_REQUIRE(GetProperty(root, "WorldEnvironment", "bloom_strength", value));
	NOX_CHECK(Near(static_cast<float>(std::get<double>(value)), ctx.bloomStrength));

	std::string error;
	NOX_CHECK(SetProperty(root, "WorldEnvironment", "bloom_strength", PropValue(1.25), &error));
	NOX_CHECK(Near(ctx.bloomStrength, 1.25f));
	NOX_CHECK(SetProperty(root, "WorldEnvironment", "bloom_strength", PropValue(99.0), &error));   // clamped
	NOX_CHECK(Near(ctx.bloomStrength, 4.0f));

	NOX_CHECK(SetProperty(root, "WorldEnvironment", "shadow_tint", PropValue(glm::vec3(0.1f, 0.2f, 0.3f)), &error));
	NOX_CHECK(glm::all(glm::epsilonEqual(ctx.gradeShadowTint, glm::vec3(0.1f, 0.2f, 0.3f), 1e-6f)));

	// tonemap is an enum: index 1 = "aces"; out-of-range and wrong-type values are rejected.
	NOX_CHECK(SetProperty(root, "WorldEnvironment", "tonemap", PropValue(int64_t(1)), &error));
	NOX_CHECK(ctx.tonemapType == RenderContext::TonemapType::ACES);
	NOX_CHECK(!SetProperty(root, "WorldEnvironment", "tonemap", PropValue(int64_t(9)), &error));
	NOX_CHECK(!SetProperty(root, "WorldEnvironment", "bloom", PropValue(1.0), &error));

	// Not on a child node: the setter refuses, so only the root edits the scene.
	auto child = std::make_shared<SceneNode>();
	root.AddChild(child);
	NOX_CHECK(!SetProperty(*child, "WorldEnvironment", "bloom_strength", PropValue(0.5), &error));
	NOX_CHECK(Near(ctx.bloomStrength, 4.0f));

	// Every property is PropNoSave: the root's components block never carries render settings.
	const nlohmann::json saved = SerializeComponents(root);
	NOX_CHECK(!saved.contains("WorldEnvironment") || saved["WorldEnvironment"].empty());
}

NOX_TEST(RenderComponents, RenderSettingTableKeysAreUniqueAndFindable)
{
	std::set<std::string> keys;
	for (const nox::render::RenderSetting& s : nox::render::RenderSettingTable()) {
		NOX_CHECK(keys.insert(s.key).second);   // unique scene keys
		NOX_CHECK(nox::render::FindRenderSetting(s.key) == &s);
	}
	NOX_CHECK(nox::render::FindRenderSetting("bloom_strength") != nullptr);
	NOX_CHECK(nox::render::FindRenderSetting("no_such_key") == nullptr);
}

NOX_TEST(RenderComponents, RenderSettingsJsonRoundTripsThroughApply)
{
	RenderContext source;
	source.bloomStrength = 1.25f;
	source.gradeShadowTint = glm::vec3(0.1f, 0.2f, 0.3f);
	source.tonemapType = RenderContext::TonemapType::GT7;
	source.exposureMode = 1;
	source.bloomMode = 1;
	source.enableSSR = true;
	source.ssrMaxSteps = 80;
	source.fogDensity = 0.02f;
	source.postDebugView = 3;

	const nlohmann::json written = SceneRenderSettings::ToJson(source, nlohmann::json::object());
	// Defaults are not written; changed values are, with their scene spelling.
	NOX_CHECK(!written.contains("contrast"));
	NOX_CHECK(written.contains("bloom_strength"));
	NOX_CHECK_EQ(written["tonemap"].get<std::string>(), std::string("gt7"));
	NOX_CHECK_EQ(written["exposure_mode"].get<std::string>(), std::string("auto"));
	NOX_CHECK_EQ(written["debug_view"].get<std::string>(), std::string("ssr"));
	NOX_CHECK(Near(written["bloom_strength"].get<float>(), 1.25f, 1e-5f));

	RenderContext loaded;
	SceneRenderSettings::Apply(loaded, written);
	NOX_CHECK(Near(loaded.bloomStrength, 1.25f));
	NOX_CHECK(glm::all(glm::epsilonEqual(loaded.gradeShadowTint, glm::vec3(0.1f, 0.2f, 0.3f), 1e-5f)));
	NOX_CHECK(loaded.tonemapType == RenderContext::TonemapType::GT7);
	NOX_CHECK_EQ(loaded.exposureMode, 1);
	NOX_CHECK_EQ(loaded.bloomMode, 1);
	NOX_CHECK(loaded.enableSSR);
	NOX_CHECK_EQ(loaded.ssrMaxSteps, 80);
	NOX_CHECK(Near(loaded.fogDensity, 0.02f));
	NOX_CHECK_EQ(loaded.postDebugView, 3);
}

NOX_TEST(RenderComponents, RenderSettingsJsonKeepsExistingKeys)
{
	RenderContext ctx;
	nlohmann::json existing = { { "quality_tier", "high" }, { "bloom_strength", 0.5 } };
	ctx.bloomStrength = 0.5f;
	const nlohmann::json written = SceneRenderSettings::ToJson(ctx, existing);
	NOX_CHECK_EQ(written["quality_tier"].get<std::string>(), std::string("high"));
	NOX_CHECK(Near(written["bloom_strength"].get<float>(), 0.5f));
}

// ---- Decal and ParticleEmitter components ----------------------------------------------------------

NOX_TEST(RenderComponents, ParticleEmitterAddSetBurstRemoveRoundTrip)
{
	RegisterBuiltinComponents();
	SceneNode node;
	const ComponentDesc* desc = Registry::Get().Find("ParticleEmitter");
	NOX_REQUIRE(desc != nullptr);
	NOX_CHECK(!desc->has(node));
	NOX_REQUIRE(desc->add(node));
	NOX_CHECK(desc->has(node));
	NOX_CHECK(!desc->add(node));   // already present

	std::string error;
	NOX_CHECK(SetProperty(node, "ParticleEmitter", "rate", PropValue(42.0), &error));
	NOX_CHECK(SetProperty(node, "ParticleEmitter", "blend", PropValue(int64_t(1)), &error));
	NOX_CHECK(SetProperty(node, "ParticleEmitter", "texture", PropValue(std::string("textures/spark.png")), &error));
	NOX_CHECK(SetProperty(node, "ParticleEmitter", "lifetime_max", PropValue(5.0), &error));
	NOX_CHECK(SetProperty(node, "ParticleEmitter", "lifetime_min", PropValue(3.0), &error));
	PropValue lifetimeMin;
	NOX_REQUIRE(GetProperty(node, "ParticleEmitter", "lifetime_min", lifetimeMin));
	NOX_CHECK(std::get<double>(lifetimeMin) == 3.0);

	PropValue burstResult;
	NOX_REQUIRE(InvokeMethod(node, "ParticleEmitter", "burst", { PropValue(int64_t(4)) }, burstResult, &error));
	PropValue count;
	NOX_REQUIRE(GetProperty(node, "ParticleEmitter", "particle_count", count));
	NOX_CHECK(std::get<int64_t>(count) == 4);

	const nlohmann::json saved = SerializeComponents(node);
	NOX_REQUIRE(saved.contains("ParticleEmitter"));
	NOX_CHECK(saved["ParticleEmitter"]["rate"].get<double>() == 42.0);
	NOX_CHECK(saved["ParticleEmitter"]["blend"].get<std::string>() == "additive");
	NOX_CHECK(saved["ParticleEmitter"]["texture"].get<std::string>() == "textures/spark.png");
	NOX_CHECK(!saved["ParticleEmitter"].contains("particle_count"));   // runtime state is not saved

	SceneNode restored;
	DeserializeComponents(restored, saved);
	NOX_REQUIRE(desc->has(restored));
	PropValue rate;
	NOX_REQUIRE(GetProperty(restored, "ParticleEmitter", "rate", rate));
	NOX_CHECK(std::get<double>(rate) == 42.0);

	NOX_CHECK(desc->remove(node));
	NOX_CHECK(!desc->has(node));
	NOX_CHECK(!desc->remove(node));
}

NOX_TEST(RenderComponents, DecalAddSetRemoveRoundTrip)
{
	RegisterBuiltinComponents();
	SceneNode node;
	const ComponentDesc* desc = Registry::Get().Find("Decal");
	NOX_REQUIRE(desc != nullptr);
	NOX_REQUIRE(desc->add(node));

	std::string error;
	NOX_CHECK(SetProperty(node, "Decal", "texture", PropValue(std::string("textures/road_decals/arrow.png")), &error));
	NOX_CHECK(SetProperty(node, "Decal", "size", PropValue(glm::vec3(2.0f, 0.2f, 3.0f)), &error));
	NOX_CHECK(!SetProperty(node, "Decal", "size", PropValue(glm::vec3(0.0f, 0.2f, 3.0f)), &error));   // zero extent
	NOX_CHECK(SetProperty(node, "Decal", "color", PropValue(glm::vec4(1.0f, 0.5f, 0.25f, 0.8f)), &error));
	NOX_CHECK(SetProperty(node, "Decal", "opacity", PropValue(0.5), &error));

	const nlohmann::json saved = SerializeComponents(node);
	NOX_REQUIRE(saved.contains("Decal"));
	NOX_CHECK(saved["Decal"]["texture"].get<std::string>() == "textures/road_decals/arrow.png");
	NOX_CHECK(saved["Decal"]["opacity"].get<double>() == 0.5);

	SceneNode restored;
	DeserializeComponents(restored, saved);
	NOX_REQUIRE(desc->has(restored));
	const auto decal = restored.GetDecal();
	NOX_REQUIRE(decal != nullptr);
	NOX_CHECK(decal->texture == "textures/road_decals/arrow.png");
	NOX_CHECK(glm::all(glm::epsilonEqual(decal->size, glm::vec3(2.0f, 0.2f, 3.0f), 1e-5f)));
	NOX_CHECK(Near(decal->opacity, 0.5f));

	NOX_CHECK(desc->remove(node));
	NOX_CHECK(!desc->has(node));
	NOX_CHECK(node.GetDecal() == nullptr);
}

// ---- Camera.current --------------------------------------------------------------------------------

NOX_TEST(RenderComponents, CurrentCameraNodeDrivesTheRenderCamera)
{
	RegisterBuiltinComponents();
	SceneGraph graph;
	auto cam = SceneNode::CreateWithECS(graph.GetComponentManager(), graph.GetTransformSystem(), "Cam", NodeType::CAMERA);
	NOX_REQUIRE(cam != nullptr);
	graph.GetRoot()->AddChild(cam);

	const ComponentDesc* desc = Registry::Get().Find("Camera");
	NOX_REQUIRE(desc != nullptr);
	NOX_REQUIRE(desc->add(*cam));
	std::string error;
	NOX_CHECK(SetProperty(*cam, "Camera", "fov", PropValue(60.0), &error));
	NOX_CHECK(SetProperty(*cam, "Camera", "current", PropValue(false), &error));

	Camera render(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f, 45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
	NOX_CHECK(!nox::render::ApplyActiveCamera(graph, render));   // no current camera: untouched
	NOX_CHECK(nox::render::FindActiveCameraNode(*graph.GetRoot()) == nullptr);

	NOX_CHECK(SetProperty(*cam, "Camera", "current", PropValue(true), &error));
	cam->SetPosition(glm::vec3(3.0f, 2.0f, 1.0f));
	NOX_CHECK(nox::render::FindActiveCameraNode(*graph.GetRoot()) == cam.get());
	NOX_CHECK(nox::render::ApplyActiveCamera(graph, render));
	NOX_CHECK(Near(render.GetCameraFov(), 60.0f));
	NOX_CHECK(glm::all(glm::epsilonEqual(render.GetCameraPosition(), glm::vec3(3.0f, 2.0f, 1.0f), 1e-5f)));
}
