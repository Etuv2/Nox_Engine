// Physics level benchmark, not part of the default run. NOX_PHYSICS_BENCH=1 prints, for two levels made of mesh files
// with a StaticCollision component on the level and one character walking on it: the load time (glTF parse and BVH
// build of every file, cache cleared first), the first StaticCollision sync (one body per file, from the cache) and the
// physics step time (PhysicsEngine::Update) averaged over a 600-frame walk. Headless: no GPU.
#include "TestFramework.h"

#include "CollisionWorld.h"
#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "physics/CharacterSystem.h"
#include "physics/MeshCollider.h"
#include "reflect/ReflectServices.h"
#include "reflect/Reflection.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using nox::reflect::Registry;
using nox::reflect::Services;

namespace {

const float kDt = 1.0f / 60.0f;

struct ServicesGuard {
	PhysicsEngine* physics;
	ServicesGuard() : physics(Services().physics) {}
	~ServicesGuard() { Services().physics = physics; }
};

struct TestScene {
	SceneGraph graph;

	std::shared_ptr<SceneNode> Make(const char* name, const glm::vec3& pos, const std::shared_ptr<SceneNode>& parent = nullptr) {
		auto node = std::make_shared<SceneNode>(graph.GetComponentManager(), graph.GetTransformSystem());
		node->SetName(name);
		(parent ? parent : graph.GetRoot())->AddChild(node);
		node->SetECSContext(graph.GetComponentManager(), graph.GetTransformSystem());
		node->CreateECSEntity(name);
		node->SetPosition(pos);
		return node;
	}
};

struct LevelBench {
	std::string name;
	size_t meshes = 0;
	size_t triangles = 0;
	double loadMs = 0.0;      // parse and BVH build of every file (cache cleared first)
	double createMs = 0.0;    // first StaticCollision sync: one body per file, from the cache
	double avgStepMs = 0.0;   // PhysicsEngine::Update, averaged over the walk
	double maxStepMs = 0.0;
	int frames = 0;
	float walked = 0.0f;           // metres moved along +X by the character
	double onFloorFraction = 0.0;  // share of frames the character reported is_on_floor
};

double MillisSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

LevelBench BenchLevel(const std::string& name, const std::vector<std::string>& files) {
	LevelBench out;
	out.name = name;
	nox::reflect::RegisterBuiltinComponents();
	ServicesGuard guard;
	PhysicsEngine physics;
	Services().physics = &physics;
	std::map<const Scene*, std::string> paths;
	physics.SetModelPathResolver([&paths](const Scene& model) {
		auto it = paths.find(&model);
		return it == paths.end() ? std::string() : it->second;
	});

	MeshCollider::ClearCache();
	auto t0 = std::chrono::steady_clock::now();
	for (const std::string& file : files) {
		auto mesh = MeshCollider::LoadShared(file);
		if (mesh) {
			out.meshes++;
			out.triangles += mesh->TriangleCount();
		}
	}
	out.loadMs = MillisSince(t0);

	TestScene scene;
	auto level = scene.Make("level", glm::vec3(0.0f));
	std::vector<std::shared_ptr<Scene>> models;
	for (const std::string& file : files) {
		auto node = scene.Make("chunk", glm::vec3(0.0f), level);
		auto model = std::make_shared<Scene>();
		paths[model.get()] = file;
		node->SetModel(model);
		models.push_back(model);
	}
	NOX_REQUIRE(Registry::Get().Find("StaticCollision")->add(*level));
	t0 = std::chrono::steady_clock::now();
	physics.Update(kDt);
	out.createMs = MillisSince(t0);

	// Start the character on the first mesh's floor (a ray down from above its bounds centre).
	glm::vec3 mn(0.0f), mx(0.0f);
	auto first = MeshCollider::LoadShared(files.front());
	if (!first || !MeshCollider::LocalBounds(*first, mn, mx)) return out;
	const glm::vec3 centre = 0.5f * (mn + mx);
	PhysicsRayHit hit;
	auto hero = scene.Make("hero", glm::vec3(0.0f));
	NOX_REQUIRE(physics.Characters().Add(hero));
	if (physics.RayCastClosest(glm::vec3(centre.x, mx.y + 50.0f, centre.z), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f, 0xFFFFFFFFu, hit)) {
		hero->SetPosition(hit.point + glm::vec3(0.0f, 0.05f, 0.0f));
	}
	for (int frame = 0; frame < 30; ++frame) physics.Update(kDt);   // settle

	const float startX = hero->GetWorldPosition().x;
	double total = 0.0;
	int onFloorFrames = 0;
	for (int frame = 0; frame < 600; ++frame) {
		physics.Characters().Move(physics, *hero, glm::vec3(0.06f, 0.0f, 0.0f));
		physics.Update(kDt);
		const double ms = physics.GetProfilingData().stepTimeMs;
		total += ms;
		out.maxStepMs = std::max(out.maxStepMs, static_cast<double>(ms));
		out.frames++;
		if (const CharacterSettings* s = physics.Characters().Find(hero.get()); s && s->onFloor) onFloorFrames++;
	}
	out.avgStepMs = total / std::max(1, out.frames);
	out.walked = hero->GetWorldPosition().x - startX;
	out.onFloorFraction = static_cast<double>(onFloorFrames) / std::max(1, out.frames);
	MeshCollider::ClearCache();
	return out;
}

void PrintBench(const LevelBench& b) {
	std::fprintf(stderr, "[Bench] %s: %zu meshes, %zu triangles, load %.2f ms, bodies %.2f ms, "
		"step avg %.4f ms max %.4f ms over %d frames; character walked %.2f m, on floor %.0f%% of frames\n",
		b.name.c_str(), b.meshes, b.triangles, b.loadMs, b.createMs, b.avgStepMs, b.maxStepMs, b.frames,
		static_cast<double>(b.walked), 100.0 * b.onFloorFraction);
}

} // namespace

NOX_TEST(PhysicsBench, LevelStepTimes)
{
	const char* gate = std::getenv("NOX_PHYSICS_BENCH");
	if (!gate || std::string(gate) != "1") return;
	PrintBench(BenchLevel("character demo level", { "projects/character-demo/models/level.glb" }));

	std::vector<std::string> shuto;
	for (const auto& entry : std::filesystem::directory_iterator("models/shuto/rep_section/collision")) {
		if (entry.path().extension() == ".glb") shuto.push_back(entry.path().generic_string());
	}
	std::sort(shuto.begin(), shuto.end());
	NOX_REQUIRE(!shuto.empty());
	PrintBench(BenchLevel("shuto rep section collision (" + std::to_string(shuto.size()) + " chunks)", shuto));
}
