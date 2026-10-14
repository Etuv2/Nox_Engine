#pragma once
#include <map>
#include <memory>
#include <tuple>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"
#include "GameModule.h"
#include "CollisionWorld.h"
#include "DriveMode.h"
#include "RoadNetwork.h"
#include "RacingHUD.h"
#include "WorldMapUI.h"

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
* in the user directory (saves/speedwall.json, seeded from the scene "game".speedwall_seed).
*
* Authoring data lives in the scene's "game" object: player/body nodes, spawn, handling, camera,
* road_network, events[{id, type, name, difficulty, start:[x,z] or [x,y,z], start_heading_deg,
* trigger_radius, countdown, checkpoints:[{pos:[x,z] or [x,y,z], width, heading_deg?}]}], hud (config
* path), collision {files:[...glb]} (docs/shuto: stacked roads; positions with a height only count
* within height_tolerance of the car, so a gate on an upper deck ignores the road below).
*/
class GameSession : public IGameModule {
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
	~GameSession() override;

	// IGameModule
	void InitializeResources() override;   // HUD fonts, UI icon atlas, drive audio
	// (Re)build from a scene's "game" object. sceneDir resolves the generated layout file.
	void Configure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& sceneGraph) override;
	// Same as Configure but keeps the selected event (editor edits).
	void Reconfigure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& sceneGraph) override;
	bool IsAvailable() const override { return m_drive.IsAvailable(); }
	bool WantsAutoPlay() const override;   // NOX_DRIVE=1
	nlohmann::json SaveGameJson() const override;   // authoring data as a scene "game" object

	// Play state. Editing: the car is posed at spawn and the camera frames it (no simulation).
	void BeginPlay() override;
	void EndPlay() override;
	bool IsPlaying() const { return m_playing; }
	void SetPaused(bool paused) override;
	bool IsPaused() const override { return m_paused; }

	// inputEnabled: the game view has focus and no text field is active.
	UpdateResult Update(float dt, bool inputEnabled, float aspect) override;
	void DrawHUD(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, float dt);
	void DrawOverlay(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, float dt) override { DrawHUD(dl, origin, size, dt); }
	// Full-screen world map (M / pad View): freezes driving and replaces the HUD while open.
	bool IsWorldMapOpen() const { return m_worldMap.IsOpen(); }
	void OpenWorldMap();
	void CloseWorldMap() { m_worldMap.Close(); }
	// Route waypoint (free roam): guidance routes to it until reached or toggled off with ENTER.
	void SetWaypoint(const glm::vec2& p);
	void SetWaypoint(const glm::vec2& p, float y);   // with the road height (stacked roads)
	void ClearWaypoint();
	bool HasWaypoint() const { return m_hasWaypoint; }
	glm::vec2 Waypoint() const { return m_waypoint; }
	bool ConsumeTeleport() override { return m_drive.ConsumeTeleport(); }
	// Menus, map, results and editing want a visible absolute pointer; driving does not.
	bool WantsPointer() const override;
	std::shared_ptr<Camera> GetCamera() const override { return m_camera; }
	bool GetPlayerPose(glm::vec3& pos) const override { const glm::vec2 p = m_drive.Position(); pos = glm::vec3(p.x, 0.0f, p.y); return m_drive.IsAvailable(); }
	void SetCameraTemplate(const Camera& editorCamera) override;

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
	void SetStandalone(bool s) override { m_standalone = s; }
	float BestTime(const std::string& eventId) const;
	void ClearBestTimes();
	// Effective gate heading (radians) of checkpoint i of event e.
	float GateHeading(const RaceEvent& e, int i) const;
	struct GateKey {
		float ax, az, bx, bz;
		bool operator<(const GateKey& o) const { return std::tie(ax, az, bx, bz) < std::tie(o.ax, o.az, o.bx, o.bz); }
	};
	mutable std::map<GateKey, float> m_gateHeadingCache;   // GateHeading results (road routing is costly)

private:
	struct Buttons {
		bool confirm = false, back = false, pause = false, reset = false, camera = false, up = false, down = false, start = false;
		bool map = false, recenter = false;   // M key / pad View (tap); F key
	};
	WorldMapContext BuildWorldMapContext() const;
	void UpdateWorldMap(const Buttons& b, bool inputEnabled, float dt);
	MenuAction Tick(float dt, bool inputEnabled, float aspect);
	void RefreshRoute();
	void UpdateMapPreview(int eventIndex);   // routed checkpoints of an event (cached per index)
	void ReadInput(bool enabled, DriveMode::Input& drive, Buttons& pressed);
	void UpdateRace(float dt);
	void UpdateMenus(const Buttons& b, MenuAction& action);
	void ActivateMenuItem(int index, MenuAction& action);
	// Mouse on the HUD (menus, start prompt) through the engine pointer layer.
	void UpdateMenuPointer(MenuAction& action, bool& startEvent);
	// Cursor policy (standalone): visible in menus/map, hidden while driving; focus-loss pause.
	void UpdateCursorPolicy(float dt);
	// NOX_TEST_POINTER_SCRIPT: synthetic pointer events + [PointerTest] PASS/FAIL checks.
	bool RunPointerScript();
	bool PointerTestExpect(const std::string& what);
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
	WorldMapUI m_worldMap;
	int m_previewIndex = -2;                 // event whose route is in m_mapPreview (-2 = none)
	std::vector<glm::vec2> m_mapPreview;
	bool m_hasWaypoint = false;
	glm::vec2 m_waypoint{ 0.0f };
	bool m_waypointHasY = false;
	float m_waypointY = 0.0f;
	// Pointer: the overlay rectangle of the last draw, whether the pointer is over it (editor: the
	// Game window is hovered), and whether game UI may use the mouse this frame.
	glm::vec2 m_overlayOrigin{ 0.0f }, m_overlaySize{ 0.0f };
	bool m_overlayHovered = false;
	bool m_pointerOn = false;
	int m_mapClickEvent = -1;           // last event marker selected by a mouse click (tests)
	float m_cursorReveal = 0.0f;        // s: cursor shown after mouse movement at the start prompt
	bool m_pauseOnFocusLoss = true;     // scene "game".pause_on_focus_loss / NOX_PAUSE_ON_FOCUS_LOSS
	struct PointerTest {
		std::vector<std::string> steps;
		size_t next = 0;
		int wait = 0;
		int untilFrames = 0;
		int pass = 0, fail = 0;
		glm::vec2 markCentre{ 0.0f }, markWorld{ 0.0f }, markPos{ 0.0f };
		float markPpm = 0.0f;
	} m_ptest;
	// Pad View/Back: a tap (released within 0.5 s) toggles the map, a hold resets the car.
	bool m_padBackDown = false;
	bool m_padBackHoldDone = false;
	float m_padBackHold = 0.0f;
	unsigned m_updateFrame = 0;     // NOX_TEST_* hooks count Update calls

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
	int m_routeKeep = 0;                 // consecutive refreshes that only trimmed the route (hysteresis)
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
