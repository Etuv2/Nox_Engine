#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "json.hpp"
#include "LegacyNfsPolicy.h"
#include "scripting/LuaProject.h"
#include "scripting/LuaRuntime.h"
#include "scripting/LuaVehiclePolicy.h"
#include "vehicle/VehicleService.h"
#include "vehicle/VehiclePolicyTrace.h"

class Camera;
class SceneGraph;
class SceneNode;
class RoadNetwork;
class CollisionWorld;

/*
* Drive mode (docs/nfs2015/game.md): the racing prototype's car and chase camera.
*
* Thin facade (legacy racing library). The car itself is one VehicleService instance (src/vehicle,
* native backend: tyres, integration, collision, suspension, rig posing, mechanical gearbox) driven by
* a LegacyNfsPolicy (src/LegacyNfsPolicy.*: nitrous, drift, assists, gear selection). This class keeps
* the chase camera, the scene-JSON configuration (handling, camera, spawn, layout) and the
* NOX_VEHICLE_TELEMETRY recorder.
*
* Tuning lives in the scene's "game" object ("handling", "camera", "spawn").
*/
class DriveMode {
public:
	using Handling = LegacyNfsPolicy::Handling;
	struct CameraRig {
		float distance = 5.0f;          // m behind the car centre
		float height = 1.65f;           // m above the road
		float lookAhead = 10.0f;        // m ahead of the car centre
		float lookHeight = 0.7f;        // m (with height/distance this gives ~3.5 deg down)
		float fov = 55.0f;              // vertical degrees at rest
		float fovSpeedGain = 5.0f;      // restrained
		float fovNitro = 4.0f;
		float pullBack = 0.6f;          // extra distance at top speed
		float stiffness = 9.0f;
		float headingLag = 5.0f;
		float velocityFollow = 0.45f;   // 0 = follow heading, 1 = follow velocity (drift framing)
	};
	using Input = VehicleInput;
	using WheelState = VehicleWheelState;

	void Configure(const nlohmann::json& game, SceneGraph& sceneGraph, const RoadNetwork* roads,
		const std::string& layoutPath, const CollisionWorld* collision = nullptr);
	bool IsAvailable() const { return m_available; }
	bool IsActive() const { return m_active; }
	void SetActive(bool active);
	void CycleCamera() { m_cameraMode = (m_cameraMode + 1) % 4; }
	int CameraMode() const { return m_cameraMode; }
	void ResetToSpawn();
	void Teleport(const glm::vec2& pos, float heading);
	// Surface mode: places the car on the ground found below pos + 2.5 m (falls back to the highest
	// ground at pos). Legacy mode ignores the height.
	void Teleport(const glm::vec3& pos, float heading);
	void RecoverToRoad();
	void Refuel();                      // Lua controller: its on_respawn hook (the refuel rule)
	bool ConsumeTeleport();
	bool ConsumeTeleportForEffects();
	// True when the car runs the project's Lua controller (NOX_VEHICLE_POLICY=lua, transitional).
	bool UsesLuaPolicy() const { return m_luaPolicy != nullptr; }
	const NoxLua::LuaVehiclePolicy* LuaPolicy() const { return m_luaPolicy.get(); }

	// Simulates the car (frozen: no motion, e.g. countdown/menus) and poses the camera.
	void Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect);

	// State for HUD / race / audio / effects.
	glm::vec2 Position() const { return State().pos; }
	glm::vec3 Position3() const { return glm::vec3(State().pos.x, BaseY(), State().pos.y); }  // ground point under the car
	bool HasSurface() const { return m_collision != nullptr; }
	bool Airborne() const { return State().airborne; }
	glm::vec2 PreviousPosition() const { return State().prevPos; }
	float Heading() const { return State().heading; }
	float SpeedKmh() const { return glm::length(State().vel) * 3.6f; }
	float SpeedMs() const { return State().vx; }                 // signed forward speed
	float Speed01() const;
	float Rpm() const { return State().rpm; }
	int Gear() const { return State().gear; }                   // -1 = R, 1..GearCount()
	int GearCount() const { return m_policy.GearCount(); }
	float Nitro() const { return ActivePolicyDebug().Channel("nitro", 1.0f); }
	bool NitroActive() const { return ActivePolicyDebug().Channel("nitro_active") > 0.5f; }
	bool NitroRecharging() const { const VehiclePolicyDebug d = ActivePolicyDebug(); return !(d.Channel("nitro_active") > 0.5f) && d.Channel("nitro", 1.0f) < 1.0f && d.Channel("nitro_cooldown") <= 0.0f; }
	float Load() const { return State().load; }
	float Throttle() const { return State().throttle; }
	float Slip01() const { return State().slip; }
	bool Drifting() const { return ActivePolicyDebug().Channel("drift") > 0.5f; }
	float DriftAngleDeg() const;
	bool AtRedline() const { return State().rpm >= m_policy.GetHandling().rpmRedline; }
	bool LimiterHit() const { return State().limiter; }
	bool ConsumeShift() { return m_luaPolicy ? m_luaPolicy->ConsumeShift() : m_policy.ConsumeShift(); }
	bool HasCollided() const { return State().impact > 0.0f; }
	WheelState Wheel(int i) const { return State().wheels[i]; }
	bool HasWheelRig() const { return State().wheelRig; }

	Handling& MutableHandling() { return m_policy.MutableHandling(); }
	CameraRig& MutableCamera() { return m_rig; }
	glm::vec2 SpawnPosition() const { return State().spawnPos; }
	glm::vec3 SpawnPosition3() const { return glm::vec3(State().spawnPos.x, State().spawnY, State().spawnPos.y); }
	float SpawnHeading() const { return State().spawnHeading; }
	void SetSpawn(const glm::vec2& p, float heading) { m_service.SetSpawn(m_handle, p, heading); }
	nlohmann::json SaveTuning() const; // "handling" + "camera" objects

	static void ReadHandling(const nlohmann::json& j, Handling& h) { LegacyNfsPolicy::ReadHandling(j, h); }
	static void ReadCamera(const nlohmann::json& j, CameraRig& c);

	// Road surface height (procedural bumps on asphalt, curbs/sidewalks). Legacy planar mode.
	float GroundHeight(const glm::vec2& p) const { return m_service.GroundHeight(m_handle, p); }
	// Surface mode: ground under p searched downward from 'fromY'. Returns false over a gap.
	bool SurfaceHeight(const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal = nullptr) const {
		return m_service.SurfaceHeight(m_handle, p, fromY, maxDrop, outY, outNormal);
	}

private:
	VehicleState State() const { return m_service.State(m_handle); }
	float BaseY() const { const VehicleState s = State(); return s.surface ? s.y : 0.0f; }
	void SyncTuning();
	void RecordTelemetry(const VehicleSubstepRecord& r);   // NOX_VEHICLE_TELEMETRY (per sub-step)
	void UpdateCamera(float dt, Camera& camera, float aspect);
	VehiclePolicyDebug ActivePolicyDebug() const { return m_luaPolicy ? m_luaPolicy->Debug() : m_policy.Debug(); }
	// Creates the Lua controller (NOX_VEHICLE_POLICY=lua). A controller that fails to load is kept, faulted (the
	// car coasts, see LuaVehiclePolicy); the reason is printed.
	void CreateLuaPolicy(const nlohmann::json& handling);

	bool m_available = false;
	bool m_active = false;
	std::weak_ptr<SceneNode> m_player;
	const CollisionWorld* m_collision = nullptr;

	// Handling values, the numeric tuning source and the native-only camera data. With the Lua controller
	// selected this policy is NOT called: it only stores the handling profile that is handed to the controller.
	LegacyNfsPolicy m_policy;
	VehicleService m_service;
	VehicleHandle m_handle;

	// Transitional Lua controller (NOX_VEHICLE_POLICY=lua). Declared source, runtime, policy, recorder in that
	// order so each is destroyed before the object it refers to.
	std::unique_ptr<NoxLua::AssetScriptSource> m_luaSource;
	std::unique_ptr<NoxLua::Runtime> m_luaRuntime;
	std::unique_ptr<NoxLua::LuaVehiclePolicy> m_luaPolicy;
	std::unique_ptr<VehiclePolicyTrace::Recorder> m_recorder;   // NOX_VEHICLE_POLICY_TRACE
	size_t m_luaFaultsReported = 0;                             // controller faults already printed

	// Camera state.
	CameraRig m_rig;
	int m_cameraMode = 0;        // 0 chase, 1 far chase, 2 bumper, 3 free
	glm::vec3 m_camPos{ 0.0f };
	glm::vec3 m_camVel{ 0.0f };
	glm::vec3 m_camCarPrev{ 0.0f };
	float m_camHeading = 0.0f;
	float m_camFov = 55.0f;
	bool m_camInitialized = false;
	uint32_t m_camTeleportSerial = 0;
};
