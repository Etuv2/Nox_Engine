#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include "json.hpp"

class RoadNetwork;
class Texture;

/*
* Full-screen world map (game side, Shuto Expressway): satellite-style base image, carriageways drawn
* live as glowing expressway lines, the active route, race events, parking areas, the player arrow,
* district labels and the waypoint. Opened with M (pad View/Back tap); pans and zooms with WASD /
* stick, wheel / triggers and drag; F or Y recentres; ENTER / A toggles a waypoint on the selected
* event; ESC / B / M closes.
* Mouse (engine pointer layer, ui::Pointer): drag pans (only when the press starts on the map),
* the wheel zooms about the cursor, clicking a race marker selects it (route preview), clicking
* elsewhere places a waypoint on the nearest road, right-click clears it; the waypoint row of the
* event panel and the control hints (Center, Back, Clear) are clickable buttons.
*
* Data (scene "game" object, "world_map"): { "image": "textures/.../worldmap_base.png", "bounds":
* [x0, z0, x1, z1] (NOX metres, row 0 = north), "labels": "data/shuto/map_labels.json", "network":
* "data/shuto/network.json" (carriageway -> route ids and names), "attribution": "..." }. Every key is
* optional: without an image the map is a plain grid, without network metadata the roads are drawn
* as generic streets. Roads are never baked into the image.
*
* Units: world (x, z) in metres (x east, z south); screen is north up. Layout is authored at 1080 px
* height and scales with the target rectangle.
*/

struct WorldMapMarker {
	glm::vec2 pos{ 0.0f };          // start (marker position)
	glm::vec2 end{ 0.0f };          // finish (last checkpoint), used for the "A to B" subtitle
	bool hasEnd = false;
	std::string title;              // event name
	std::string type;               // "SPRINT RACE"
};

// Per-frame state supplied by the game (cheap to build; the map keeps no pointers past a call).
struct WorldMapContext {
	std::vector<WorldMapMarker> events;
	std::vector<glm::vec2> places;                  // parking areas
	glm::vec2 player{ 0.0f };
	float playerHeading = 0.0f;                     // radians, 0 = +Z (south), grows toward +X
	const std::vector<glm::vec2>* route = nullptr;  // active guidance route (world polyline)
	const std::vector<glm::vec2>* previewRoute = nullptr; // routed checkpoints of the displayed event
	bool hasWaypoint = false;
	glm::vec2 waypoint{ 0.0f };
	int selectedEvent = 0;
	bool padConnected = false;
};

// One frame of map input (filled by GameSession from SDL keyboard/pad; the mouse is read from the
// engine pointer layer when 'pointer' is set).
struct WorldMapInput {
	glm::vec2 pan{ 0.0f };           // -1..1 (x east, y south): keys or left stick
	float zoomSteps = 0.0f;          // extra zoom notches this frame (+ = in), about the map centre
	float zoomRate = 0.0f;           // -1..1 continuous (triggers), + = in
	bool pointer = false;            // the map may use the mouse (game view focused and hovered)
	bool confirm = false;            // ENTER / pad A (pressed this frame)
	bool recenter = false;           // F / pad Y (pressed this frame)
	bool close = false;              // ESC / Backspace / pad B / M / pad View (pressed this frame)
	int cycle = 0;                   // -1 / +1: previous / next event (Tab, pad D-pad)
};

struct WorldMapAction {
	bool close = false;
	bool toggleWaypoint = false;     // ENTER (or the panel button) on the selected event
	int eventIndex = -1;
	bool placeWaypoint = false;      // click on the map: waypoint at the nearest road to worldPos
	glm::vec2 worldPos{ 0.0f };
	float snapRadius = 0.0f;         // metres: the road must be this close to the click
	bool clearWaypoint = false;      // right-click / Clear button
	bool selectedByClick = false;    // a marker click changed the selection
};

// Pointer regions registered by the map while drawing (ids: ui::HashId(name, index)).
namespace WorldMapRegion {
	constexpr const char* kView = "map.view";        // whole map (drag, wheel, click = waypoint)
	constexpr const char* kEvent = "map.event";      // index = event marker
	constexpr const char* kButton = "map.btn";       // index = WorldMapButton
}
enum WorldMapButton { MapBtnWaypoint = 0, MapBtnCenter = 1, MapBtnBack = 2, MapBtnClear = 3, MapBtnCount = 4 };

class WorldMapUI {
public:
	WorldMapUI();
	~WorldMapUI();

	// Loads the optional base image, labels and carriageway metadata, and builds the simplified
	// per-carriageway polylines from the road network (once per scene configuration).
	void Configure(const nlohmann::json& worldMapCfg, const RoadNetwork& roads);

	void Open(const WorldMapContext& ctx);
	void Close() { m_open = false; }
	bool IsOpen() const { return m_open; }

	// Advances pan, zoom and selection from this frame's input. Mouse hit tests use the rectangle
	// and markers of the previous Draw.
	WorldMapAction Update(const WorldMapInput& in, const WorldMapContext& ctx, float dt);
	void Draw(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, const WorldMapContext& ctx, float dt);

	int SelectedEvent() const { return m_selected; }
	// View state (tests, tools): centre in world metres, zoom in pixels per metre, and the world
	// point under a window position for the last drawn rectangle.
	glm::vec2 ViewCentre() const { return m_centre; }
	float PixelsPerMetre() const { return m_ppm; }
	glm::vec2 WorldAt(const glm::vec2& windowPos) const { return ScreenToWorld(windowPos, m_origin, m_size); }
	// Event shown in the panel and previewed on the map: hovered, else selected (-1 when none).
	int DisplayedEvent() const { return m_hovered >= 0 ? m_hovered : m_selected; }

private:
	// One drivable carriageway (or merged run of chains): simplified polylines per detail level.
	struct ChainInfo {
		int route = -1;                 // index into m_routeNames, -1 = unknown
		uint8_t kind = 0;               // 0 main, 1 ramp / junction, 2 other street
		glm::vec2 mn{ 0.0f }, mx{ 0.0f };
		std::array<std::vector<glm::vec2>, 4> lv;   // tolerance levels (see kTolerance)
	};
	struct ScreenRun { int chain = 0; size_t offset = 0; size_t count = 0; };
	struct MapLabel { std::string text; glm::vec2 pos{ 0.0f }; int kind = 0; };   // kind: 0 route, 1 district, 2 water

	void BuildChains(const RoadNetwork& roads, const std::string& networkPath);
	void BuildLabels(const std::string& labelsPath);
	void UpdateScreenCache(const ImVec2& origin, const ImVec2& size);
	glm::vec2 WorldToScreen(const glm::vec2& w, const ImVec2& origin, const ImVec2& size) const;
	glm::vec2 ScreenToWorld(const glm::vec2& s, const ImVec2& origin, const ImVec2& size) const;
	int LevelForZoom() const;
	float DefaultPpm(const ImVec2& size) const;
	int NearestEvent(const WorldMapContext& ctx, const glm::vec2& mouse, float radius) const;
	std::string DistrictNear(const glm::vec2& p, float maxDist) const;
	ImFont* Font(const char* family, float px) const;
	void RecordDrawTime(double us);
	void DrawGlowPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 core, float coreW, ImU32 glow, float glowW);
	void DrawEventMarker(ImDrawList* dl, const ImVec2& c, float s, bool selected, bool hovered);
	void DrawParkingMarker(ImDrawList* dl, const ImVec2& c, float s);
	void DrawPlayer(ImDrawList* dl, const ImVec2& c, float heading, float s);
	void DrawSpacedText(ImDrawList* dl, ImFont* font, float px, ImVec2 p, ImU32 col, const char* text, float spacing, bool centre);
	void DrawKeyCap(ImDrawList* dl, ImFont* font, float px, ImVec2 p, const char* key, ImU32 keyCol, float s, float* outWidth);

	bool m_open = false;
	bool m_configured = false;
	glm::vec2 m_centre{ 0.0f };
	float m_ppm = 0.05f;             // pixels per metre
	bool m_viewInit = false;
	int m_selected = 0;
	int m_hovered = -1;
	bool m_dragging = false;
	int m_hoverButton = -1, m_pressButton = -1;   // pointer feedback on the map buttons
	ImVec2 m_origin{ 0, 0 }, m_size{ 0, 0 };
	float m_time = 0.0f;

	// Base image and its world bounds (north-up, NOX metres).
	std::unique_ptr<Texture> m_base;
	glm::vec2 m_mapMin{ 0.0f }, m_mapMax{ 0.0f };
	bool m_hasMapBounds = false;
	std::string m_attribution;

	std::vector<std::string> m_routeNames;
	std::vector<ChainInfo> m_chains;
	std::vector<MapLabel> m_labels;

	// Screen-space cache for the current view (rebuilt when the view, size or detail level changes).
	std::vector<ImVec2> m_screenPts;
	std::vector<ScreenRun> m_runs;
	glm::vec2 m_cacheCentre{ 0.0f };
	float m_cachePpm = 0.0f;
	ImVec2 m_cacheSize{ 0, 0 };
	int m_cacheLevel = -1;
	bool m_cacheValid = false;
	std::vector<ImVec2> m_markerScreen;    // event marker centres (last Draw)
	// Draw timing (NOX_WORLDMAP_TIMING=1 logs the average every 120 draws).
	double m_timeSumUs = 0.0, m_timeMaxUs = 0.0;
	int m_timeCount = 0;
};
