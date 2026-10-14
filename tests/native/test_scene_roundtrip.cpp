// Save/load round trips and prefab references, headless: no GL device (no skybox, no GUI nodes), no audio
// device (audio nodes without a sound file and with autoplay off). Each test runs in a private loose AssetIO
// sandbox under the system temp directory, so scene and prefab files never touch the source tree.
//
// Round trip contract: load(original) -> save A; load(A) -> save B; A == B byte for byte. The engine-mode
// check (NOX_SAVE_ROUNDTRIP on real scenes) is documented in docs/building.md.
#include "TestFramework.h"

#include "PhysicsEngine.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "LightNode.h"
#include "SceneNode.h"
#include "assets/AssetIO.h"
#include "scene/Prefab.h"
#include "reflect/Reflection.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Loose AssetIO rooted at a fresh temp directory. The previous AssetIO configuration (if any) is restored.
struct LooseSandbox {
	fs::path root;
	bool hadConfig = false;
	AssetIO::Config previous;
	bool ok = false;

	LooseSandbox() {
		if (AssetIO::IsInitialized()) {
			hadConfig = true;
			previous = AssetIO::GetConfig();
			AssetIO::Shutdown();
		}
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path() / ("nox_roundtrip_" + std::to_string(stamp));
		fs::create_directories(root / "scenes");
		fs::create_directories(root / "prefabs");
		AssetIO::Config config;
		config.mode = AssetIO::Mode::Loose;
		config.looseRoot = root;
		config.cacheDir = root / "cache";
		config.userDir = root / "user";
		config.projectName = "roundtrip-test";
		std::string error;
		ok = AssetIO::Initialize(config, &error);
		nox::reflect::RegisterBuiltinComponents();
	}

	~LooseSandbox() {
		AssetIO::Shutdown();
		if (hadConfig) AssetIO::Initialize(previous);
		std::error_code ec;
		fs::remove_all(root, ec);
	}

	void WriteText(const std::string& logical, const std::string& text) const {
		fs::create_directories((root / logical).parent_path());
		std::ofstream out(root / logical, std::ios::binary | std::ios::trunc);
		out << text;
	}

	json ReadJson(const std::string& logical) const {
		std::ifstream in(root / logical, std::ios::binary);
		std::stringstream buffer;
		buffer << in.rdbuf();
		return json::parse(buffer.str(), nullptr, false);
	}

	std::string ReadBytes(const fs::path& physical) const {
		std::ifstream in(physical, std::ios::binary);
		std::stringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	fs::path Physical(const std::string& name) const { return root / name; }
};

// Loads a scene (logical path) with a physics engine so colliders can be created.
std::shared_ptr<SceneGraph> LoadScene(SceneLoader& loader, const std::string& logical) {
	return loader.LoadScene(logical);
}

// Saves a loaded graph to a physical file, then returns the file's bytes.
std::string SaveCopy(SceneLoader& loader, const std::shared_ptr<SceneGraph>& graph, const std::string& sourceLogical,
	const fs::path& out) {
	NOX_REQUIRE(loader.SaveSceneCopy(graph, sourceLogical, out.string()));
	std::ifstream in(out, std::ios::binary);
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

std::shared_ptr<SceneNode> FindChild(const std::shared_ptr<SceneNode>& parent, const std::string& name) {
	for (const auto& child : parent->children) {
		if (child && child->GetName() == name) return child;
	}
	return nullptr;
}

const char* kCrateJson = R"({
  "type": "node", "name": "Crate",
  "children": [
    { "type": "node", "name": "Lid", "position": [0, 1, 0] },
    { "type": "node", "name": "Lid", "position": [0, 2, 0] },
    { "type": "light", "name": "Glow", "light": { "type": "point" }, "position": [0, 3, 0] }
  ]
})";

// One scene with every node kind the headless loader supports and one prefab reference.
const char* kSceneJson = R"({
  "scene_name": "RoundTrip",
  "exposure": 1.5,
  "gamma": 2.2,
  "physics_enabled": true,
  "render_settings": { "bloom": 0.3 },
  "nodes": [
    { "type": "node", "name": "Empty", "position": [1, 2, 3], "rotation": [0, 45, 0], "scale": [1, 1, 2],
      "children": [ { "type": "node", "name": "EmptyChild", "position": [0, 1, 0] } ] },
    { "type": "light", "name": "Sun", "light": { "type": "directional" }, "position": [0, 10, 0] },
    { "type": "light", "name": "Lamp", "light": { "type": "point" }, "position": [2, 2, 2] },
    { "type": "audio", "name": "Ambience", "sound": "", "volume": 0.5, "autoplay": false, "loop": true },
    { "type": "lpv_volume", "name": "GI",
      "lpv_data": { "center": [0, 0, 0], "extent": [10, 5, 10], "voxel_size": 1.0, "grid_resolution": 8 } },
    { "type": "node", "name": "Ball", "position": [0, 5, 0],
      "collider": { "type": "sphere", "mass": 2.0, "radius": 0.5, "gravity": true } },
    { "prefab": "prefabs/crate.prefab.json", "name": "Crate 1", "position": [5, 0, 0],
      "overrides": {
        "Lid": { "position": [0, 9, 0] },
        "Lid#1": { "remove": true },
        "Glow": { "components": { "Light": { "intensity": 3.0 } } }
      },
      "children": [ { "type": "node", "name": "Handle", "position": [1, 0, 0] } ] }
  ]
})";

// Node and top-level keys the loader does not model (or models without writing them back), on every node type.
const char* kUnknownKeysJson = R"({
  "scene_name": "Unknown", "custom_tool": { "rev": 3 },
  "nodes": [
    { "type": "node", "name": "Tagged", "tags": ["a", "b"], "lpv_config": { "grid_resolution": 128 } },
    { "type": "light", "name": "Tag", "light": { "type": "point", "intensity": 2 }, "editor_group": "lights" },
    { "type": "audio", "name": "Ambience", "sound": "", "volume": 0.5, "autoplay": false, "marker": 7 },
    { "type": "lpv_volume", "name": "GI", "lpv_data": { "grid_resolution": 8 }, "note": "keep" },
    { "type": "node", "name": "Deep", "position": [1, 0, 0], "nested": { "deep": [1, 2, { "x": null }] } }
  ]
})";

// Photometric authoring: lumens (point, spot), lux (directional), and candela with explicit attenuation.
const char* kLightUnitsJson = R"({
  "scene_name": "Units",
  "nodes": [
    { "type": "light", "name": "Bulb", "light": { "type": "point", "color": [1, 0.8, 0.6], "luminous_power": 1200 },
      "position": [0, 3, 0] },
    { "type": "light", "name": "Spot", "light": { "type": "spot", "luminous_power": 800, "cutOff": 20, "outerCutOff": 30 },
      "position": [0, 4, 0] },
    { "type": "light", "name": "Sun", "light": { "type": "directional", "illuminance": 50000 } },
    { "type": "light", "name": "Cd", "light": { "type": "point", "intensity": 5, "attenuation": [1, 0, 0.5] } }
  ]
})";

bool PropertyApplies(const SceneNode& node, const char* component, const char* property) {
	const auto* desc = nox::reflect::Registry::Get().Find(component);
	if (!desc) return false;
	for (const auto& p : desc->properties) {
		if (p.name == property) return p.AppliesTo(node);
	}
	return false;
}

} // namespace

NOX_TEST(SceneRoundTrip, UnknownKeysSurviveOnEveryNodeType)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("scenes/unknown.json", kUnknownKeysJson);

	SceneLoader loaderA(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loaderA, "scenes/unknown.json");
	NOX_REQUIRE(graph != nullptr);
	const std::string a = SaveCopy(loaderA, graph, "scenes/unknown.json", sandbox.Physical("a.json"));
	sandbox.WriteText("scenes/a.json", a);
	SceneLoader loaderB(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graphB = LoadScene(loaderB, "scenes/a.json");
	NOX_REQUIRE(graphB != nullptr);
	const std::string b = SaveCopy(loaderB, graphB, "scenes/a.json", sandbox.Physical("b.json"));
	NOX_CHECK(a == b);

	const json written = json::parse(a);
	const json& nodes = written["nodes"];
	NOX_REQUIRE(nodes.size() == 5);
	NOX_CHECK_EQ(nodes[0]["lpv_config"]["grid_resolution"].get<int>(), 128);
	NOX_CHECK_EQ(nodes[0]["tags"].size(), size_t(2));
	NOX_CHECK_EQ(nodes[1]["editor_group"].get<std::string>(), std::string("lights"));
	NOX_CHECK_EQ(nodes[2]["marker"].get<int>(), 7);
	NOX_CHECK_EQ(nodes[3]["note"].get<std::string>(), std::string("keep"));
	NOX_CHECK(nodes[4]["nested"]["deep"][2]["x"].is_null());
	NOX_CHECK_EQ(written["custom_tool"]["rev"].get<int>(), 3);
}

NOX_TEST(SceneRoundTrip, LightPhotometricUnitsRoundTripAndEditInTheirOwnUnit)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("scenes/units.json", kLightUnitsJson);

	SceneLoader loader(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loader, "scenes/units.json");
	NOX_REQUIRE(graph != nullptr);
	auto bulb = FindChild(graph->GetRoot(), "Bulb");
	auto spot = FindChild(graph->GetRoot(), "Spot");
	NOX_REQUIRE(bulb != nullptr && spot != nullptr);

	// Loaded as candela: 1200 lm / 4 pi and 800 lm / pi, with the implicit inverse-square falloff.
	auto bulbLight = dynamic_cast<LightNode*>(bulb.get())->GetLight();
	NOX_CHECK(std::abs(bulbLight->GetIntensity() - 1200.0f / (4.0f * 3.14159265f)) < 1e-3f);
	NOX_CHECK(std::abs(dynamic_cast<LightNode*>(spot.get())->GetLight()->GetIntensity() - 800.0f / 3.14159265f) < 1e-3f);
	NOX_CHECK(PropertyApplies(*bulb, "Light", "luminous_power"));
	NOX_CHECK(!PropertyApplies(*bulb, "Light", "intensity"));
	NOX_CHECK(!PropertyApplies(*bulb, "Light", "illuminance"));
	auto sun = FindChild(graph->GetRoot(), "Sun");
	NOX_REQUIRE(sun != nullptr);
	NOX_CHECK(PropertyApplies(*sun, "Light", "illuminance"));
	NOX_CHECK(!PropertyApplies(*sun, "Light", "luminous_power"));
	auto cd = FindChild(graph->GetRoot(), "Cd");
	NOX_REQUIRE(cd != nullptr);
	NOX_CHECK(PropertyApplies(*cd, "Light", "intensity"));

	const std::string a = SaveCopy(loader, graph, "scenes/units.json", sandbox.Physical("a.json"));
	const json written = json::parse(a);
	const json& nodes = written["nodes"];
	NOX_REQUIRE(nodes.size() == 4);
	NOX_CHECK(!nodes[0]["light"].contains("intensity"));
	NOX_CHECK(std::abs(nodes[0]["light"]["luminous_power"].get<double>() - 1200.0) < 1e-2);
	NOX_CHECK(!nodes[0]["light"].contains("attenuation"));
	NOX_CHECK(std::abs(nodes[1]["light"]["luminous_power"].get<double>() - 800.0) < 1e-2);
	NOX_CHECK(!nodes[1]["light"].contains("intensity"));
	NOX_CHECK(std::abs(nodes[2]["light"]["illuminance"].get<double>() - 50000.0) < 1e-2);
	NOX_CHECK(!nodes[2]["light"].contains("intensity"));
	NOX_CHECK(nodes[3]["light"].contains("intensity"));
	NOX_CHECK(nodes[3]["light"].contains("attenuation"));

	// Editing in the inspector: luminous_power is the unit shown and written; the file stays in lumens.
	std::string error;
	NOX_CHECK(nox::reflect::SetProperty(*bulb, "Light", "luminous_power", 600.0, &error));
	const json edited = json::parse(SaveCopy(loader, graph, "scenes/units.json", sandbox.Physical("edited.json")));
	NOX_CHECK(std::abs(edited["nodes"][0]["light"]["luminous_power"].get<double>() - 600.0) < 1e-2);
	NOX_CHECK(!edited["nodes"][0]["light"].contains("intensity"));

	// Lumens and lux survive a second load and save unchanged (A == B).
	sandbox.WriteText("scenes/a.json", a);
	SceneLoader loaderB(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graphB = LoadScene(loaderB, "scenes/a.json");
	NOX_REQUIRE(graphB != nullptr);
	NOX_CHECK(a == SaveCopy(loaderB, graphB, "scenes/a.json", sandbox.Physical("b.json")));
}

NOX_TEST(SceneRoundTrip, LoadSaveLoadSaveIsByteIdentical)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("prefabs/crate.prefab.json", kCrateJson);
	sandbox.WriteText("scenes/rt.json", kSceneJson);

	SceneLoader loaderA(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loaderA, "scenes/rt.json");
	NOX_REQUIRE(graph != nullptr);

	const std::string a = SaveCopy(loaderA, graph, "scenes/rt.json", sandbox.Physical("a.json"));
	sandbox.WriteText("scenes/a.json", a);

	SceneLoader loaderB(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graphB = LoadScene(loaderB, "scenes/a.json");
	NOX_REQUIRE(graphB != nullptr);
	const std::string b = SaveCopy(loaderB, graphB, "scenes/a.json", sandbox.Physical("b.json"));

	NOX_CHECK(!a.empty());
	NOX_CHECK(a == b);

	// The written file keeps the authored top-level keys the runtime does not model.
	const json written = json::parse(a);
	NOX_CHECK(written.contains("render_settings"));
	NOX_CHECK_EQ(written["scene_name"].get<std::string>(), std::string("RoundTrip"));
	NOX_CHECK(written["nodes"].size() == 7);
}

NOX_TEST(SceneRoundTrip, ComponentsAndLegacyKeysSurviveTheRoundTrip)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("prefabs/crate.prefab.json", kCrateJson);
	sandbox.WriteText("scenes/rt.json", kSceneJson);

	SceneLoader loader(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loader, "scenes/rt.json");
	NOX_REQUIRE(graph != nullptr);
	const json written = json::parse(SaveCopy(loader, graph, "scenes/rt.json", sandbox.Physical("out.json")));
	const json& nodes = written["nodes"];

	// Light: legacy "light" key and the registry "components" block agree on the type.
	NOX_CHECK_EQ(nodes[2]["light"]["type"].get<std::string>(), std::string("point"));
	NOX_CHECK(nodes[2].contains("components"));
	NOX_CHECK(nodes[2]["components"].contains("Light"));

	// Audio without a device keeps its authored values.
	NOX_CHECK_EQ(nodes[3]["volume"].get<double>(), 0.5);
	NOX_CHECK(nodes[3]["loop"].get<bool>());
	NOX_CHECK(!nodes[3]["autoplay"].get<bool>());

	// LPV volume data.
	NOX_CHECK_EQ(nodes[4]["lpv_data"]["grid_resolution"].get<int>(), 8);

	// Rigid body collider.
	NOX_CHECK_EQ(nodes[5]["collider"]["type"].get<std::string>(), std::string("sphere"));
	NOX_CHECK_EQ(nodes[5]["collider"]["mass"].get<double>(), 2.0);
}

NOX_TEST(Prefab, InstanceKeepsItsLinkAndSavesOnlyDifferences)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("prefabs/crate.prefab.json", kCrateJson);
	sandbox.WriteText("scenes/rt.json", kSceneJson);

	SceneLoader loader(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loader, "scenes/rt.json");
	NOX_REQUIRE(graph != nullptr);
	auto crate = FindChild(graph->GetRoot(), "Crate 1");
	NOX_REQUIRE(crate != nullptr);
	NOX_CHECK_EQ(crate->GetPrefabPath(), std::string("prefabs/crate.prefab.json"));

	// Overrides applied: first Lid moved, second Lid removed, Glow intensity changed, Handle added.
	NOX_REQUIRE(crate->children.size() == 3);   // Lid, Glow, Handle
	NOX_CHECK(FindChild(crate, "Lid") != nullptr);
	NOX_CHECK(FindChild(crate, "Glow") != nullptr);
	NOX_CHECK(FindChild(crate, "Handle") != nullptr);
	NOX_CHECK(FindChild(crate, "Lid")->GetPosition() == glm::vec3(0.0f, 9.0f, 0.0f));
	// The unchanged prefab children are not prefab roots.
	NOX_CHECK(FindChild(crate, "Lid")->GetPrefabPath().empty());

	const json written = json::parse(SaveCopy(loader, graph, "scenes/rt.json", sandbox.Physical("out.json")));
	const json& ref = written["nodes"][6];
	NOX_CHECK_EQ(ref["prefab"].get<std::string>(), std::string("prefabs/crate.prefab.json"));
	NOX_CHECK_EQ(ref["name"].get<std::string>(), std::string("Crate 1"));
	NOX_CHECK(ref.contains("position"));
	NOX_CHECK(!ref.contains("children") || ref["children"].size() == 1);   // Handle is an addition
	NOX_REQUIRE(ref.contains("overrides"));
	NOX_CHECK(ref["overrides"].contains("Lid"));
	NOX_CHECK(ref["overrides"].contains("Lid#1") && ref["overrides"]["Lid#1"].value("remove", false));
	NOX_CHECK(ref["overrides"].contains("Glow"));
	// Nothing that matches the prefab is written: the second Lid's position is only in the removal.
	NOX_CHECK(!ref["overrides"].contains("Lid#1/position"));
}

NOX_TEST(Prefab, SavePrefabWritesTheSubtreeAndInstantiatePlacesIt)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("scenes/empty.json", R"({"nodes":[]})");

	SceneLoader loader(nullptr, std::make_shared<PhysicsEngine>(), 1280, 720);
	auto graph = LoadScene(loader, "scenes/empty.json");
	NOX_REQUIRE(graph != nullptr);

	auto source = loader.InstantiateNode(*graph, json::parse(kCrateJson), graph->GetRoot(), "scenes/empty.json");
	NOX_REQUIRE(source != nullptr);
	std::string error;
	NOX_REQUIRE(nox::prefab::SavePrefab(loader, source, "prefabs/saved.prefab.json", &error));
	const json saved = sandbox.ReadJson("prefabs/saved.prefab.json");
	NOX_CHECK_EQ(saved["name"].get<std::string>(), std::string("Crate"));
	NOX_CHECK(saved["children"].size() == 3);
	NOX_CHECK(!saved.contains("prefab"));

	// Instantiate places a new instance; saving it again writes only the placement.
	auto instance = nox::prefab::InstantiatePrefab(loader, *graph, "prefabs/saved.prefab.json", nullptr,
		json{ { "position", { 1.0, 2.0, 3.0 } } }, &error);
	NOX_REQUIRE(instance != nullptr);
	NOX_CHECK_EQ(instance->GetPrefabPath(), std::string("prefabs/saved.prefab.json"));
	NOX_CHECK(instance->GetPosition() == glm::vec3(1.0f, 2.0f, 3.0f));
	NOX_CHECK_EQ(instance->children.size(), size_t(3));

	const json written = json::parse(SaveCopy(loader, graph, "scenes/empty.json", sandbox.Physical("placed.json")));
	const json& ref = written["nodes"][1];   // nodes[0] is the plain source subtree
	NOX_CHECK_EQ(ref["prefab"].get<std::string>(), std::string("prefabs/saved.prefab.json"));
	NOX_CHECK(ref.contains("position"));
	NOX_CHECK(!ref.contains("children"));
	NOX_CHECK(!ref.contains("overrides"));
}

NOX_TEST(Prefab, NestedReferencesExpandAndCyclesAreRejected)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("prefabs/inner.prefab.json", R"({"type":"node","name":"Inner"})");
	sandbox.WriteText("prefabs/outer.prefab.json",
		R"({"type":"node","name":"Outer","children":[{"prefab":"prefabs/inner.prefab.json","name":"Inner 2"}]})");
	sandbox.WriteText("prefabs/self.prefab.json", R"({"type":"node","name":"Self","children":[{"prefab":"prefabs/self.prefab.json"}]})");

	json ref = { { "prefab", "prefabs/outer.prefab.json" } };
	std::string error;
	NOX_REQUIRE(nox::prefab::ExpandReferences(ref, &error));
	NOX_CHECK_EQ(ref["name"].get<std::string>(), std::string("Outer"));
	NOX_REQUIRE(ref["children"].size() == 1);
	NOX_CHECK_EQ(ref["children"][0]["name"].get<std::string>(), std::string("Inner 2"));
	NOX_CHECK_EQ(ref["children"][0]["prefab"].get<std::string>(), std::string("prefabs/inner.prefab.json"));

	json cycle = { { "prefab", "prefabs/self.prefab.json" } };
	error.clear();
	NOX_CHECK(!nox::prefab::ExpandReferences(cycle, &error));
	NOX_CHECK(error.find("references itself") != std::string::npos);

	json missing = { { "prefab", "prefabs/none.prefab.json" } };
	error.clear();
	NOX_CHECK(!nox::prefab::ExpandReferences(missing, &error));
}

NOX_TEST(Prefab, ChildKeysAddressDuplicateNamesInOrder)
{
	const json children = json::parse(R"([{"name":"A"},{"name":"B/x"},{"name":"A"},{"type":"light"}])");
	const std::vector<std::string> keys = nox::prefab::ChildKeys(children);
	NOX_REQUIRE(keys.size() == 4);
	NOX_CHECK_EQ(keys[0], std::string("A"));
	NOX_CHECK_EQ(keys[1], std::string("B_x"));
	NOX_CHECK_EQ(keys[2], std::string("A#1"));
	NOX_CHECK_EQ(keys[3], std::string("light"));

	json root = json::parse(R"({"children":[{"name":"A","children":[{"name":"deep"}]},{"name":"A"}]})");
	NOX_REQUIRE(nox::prefab::FindPath(root, "A/deep") != nullptr);
	NOX_CHECK(nox::prefab::FindPath(root, "A#1") != nullptr);
	NOX_CHECK(nox::prefab::FindPath(root, "A/missing") == nullptr);
}

NOX_TEST(Prefab, OverridesApplyToTheExpandedTree)
{
	LooseSandbox sandbox;
	NOX_REQUIRE(sandbox.ok);
	sandbox.WriteText("prefabs/crate.prefab.json", kCrateJson);

	json ref = json::parse(R"({"prefab":"prefabs/crate.prefab.json",
		"components":{"Node":{}},
		"children":[{"name":"Extra"}],
		"overrides":{"Lid":{"position":[7,7,7]},"Glow":{"components":{"Light":{"intensity":4}}}}})");
	std::string error;
	NOX_REQUIRE(nox::prefab::ExpandReferences(ref, &error));
	NOX_CHECK_EQ(ref["children"].size(), size_t(4));   // Lid, Lid, Glow, Extra
	NOX_CHECK_EQ(ref["children"][0]["position"][1].get<int>(), 7);
	NOX_CHECK_EQ(ref["children"][2]["components"]["Light"]["intensity"].get<int>(), 4);
	NOX_CHECK_EQ(ref["children"][2]["light"]["type"].get<std::string>(), std::string("point"));
}
