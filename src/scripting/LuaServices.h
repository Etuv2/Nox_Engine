#pragma once
// Engine services exposed to project Lua (docs/lua_api.md "Camera", "Effects", "Navigation", "Save data").
// Owned by LuaWorld; the module wires the camera. Everything here is generic: road networks and collision
// meshes come from files the project declares, particles are the native VehicleFX pools, and save data is
// a validated JSON file in the user directory.
#include "../audio/ProceduralVehicleAudio.h"
#include "CollisionWorld.h"
#include "RoadNetwork.h"
#include "json.hpp"

#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;
class Camera;

namespace NoxLua {

	namespace services_limits {
		constexpr size_t kMaxRoads = 8;               // live road networks per world (declared + inline)
		constexpr size_t kMaxRoadSlots = 256;         // slots created per world, released ones included
		constexpr size_t kMaxCollision = 8;
		constexpr int kMaxFxEmitsPerFrame = 512;      // emitter calls per frame (native pools do the rest)
		constexpr size_t kMaxRoutePoints = 4096;
		constexpr size_t kMaxEdgeSegments = 32768;    // edge_polylines: edges x segments per call
		constexpr int kMaxJsonNodes = 20000;          // save data: values per table tree
		constexpr int kMaxJsonDepth = 16;
		constexpr size_t kMaxStringBytes = 65536;
		constexpr size_t kMaxUserFileBytes = 1024u * 1024u;
		constexpr size_t kMaxUserNameLength = 96;
		constexpr size_t kMaxAudioCuesPerFrame = 16;
	}

	// A file the project declares in project.json ("roads" / "collision"). `name` is what scripts pass.
	struct DeclaredFile {
		std::string name;   // as written in project.json
		std::string path;   // resolved logical path
	};

	struct LuaServices {
		Camera* camera = nullptr;          // module camera (owned by the module), may be null
		float aspect = 16.0f / 9.0f;       // from the module's Update
		glm::vec2 overlayOrigin{ 0.0f };   // game overlay rectangle in window coordinates (ctx:overlay_rect)
		glm::vec2 overlaySize{ 0.0f };
		bool cameraCut = false;            // ctx:camera_cut(), consumed by ConsumeTeleport
		int fxEmitsThisFrame = 0;
		bool fxUsed = false;               // any emitter call since the last Clear: the module ticks VehicleFX
		std::vector<DeclaredFile> declaredRoads;
		std::vector<DeclaredFile> declaredCollision;
		std::vector<std::string> sceneCollisionFiles;   // normalized game.collision.files of the scene (nox.collision_file)
		std::vector<std::unique_ptr<RoadNetwork>> roads;
		std::map<std::string, size_t> roadIndex;
		std::map<std::string, size_t> inlineRoadIndex;   // canonical JSON of a from_table network -> slot (shared)
		std::vector<std::unique_ptr<CollisionWorld>> collision;
		std::map<std::string, size_t> collisionIndex;
		VehicleAudioState audio;           // published by the module once per frame (ctx:audio_set)
		bool audioUsed = false;            // a script touched audio since the last reset
		std::vector<AudioCue> audioCues;   // queued by ctx:audio_cue, played after the update
	};

	// Adds the camera/effects/save functions to the ctx table at ctxIdx and the nox.roads / nox.collision loaders to
	// the nox table at noxIdx. The services pointer is kept in the registry and must outlive the VM.
	void InstallServicesApi(lua_State* L, int ctxIdx, int noxIdx, LuaServices* services);
	// Called by LuaWorld::Load before the entry runs: the project's declared files and a clean per-world state.
	void ResetServices(LuaServices& services, const std::vector<DeclaredFile>& roads, const std::vector<DeclaredFile>& collision);

	// Save data names: 1-4 segments of [A-Za-z0-9_-] separated by '/', last segment ends with ".json".
	bool ValidateUserFileName(const std::string& name, std::string& error);
	// Lua value -> JSON with the limits above. Tables are arrays (keys 1..n) or objects (string keys); no functions,
	// userdata, threads, mixed tables or non-finite numbers.
	bool LuaValueToJson(lua_State* L, int idx, nlohmann::json& out, std::string& error);
	// JSON -> Lua value pushed on the stack. Arrays are marked (see MarkJsonArray).
	void PushJsonValue(lua_State* L, const nlohmann::json& value);
	// Marks the table at idx as a JSON array: it saves as an array even when empty (nox.array(t) does this for scripts).
	void MarkJsonArray(lua_State* L, int idx);
}
