// Game layer: registers the racing game (GameSession) and its content producers with the engine.
// main() calls RegisterNoxRacingGame() before the window opens; nothing in the engine refers to
// the names or file layouts used here.
#include "NoxRacingGame.h"
#include "GameModule.h"
#include "GameEditorPanels.h"
#include "GameSession.h"
#include "NightStreetPacks.h"
#include "ProceduralStreetKit.h"
#include "RoadNetwork.h"
#include "SceneExtensions.h"
#include <filesystem>
#include <iostream>

namespace {
	ProceduralGeneratorResult ToGeneratorResult(ProceduralStreetKit::Result&& generated)
	{
		ProceduralGeneratorResult out;
		out.ok = generated.ok;
		if (!generated.ok) return out;
		out.gltfPath = std::move(generated.gltfPath);
		// Generated children: light nodes first, then CC0 prop model instances (district).
		out.childNodes = std::move(generated.lightNodes);
		for (const nlohmann::json& prop : generated.propNodes) out.childNodes.push_back(prop);
		out.reflectionProbes = std::move(generated.reflectionProbes);
		out.renderExtras = std::move(generated.renderExtras);
		out.hasRenderExtras = true;
		return out;
	}
}

void RegisterNoxRacingGame()
{
	GameModules::SetFactory([] { return std::make_unique<GameSession>(); });
	RegisterRacingEditorPanels();

	SceneExtensions::RegisterGenerator("street_kit", [](const ProceduralGeneratorInput& in) {
		return ToGeneratorResult(ProceduralStreetKit::Generate(in.params));
	}, true);

	SceneExtensions::RegisterGenerator("district", [](const ProceduralGeneratorInput& in) {
		// Road graph: node params override the scene's game.road_network; default grid otherwise.
		RoadNetwork network;
		bool loaded = false;
		if (in.params.contains("road_network")) loaded = network.Load(in.params["road_network"]);
		else if (in.sceneGame.contains("road_network")) loaded = network.Load(in.sceneGame["road_network"]);
		if (!loaded) ProceduralStreetKit::BuildDefaultDistrictNetwork(network);
		return ToGeneratorResult(ProceduralStreetKit::GenerateDistrict(in.params, network));
	});

	// "window_room_root": "<dir>" - an authored room atlas (atlas_manifest.json in <dir>) for
	// imported buildings whose glass uses interior mapping (textures/window_rooms/README.md).
	SceneExtensions::RegisterSceneKey("window_room_root", [](SceneKeyInput& in) {
		if (!in.value.is_string()) return false;
		const std::string root = in.value.get<std::string>();
		const std::string manifest = in.resolvePath((std::filesystem::path(root) / "atlas_manifest.json").string());
		if (manifest.empty() || !NightStreetPacks::BuildInteriorAtlas(
			std::filesystem::path(manifest).parent_path().string(), in.renderExtras.interiorAtlas)) {
			std::cerr << "[Game] Failed to load window room atlas: " << root << std::endl;
			return false;
		}
		std::cout << "[Game] Window room atlas: " << in.renderExtras.interiorAtlas.RoomCount() << " rooms, "
			<< in.renderExtras.interiorAtlas.faceLayers.size() << " faces" << std::endl;
		return true;
	});
}
