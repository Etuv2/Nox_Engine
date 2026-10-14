// Procedural street generators ("street_kit", "district"): the content producers of the racing streets.
// They are compiled into NoxProcedural, which only nox_cook (packaging-time cook) and the legacy racing
// executable (Nox_RacingLegacy.exe) link. Nox_Engine.exe never compiles them; it loads their cooked results.
#include "procedural/ProceduralStreetGenerators.h"
#include "ProceduralStreetKit.h"
#include "RoadNetwork.h"
#include "SceneExtensions.h"
#include <vector>

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
		// The district's collision layout (<output>_layout.json) is a side file of the model.
		if (!generated.layoutPath.empty()) out.auxiliaryPaths.push_back(generated.layoutPath);
		return out;
	}
}

// Registered by the legacy game and by tools/cook (nox_cook), which runs them headless; the engine only sees
// the generic registry (SceneExtensions). Bump a version when a generator's output for the same params
// changes, so cooked results are rebuilt.
void RegisterProceduralStreetGenerators()
{
	const std::vector<std::string> packInputs = { "texture_root", "decal_root", "lens_flare_root", "window_room_root" };

	SceneExtensions::RegisterGenerator("street_kit", [](const ProceduralGeneratorInput& in) {
		return ToGeneratorResult(ProceduralStreetKit::Generate(in.params));
	}, true, ProceduralGeneratorInfo{ 1, {}, packInputs, {}, "generated/nfs_street" });

	SceneExtensions::RegisterGenerator("district", [](const ProceduralGeneratorInput& in) {
		// Road graph: node params override the scene's game.road_network; default grid otherwise.
		RoadNetwork network;
		bool loaded = false;
		if (in.params.contains("road_network")) loaded = network.Load(in.params["road_network"]);
		else if (in.sceneGame.contains("road_network")) loaded = network.Load(in.sceneGame["road_network"]);
		if (!loaded) ProceduralStreetKit::BuildDefaultDistrictNetwork(network);
		return ToGeneratorResult(ProceduralStreetKit::GenerateDistrict(in.params, network));
	}, false, ProceduralGeneratorInfo{ 1, { "road_network" }, packInputs, {}, "generated/nfs_district",
		// CC0 props placed by GenerateDistrict (ProceduralStreetKit prop(...) calls); packed with the profile.
		{ "models/cc0/exterior_aircon_unit.glb", "models/cc0/fire_hydrant.glb", "models/cc0/metal_trash_can.glb",
		  "models/cc0/modular_street_seating.glb", "models/cc0/planter_box_01.glb", "models/cc0/potted_plant_01.glb",
		  "models/cc0/power_box_01.glb", "models/cc0/shrub_01.glb", "models/cc0/utility_box_01.glb" } });
}
