#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include "json.hpp"

class RoadNetwork;

/*
* Reference-style racing HUD (docs/nfs2015/game.md): neon instrument cluster (tachometer, digital
* speed, boost/load and nitrous companion gauges, gear), translucent minimap with route guidance,
* world-anchored event marker, event/race panels, countdown, results and pause menus.
*
* Drawn with ImGui draw lists into a target rectangle (the Game viewport in the editor, the whole
* window in standalone game mode). It renders after scene post-processing, so its brightness is
* independent of scene exposure. Layout, colours, fonts and gauge ranges come from
* config/hud.json and can be edited live (DrawEditorPanel) and saved without recompiling.
*
* Layout is authored on a 640x360 reference canvas (the reference screenshot) and scaled by
* target height / 360, so it stays proportional at any aspect ratio. Each element is anchored to
* a point of the target rectangle ("TL","TC","TR","ML","MC","MR","BL","BC","BR").
*/
struct HudMapMarker {
	enum class Kind { Event, Checkpoint, NextCheckpoint, Finish, Start, Place };
	glm::vec2 pos{ 0.0f }; // world (x, z)
	Kind kind = Kind::Checkpoint;
};

struct HudState {
	// Vehicle.
	float speedKmh = 0.0f;
	float rpm = 900.0f;
	float throttle = 0.0f;       // 0..1
	float load = 0.0f;           // 0..1 engine load (boost gauge; throttle x rpm approximation)
	float nitro = 1.0f;          // 0..1 tank
	bool nitroActive = false;
	bool nitroRecharging = false;
	int gear = 1;                // -1 = R, 0 = N, 1..6
	bool redline = false;        // rpm at/above the redline (flash)
	bool limiter = false;        // rev limiter hit this frame

	// Map / navigation.
	glm::vec2 carPos{ 0.0f };    // world (x, z)
	float carY = 0.0f;                  // ground height under the car (stacked roads)
	bool layered = false;               // roads on other levels are drawn dimmed
	float carHeading = 0.0f;     // radians; 0 = +Z, increases toward +X
	const RoadNetwork* roads = nullptr;
	std::vector<glm::vec2> route;            // world polyline to the current target
	std::vector<HudMapMarker> markers;

	// Event / race presentation.
	enum class Phase { FreeRoam, NearEvent, Countdown, Racing, Finished, Paused };
	Phase phase = Phase::FreeRoam;
	std::string eventType = "SPRINT RACE";
	std::string eventName = "A Legend's Wheels";
	std::string eventDifficulty = "Hard";
	std::string startKey = "Enter";          // label of the start-event key/button
	float countdown = 0.0f;                  // seconds remaining (Countdown phase)
	float raceTime = 0.0f;
	int checkpoint = 0;                      // checkpoints cleared
	int checkpointCount = 0;
	float bestTime = -1.0f;                  // persisted local best (<0 = none)
	std::string bestHolder = "You";
	float lastTime = -1.0f;                  // finish time of the last run
	bool newRecord = false;
	std::vector<float> splits;               // checkpoint split times of the current/last run
	std::string message;                     // transient centre message (e.g. MISSED CHECKPOINT)
	float messageAlpha = 0.0f;

	// World-anchored event flag and next-checkpoint marker, projected by the game:
	// normalized [0,1] coordinates inside the target rectangle (y down).
	bool eventMarkerVisible = false;
	glm::vec2 eventMarkerNdc{ 0.5f };
	float eventMarkerDistance = 0.0f;
	bool checkpointMarkerVisible = false;
	glm::vec2 checkpointMarkerNdc{ 0.5f };
	float checkpointMarkerDistance = 0.0f;

	// Menus (pause, results). Empty title = no menu.
	std::string menuTitle;
	std::vector<std::string> menuItems;
	int menuSelected = 0;

	// Small top-left hint (camera view etc.).
	std::string hint;
};

// Placement of one HUD element: anchor code, offset in reference px, scale multiplier.
struct HudPlacement {
	std::string anchor = "BR";
	glm::vec2 offset{ 0.0f };
	float scale = 1.0f;
};

// Live HUD configuration (config/hud.json). Colours are RGBA 0..1.
struct HudConfig {
	HudPlacement cluster{ "BR", { 0.0f, 0.0f }, 1.0f };
	HudPlacement minimap{ "BL", { 0.0f, 0.0f }, 1.0f };
	HudPlacement event{ "TC", { 0.0f, 0.0f }, 1.0f };
	HudPlacement panel{ "TR", { 0.0f, 0.0f }, 1.0f };
	HudPlacement hint{ "TL", { 0.0f, 0.0f }, 1.0f };

	glm::vec4 accent{ 0.35f, 0.85f, 1.0f, 1.0f };     // cyan markings, route, checkpoints
	glm::vec4 needle{ 1.0f, 0.15f, 0.12f, 1.0f };     // needles
	glm::vec4 redline{ 1.0f, 0.12f, 0.10f, 1.0f };    // high-range markings, redline flash
	glm::vec4 speed{ 0.55f, 0.80f, 1.0f, 1.0f };      // digital speed panel
	glm::vec4 lavender{ 0.80f, 0.62f, 1.0f, 1.0f };   // event title and flag
	glm::vec4 text{ 1.0f, 1.0f, 1.0f, 1.0f };
	float backingAlpha = 0.35f;
	float glow = 0.6f;

	std::string labelFont = "fonts/BarlowCondensed-SemiBold.ttf";
	std::string digitalFont = "fonts/DSEG7Classic-Bold.ttf";
	std::string fallbackFont = "fonts/arial.ttf";
	float textScale = 1.0f;

	float rpmMax = 10000.0f;
	float redlineRpm = 8000.0f;
	std::string boostLabel = "BOOST";
	float boostMax = 1.0f;
	std::string n2oLabel = "N2O";
	std::string speedUnits = "km/h";         // "km/h" or "mph"

	float mapRadiusM = 60.0f;                // visible radius in metres at rest
	float mapZoomWithSpeed = 0.15f;          // extra metres of radius per km/h
	float mapOpacity = 0.75f;
	float mapRoadPx = 1.2f;                  // road line thickness (reference px)
	float mapRadiusPx = 42.0f;               // minimap radius (reference px)
};

class RacingHUD {
public:
	static RacingHUD& Get();
	RacingHUD();
	~RacingHUD();
	RacingHUD(const RacingHUD&) = delete;
	RacingHUD& operator=(const RacingHUD&) = delete;

	// Register HUD fonts in the ImGui atlas (call during ImGui init, before the first frame).
	// Loads the config first if it has not been loaded yet.
	void LoadFonts();
	bool LoadConfig(const std::string& path = "config/hud.json");
	bool SaveConfig(const std::string& path = "");
	const std::string& ConfigPath() const { return m_configPath; }
	const HudConfig& Config() const;
	HudConfig& MutableConfig();

	// Draw into [origin, origin + size] on the given draw list (clipped to that rectangle).
	void Draw(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, const HudState& state, float dt);

	// Editor controls for the live config (placement, colours, typography, gauge ranges, minimap).
	// Plain widgets: draws into the current ImGui window (wrap it in Begin/End yourself).
	void DrawEditorPanel();

	// Gauge range used for rpm normalisation by the game (from config).
	float RpmMax() const;
	float Redline() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	std::string m_configPath = "config/hud.json";
};
