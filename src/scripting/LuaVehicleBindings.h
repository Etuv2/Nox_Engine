#pragma once
// Generic vehicle API for Lua (ctx.vehicle; docs/lua_api.md "Vehicles"). Engine code: no game names, asset
// paths or default presets. A project supplies the definition data, the rig node names and the controller.
//
// VehicleHost owns one VehicleService (independent instances, checked handles) and one LuaVehiclePolicy per
// vehicle. Every controller runs in the Runtime given to the host (one VM per project world). The host is
// stepped explicitly (ctx.vehicle.step) by the project; it never runs on its own clock.
#include "json.hpp"
#include "vehicle/VehicleService.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;
class SceneNode;
class RoadNetwork;
class CollisionWorld;

namespace NoxLua {
	class Runtime;
	class ScriptSource;
	class LuaVehiclePolicy;

	class VehicleHost {
	public:
		using NodeResolver = std::function<std::shared_ptr<SceneNode>(const std::string& name)>;

		// `runtime` and `files` must outlive the host. `files` reads definition data (logical paths).
		VehicleHost(Runtime& runtime, ScriptSource* files);
		~VehicleHost();
		VehicleHost(const VehicleHost&) = delete;
		VehicleHost& operator=(const VehicleHost&) = delete;

		// Creates a vehicle from a definition object (docs/lua_api.md "Vehicles" for the schema). Returns an
		// invalid handle with the reason in *error for any unsupported or invalid definition.
		VehicleHandle Create(const nlohmann::json& spec, const NodeResolver& resolve, std::string* error);
		// Scene lookup for rig node names (the project's world supplies it; absent = planar vehicles only).
		void SetNodeResolver(NodeResolver resolve) { m_resolve = std::move(resolve); }
		const NodeResolver& Resolver() const { return m_resolve; }
		ScriptSource* Files() const { return m_files; }
		void Destroy(VehicleHandle h);
		bool IsValid(VehicleHandle h) const { return m_service.IsValid(h); }
		size_t Count() const { return m_service.Count(); }

		// Latched driver input (held until changed; applied at each step).
		bool SetInput(VehicleHandle h, const VehicleInput& in);
		// Steps every vehicle by dt (the service clamps dt and runs its 240 Hz sub-steps).
		void Step(float dt, bool frozen);

		bool Teleport(VehicleHandle h, const glm::vec2& pos, float headingRad, std::string* error);
		bool Reset(VehicleHandle h);
		// Native RecoverToRoad (lane centre of the nearest road when the definition carries a road network,
		// else the last safe pose / spawn). Teleports and notifies the controller like any teleport.
		bool RecoverToRoad(VehicleHandle h);
		// Reload transfer (called at the swap, after the candidate validated): takes over the running vehicles of `old`
		// with their handles, physics state, latched input and tuning. Each controller is rebuilt in this host's runtime
		// (the candidate VM), its declared state is restored (state/restore hooks) and its debug table carried over.
		// Candidate-created vehicles are replaced. `old` is left empty; its controllers are released here.
		void AdoptFrom(VehicleHost& old);
		// Live handles in id order (a reloaded world re-acquires its vehicles through this).
		std::vector<VehicleHandle> Handles() const;
		// ctx.vehicle.refuel: the controller owns its boost state; calls the optional on_refuel hook.
		bool Refuel(VehicleHandle h);
		// Replaces the controller (the old one is released after the swap). `params` null keeps the vehicle's current
		// controller parameters. A controller that cannot be loaded is installed faulted and reported (the vehicle
		// coasts); a vehicle never falls back to a native policy.
		bool SetController(VehicleHandle h, const std::string& module, const nlohmann::json* params, std::string* error);
		// Numeric tuning only. Geometry, backend, drive and rig are fixed for a live vehicle: those keys are
		// rejected with a "restart Play" error.
		bool SetProfile(VehicleHandle h, const nlohmann::json& profile, std::string* error);

		nlohmann::json Telemetry(VehicleHandle h) const;
		// Four wheel tables (FL FR RL RR): contact, velocity, slip, wheelspin, load, grounded, suspension, spin_angle,
		// steer_angle. Copied out; empty array for a stale handle.
		nlohmann::json Wheels(VehicleHandle h) const;
		// Faults recorded by the vehicle's controller (empty when none).
		std::vector<std::string> Faults(VehicleHandle h) const;
		const LuaVehiclePolicy* Controller(VehicleHandle h) const;

		VehicleService& Service() { return m_service; }

	private:
		struct Entry {
			VehicleHandle handle;
			nlohmann::json controllerParams;
			std::unique_ptr<LuaVehiclePolicy> policy;
			VehicleInput input;
			VehicleTuning tuning;
			std::shared_ptr<RoadNetwork> roads;   // world.road_network; the service holds a non-owning pointer
			std::shared_ptr<CollisionWorld> collision;   // world.collision (surface mode); non-owning in the service
		};
		Entry* Find(VehicleHandle h);
		const Entry* Find(VehicleHandle h) const;
		std::unique_ptr<LuaVehiclePolicy> MakePolicy(const std::string& module, const nlohmann::json& params);

		Runtime& m_runtime;
		ScriptSource* m_files;
		NodeResolver m_resolve;
		VehicleService m_service;
		std::map<uint32_t, Entry> m_entries;   // by handle id; the service checks the incarnation
	};

	// Installs ctx.vehicle into the context table at stack index `ctxIndex`. `host` is not owned and must outlive
	// the table. The functions raise Lua errors for stale handles and bad arguments; failures that the caller is
	// expected to handle return nil plus a message.
	void InstallVehicleApi(lua_State* L, int ctxIndex, VehicleHost* host);
}
