#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"

class Camera;
class SceneGraph;
class SceneNode;
struct ImFont;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

/*
* Drive mode (docs/nfs2015/README.md, M7): NFS-2015-style presentation layer.
*
*   * Arcade vehicle: throttle/brake/reverse, speed-dependent steering, handbrake drift with lateral
*     slip, nitrous, 6-speed gearbox/rpm for the HUD, body roll/pitch from accelerations. Not a
*     handling replica of the game (scope agreed with the user).
*   * Chase camera: critically damped follow with heading lag (shows the car's flank in drifts),
*     speed/nitrous FOV widening, pull-back under acceleration and high-speed shake. Alternative
*     views: far chase, bumper, free (editor) camera.
*   * HUD (ImGui draw lists, original artwork): speed, gear, rpm arc, nitrous gauge, rotating
*     minimap of the drivable areas.
*
* Scene file: "game": { "player_node", "body_node", "drivable_areas": [[xmin,zmin,xmax,zmax],...],
* "top_speed_kmh", "start_in_drive_mode" }. Toggle with F9 (or NOX_DRIVE=1).
* Controls: W/S or triggers = throttle/brake, A/D or left stick = steer, Space/B = handbrake,
* Shift/A = nitrous, C/Y = camera, R = reset to spawn.
*/
class DriveMode {
public:
	static void LoadFonts();

	void Configure(const std::string& sceneFilePath, SceneGraph& sceneGraph);
	bool IsAvailable() const { return m_available; }
	bool IsActive() const { return m_active; }
	void SetActive(bool active);
	void Toggle() { SetActive(!m_active); }
	void CycleCamera() { m_cameraMode = (m_cameraMode + 1) % 4; }
	void ResetToSpawn();
	// True once after a teleport (reset/activation): object motion that frame is not real motion.
	bool ConsumeTeleport() { const bool t = m_teleported; m_teleported = false; return t; }

	// Simulates the car and poses the camera. Returns true when the camera was driven.
	bool Update(float dt, Camera& camera);
	void DrawHUD(int width, int height) const;

private:
	struct Area { glm::vec2 min, max; };
	struct Input { float throttle = 0, brake = 0, steer = 0; bool handbrake = false, nitrous = false; };

	Input ReadInput();
	void Simulate(float dt, const Input& in);
	void ApplyTransforms();
	void UpdateCamera(float dt, Camera& camera);
	bool InsideDrivable(const glm::vec2& p) const;

	bool m_available = false;
	bool m_active = false;
	std::weak_ptr<SceneNode> m_player;
	std::weak_ptr<SceneNode> m_body;
	glm::vec3 m_bodyBasePos{ 0.0f };
	glm::vec3 m_bodyScale{ 1.0f };
	std::vector<Area> m_areas;
	SDL_GameController* m_pad = nullptr;

	// Vehicle state (metres, seconds, radians). Heading 0 = +Z.
	glm::vec2 m_pos{ 0.0f };
	glm::vec2 m_spawnPos{ 0.0f };
	float m_spawnHeading = 0.0f;
	float m_heading = 0.0f;
	float m_speed = 0.0f;        // forward m/s (negative = reverse)
	float m_lateral = 0.0f;      // sideways m/s (drift)
	float m_steer = 0.0f;        // smoothed steer [-1,1]
	float m_yawRate = 0.0f;
	float m_topSpeed = 78.0f;    // m/s (280 km/h)
	float m_nitro = 1.0f;
	bool m_nitroActive = false;
	int m_gear = 1;
	float m_rpm = 900.0f;
	float m_longAccel = 0.0f;
	float m_latAccel = 0.0f;
	float m_roll = 0.0f, m_pitch = 0.0f;

	// Camera state.
	int m_cameraMode = 0;        // 0 chase, 1 far chase, 2 bumper, 3 free
	glm::vec3 m_camPos{ 0.0f };
	glm::vec3 m_camVel{ 0.0f };
	float m_camHeading = 0.0f;
	float m_camFov = 55.0f;
	bool m_camInitialized = false;
	float m_time = 0.0f;
	bool m_teleported = false;
};
