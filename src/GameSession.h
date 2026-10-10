#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"
#include "CollisionWorld.h"
#include "DriveMode.h"
#include "RoadNetwork.h"
#include "RacingHUD.h"

class Camera;
class SceneGraph;
struct ImDrawList;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

/*
* Racing prototype game layer (docs/nfs2015/game.md).
*
* Loop: free driving -> drive into the event marker -> start (Enter / pad X) -> countdown ->
* ordered checkpoints (gate crossing tests on the swept car path, so high speed cannot skip a gate;
* crossing a later gate first only shows MISSED CHECKPOINT) -> finish -> results (Retry / Free Roam).
* Pause menu (Esc / Start) with resume, restart, reset car and quit event. Local best times persist
* in config/speedwall.json.
*
* Authoring data lives in the scene's "game" object: player/body nodes, spawn, handling, camera,
* road_network, events[{id, type, name, difficulty, start:[x,z] or [x,y,z], start_heading_deg,
* trigger_radius, countdown, checkpoints:[{pos:[x,z] or [x,y,z], width, heading_deg?}]}], hud (config
* path), collision {files:[...glb]} (docs/shuto: stacked roads; positions with a height only count
* within height_tolerance of the car, so a gate on an upper deck ignores the road below).
*/
class GameSession {
public:
	struct Checkpoint { glm::vec2 pos{ 0.0f }; float width = 18.0f; float headingDeg = 0.0f; bool autoHeading = true; float y = 0.0f; bool hasY = false; };
	struct RaceEvent {
		std::string id = "sprint_01";
		std::string type = "SPRINT RACE";
		std::string name = "A Legend's Wheels";
		std::string difficulty = "Hard";
		glm::vec2 start{ 0.0f };
		float startY = 0.0f;
		bool hasStartY = false;
		float startHeadingDeg = 0.0f;
		float triggerRadius = 9.0f;
		float countdown = 3.0f;
		std::vector<Checkpoint> checkpoints; // last = finish
	};
	// Free-roam places (docs/shuto): parking areas used as rest, meeting and photo spots. Arriving
	// slowly shows the place, refills nitrous (rest) and lists the events that start nearby.
	struct Place {
		std::string id, type = "PARKING AREA", name, nameJa;
		glm::vec2 pos{ 0.0f };
		float y = 0.0f;
		bool hasY = false;
		float radius = 40.0f;
	};
	enum class Phase { FreeRoam, Countdown, Racing, Finished };
	enum class MenuAction { None, ExitApplication };

	GameSession();
	~GameSession();

	// (Re)build from a scene's "game" object. sceneDir resolves the generated layout file.
	void Configure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& sceneGraph);
	bool IsAvailable() const { return m_drive.IsAvailable(); }
	nlohmann::json SaveGameJson() const;   // authoring data as a scene "game" object

	// Play state. Editing: the car is posed at spawn and the camera frames it (no simulation).
	void BeginPlay();
	void EndPlay();
	bool IsPlaying() const { return m_playing; }
	void SetPaused(bool paused);
	bool IsPaused() const { return m_paused; }

	// inputEnabled: the game view has focus and no text field is active.
	MenuAction Update(float dt, bool inputEnabled, float aspect);
	void DrawHUD(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, float dt);
	bool ConsumeTeleport() { return m_drive.ConsumeTeleport(); }
	std::shared_ptr<Camera> GetCamera() const { return m_camera; }
	void SetCameraTemplate(const Camera& editorCamera);

	// Authoring access (editor panels).
	DriveMode& Drive() { return m_drive; }
	RoadNetwork& Roads() { return m_roads; }
	const CollisionWorld& Collision() const { return m_collision; }
	std::vector<RaceEvent>& Events() { return m_events; }
	const std::vector<Place>& Places() const { return m_places; }
	int SelectedEvent() const { return m_selectedEvent; }
	void SetSelectedEvent(int i) { m_selectedEvent = i; }
	Phase GetPhase() const { return m_phase; }
	float RaceTime() const { return m_raceTime; }
	int NextCheckpoint() const { return m_nextCheckpoint; }
	void StartEvent(int index);          // teleport + countdown
	void QuitEvent();
	void ResetCar();
	bool IsStandalone() const { return m_standalone; }
	void SetStandalone(bool s) { m_standalone = s; }
	float BestTime(const std::string& eventId) const;
	void ClearBestTimes();
	// Effective gate heading (radians) of checkpoint i of event e.
	float GateHeading(const RaceEvent& e, int i) const;

private:
	struct Buttons {
		bool confirm = false, back = false, pause = false, reset = false, camera = false, up = false, down = false, start = false;
	};
	void ReadInput(bool enabled, DriveMode::Input& drive, Buttons& pressed);
	void UpdateRace(float dt);
	void UpdateMenus(const Buttons& b, MenuAction& action);
	void BuildHudState(float dt);
	void ShowMessage(const std::string& text, float seconds = 2.0f);
	void EmitTyreEffects(float dt);
	void LoadBestTimes();
	void SaveBestTimes() const;
	glm::vec2 RouteTarget() const;
	glm::vec3 RouteTarget3() const;
	// Height gate for stacked roads: true when no height is authored or the car is within tolerance.
	bool AtHeight(bool hasY, float y) const;
	bool Project(const glm::vec3& world, glm::vec2& ndc, float& distance) const;

	DriveMode m_drive;
	RoadNetwork m_roads;
	CollisionWorld m_collision;
	float m_heightTolerance = 4.0f;
	std::vector<RaceEvent> m_events;
	std::vector<Place> m_places;
	int m_atPlace = -1;
	// Autopilot traversal diagnostics (NOX_DRIVE_AUTOPILOT): stuck / fall detection with recovery.
	float m_apStuck = 0.0f;
	float m_apLastY = 0.0f;
	float m_apFallTimer = 0.0f;
	int m_apIncidents = 0;
	bool m_exitAfterFinish = false;     // NOX_EXIT_ON_FINISH: validation runs quit after one finish
	nlohmann::json m_game;               // last configured authoring JSON (unknown keys preserved)
	std::shared_ptr<Camera> m_camera;
	SDL_GameController* m_pad = nullptr;
	HudState m_hud;
	bool m_playing = false;
	bool m_paused = false;
	bool m_standalone = false;
	int m_selectedEvent = 0;

	// Race state.
	Phase m_phase = Phase::FreeRoam;
	int m_activeEvent = -1;
	int m_nearEvent = -1;
	float m_countdown = 0.0f;
	int m_lastCountdownBeep = -1;
	float m_raceTime = 0.0f;
	int m_nextCheckpoint = 0;
	std::vector<float> m_splits;
	float m_wrongWayTimer = 0.0f;
	float m_lastTime = -1.0f;
	bool m_newRecord = false;
	std::string m_message;
	float m_messageTimer = 0.0f;
	std::vector<glm::vec2> m_route;
	float m_routeTimer = 0.0f;
	nlohmann::json m_bestTimes = nlohmann::json::object();

	// Menus.
	std::string m_menuTitle;
	std::vector<std::string> m_menuItems;
	int m_menuSelected = 0;

	// Edge detection for polled buttons.
	Buttons m_prevButtons;
	float m_aspect = 16.0f / 9.0f;
	float m_hudDt = 0.0f;
	std::string m_hudPath;
	float m_autoTimer = 0.0f;
	int m_autoRuns = 0;
	float m_scriptTime = 0.0f;
	int m_scriptPhase = 0;
	float m_phaseTime = 0.0f;
	float m_stuckTime = 0.0f;
};
