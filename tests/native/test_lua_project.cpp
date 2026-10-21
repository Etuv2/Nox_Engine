// The sample project through the real IGameModule path: AssetIO descriptor, scene "project" key,
// Configure, Play/Pause/Stop and frame updates. Uses the repository's loose tree (read-only).
#include "TestFramework.h"

#include "Camera.h"
#include "GameModule.h"
#include "SceneGraph.h"
#include "assets/AssetIO.h"
#include "ui/Pointer.h"
#include "scripting/LuaGameModule.h"
#include "scripting/LuaWorld.h"

#include "json.hpp"

#include <filesystem>

namespace {
	struct AssetIOSession {
		AssetIOSession() {
			AssetIO::Config config;
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = NoxTest::RepoRoot();
			config.userDir = NoxTest::ScratchDir() / "user";
			config.cacheDir = NoxTest::ScratchDir() / "cache";
			config.projectName = "lua-tests";
			std::string error;
			ok = AssetIO::Initialize(config, &error);
			if (!ok) std::printf("    AssetIO: %s\n", error.c_str());
		}
		~AssetIOSession() { AssetIO::Shutdown(); }
		bool ok = false;
	};
}

NOX_TEST(LuaProject, SampleArenaRunsThroughGameModule)
{
	AssetIOSession assets;
	NOX_REQUIRE(assets.ok);

	std::string error;
	const auto sceneText = AssetIO::ReadText("scenes/lua_sample.json", &error);
	NOX_REQUIRE(sceneText.has_value());
	const nlohmann::json scene = nlohmann::json::parse(*sceneText);
	const std::string projectPath = scene.value("project", std::string());
	NOX_CHECK_EQ(projectPath, std::string("projects/sample-arena/project.json"));

	const auto descriptorText = AssetIO::ReadText(projectPath, &error);
	NOX_REQUIRE(descriptorText.has_value());
	std::unique_ptr<IGameModule> module = NoxLua::CreateLuaProjectModule(projectPath, nlohmann::json::parse(*descriptorText), error);
	NOX_REQUIRE(module != nullptr);

	// Scene nodes the project refers to (the scene loader is not needed for the script contract).
	SceneGraph sceneGraph;
	ComponentManager* cm = sceneGraph.GetComponentManager();
	const EntityID mover = cm->CreateEntity("Mover");
	const EntityID player = cm->CreateEntity("Player");
	sceneGraph.GetTransformSystem()->SetPosition(mover, glm::vec3(0.0f));

	// The sample's camera and HUD need a module camera and the action map (fonts and icons need a GPU and are
	// not loaded in this headless test: its draw handlers are not run).
	module->SetCameraTemplate(Camera(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f, 60.0f, 16.0f / 9.0f, 0.1f, 1000.0f));
	module->Configure(scene.value("game", nlohmann::json::object()), scene, sceneGraph);
	NOX_REQUIRE(module->IsAvailable());
	auto* lua = dynamic_cast<NoxLua::LuaGameModule*>(module.get());
	NOX_REQUIRE(lua != nullptr);
	NOX_CHECK(lua->Diagnostics().empty());
	// mover, health (quests is an event handler), hud_input, orbit_camera, exhaust (HUD, camera and effects demo), menu
	NOX_CHECK_EQ(lua->World()->SystemNames().size(), size_t(6));

	// Nothing runs before Play, even though the host calls Update every frame.
	module->Update(1.0f / 60.0f, true, 1.0f);
	NOX_CHECK_EQ(lua->World()->Frame(), uint64_t(0));

	module->BeginPlay();
	const float dt = 1.0f / 60.0f;
	const glm::vec2 viewport(1280.0f, 720.0f);
	ui::Pointer& pointer = ui::Pointer::Get();
	pointer.SetInjectionOnly(true);   // the host's mouse cannot disturb the test
	// One frame as the host runs it: pointer, update, overlay (headless: the overlay records commands and hit-tests).
	const auto frame = [&]() {
		pointer.BeginFrame(dt);
		module->Update(dt, true, 1.0f);
		module->DrawOverlay(nullptr, glm::vec2(0.0f), viewport, dt);   // no draw list: records and hit-tests only
		pointer.EndFrame();
	};

	// The title screen holds the game: mover and health return early while the menu is open.
	const EntityHandle playerHandle = lua->World()->FindByName("Player");
	NOX_REQUIRE(playerHandle.id == player);
	NoxLua::Value health;
	for (int i = 0; i < 5; ++i) frame();
	NOX_CHECK_EQ(lua->World()->Frame(), uint64_t(5));
	NOX_CHECK(sceneGraph.GetTransformSystem()->GetWorldPosition(mover).x == 0.0f);
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Health", "current", health));
	NOX_CHECK_EQ(std::get<int64_t>(health), int64_t(3));

	// Start is the first button of the title screen: centred at (640, 307) in the 1280 x 720 overlay. Hover, press, release.
	pointer.InjectMove(glm::vec2(640.0f, 307.0f));
	frame();
	pointer.InjectButton(ui::PointerButton::Left, true);
	frame();
	pointer.InjectButton(ui::PointerButton::Left, false);
	frame();
	frame();
	NOX_CHECK(sceneGraph.GetTransformSystem()->GetWorldPosition(mover).x != 0.0f);

	// Health ticks once at frame 120: 3 -> 2.
	while (lua->World()->Frame() < 120) frame();
	NOX_CHECK_EQ(lua->World()->Frame(), uint64_t(120));
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Health", "current", health));
	NOX_CHECK_EQ(std::get<int64_t>(health), int64_t(2));

	// Pause freezes the frame counter and the health ticks.
	module->SetPaused(true);
	NOX_CHECK(module->IsPaused());
	for (int i = 0; i < 120; ++i) frame();
	NOX_CHECK_EQ(lua->World()->Frame(), uint64_t(120));
	module->SetPaused(false);

	// Run to defeat: ticks at frames 240 and 360 bring health to zero; the event then fails the quest.
	while (lua->World()->Frame() < 360) frame();
	NOX_CHECK(lua->World()->FaultedNames().empty());
	NOX_CHECK(lua->Diagnostics().empty());
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Health", "current", health));
	NOX_CHECK_EQ(std::get<int64_t>(health), int64_t(0));
	NoxLua::Value quest;
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Quest", "state", quest));
	NOX_CHECK_EQ(std::get<std::string>(quest), std::string("failed"));

	// Stop restores authored state and the mover's pre-play transform.
	module->EndPlay();
	NOX_CHECK(sceneGraph.GetTransformSystem()->GetWorldPosition(mover).x == 0.0f);
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Health", "current", health));
	NOX_CHECK_EQ(std::get<int64_t>(health), int64_t(3));
	NOX_REQUIRE(lua->World()->GetDeclaredField(playerHandle, "Inventory", "gold", health));
	NOX_CHECK_EQ(std::get<int64_t>(health), int64_t(12));
}

NOX_TEST(LuaProject, DescriptorValidationRejectsWrongRuntimeAndFormat)
{
	std::string error;
	NOX_CHECK(NoxLua::CreateLuaProjectModule("projects/x/project.json",
		nlohmann::json::parse(R"({"format":1,"runtime":"native","api_version":1,"entry":"scripts/main.lua"})"), error) == nullptr);
	NOX_CHECK(error.find("runtime") != std::string::npos);
	error.clear();
	NOX_CHECK(NoxLua::CreateLuaProjectModule("projects/x/project.json",
		nlohmann::json::parse(R"({"format":2,"runtime":"lua","api_version":1,"entry":"scripts/main.lua"})"), error) == nullptr);
	NOX_CHECK(error.find("format") != std::string::npos);
	error.clear();
	NOX_CHECK(NoxLua::CreateLuaProjectModule("projects/x/project.json",
		nlohmann::json::parse(R"({"format":1,"runtime":"lua","api_version":2,"entry":"scripts/main.lua"})"), error) == nullptr);
	NOX_CHECK(error.find("api_version") != std::string::npos);
	error.clear();
	NOX_CHECK(NoxLua::CreateLuaProjectModule("projects/x/project.json",
		nlohmann::json::parse(R"({"format":1,"runtime":"lua","api_version":1})"), error) == nullptr);
	NOX_CHECK(error.find("entry") != std::string::npos);
}
