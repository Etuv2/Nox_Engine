#include "GameSession.h"
#include "FrameProfiler.h"

#include "Camera.h"
#include "DriveAudio.h"
#include "VehicleFX.h"
#include "ProceduralStreetKit.h"
#include "SceneGraph.h"
#include "ui/Pointer.h"
#include <SDL/SDL.h>
#include <IMGUI/imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <iostream>

using nlohmann::json;

namespace {
	constexpr float kPi = 3.14159265f;
	const char* kBestTimesPath = "config/speedwall.json";

	// [x, z] or [x, y, z].
	glm::vec2 ReadXZ(const json& j, const glm::vec2& fallback) {
		if (j.is_array() && j.size() >= 3 && j[0].is_number() && j[2].is_number()) return glm::vec2(j[0].get<float>(), j[2].get<float>());
		if (j.is_array() && j.size() >= 2 && j[0].is_number() && j[1].is_number()) return glm::vec2(j[0].get<float>(), j[1].get<float>());
		return fallback;
	}
	bool ReadY(const json& j, float& y) {
		if (j.is_array() && j.size() >= 3 && j[1].is_number()) { y = j[1].get<float>(); return true; }
		return false;
	}
	json WritePos(const glm::vec2& p, bool hasY, float y) {
		return hasY ? json{ p.x, y, p.y } : json{ p.x, p.y };
	}

	// Segment-segment intersection (p0->p1 against q0->q1).
	bool SegmentsIntersect(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& q0, const glm::vec2& q1) {
		const glm::vec2 r = p1 - p0, s = q1 - q0;
		const float denom = r.x * s.y - r.y * s.x;
		if (std::abs(denom) < 1e-8f) return false;
		const glm::vec2 qp = q0 - p0;
		const float t = (qp.x * s.y - qp.y * s.x) / denom;
		const float u = (qp.x * r.y - qp.y * r.x) / denom;
		return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
	}

	std::string LayoutPathFromScene(const json& sceneRoot) {
		if (!sceneRoot.is_object() || !sceneRoot.contains("nodes") || !sceneRoot["nodes"].is_array()) return {};
		for (const json& n : sceneRoot["nodes"]) {
			if (!n.is_object() || !n.contains("procedural")) continue;
			const json& p = n["procedural"];
			if (p.value("generator", std::string()) != "district") continue;
			const json params = p.value("params", json::object());
			return params.value("output", std::string("generated/nfs_district")) + "_layout.json";
		}
		return {};
	}
}

GameSession::GameSession() = default;

GameSession::~GameSession()
{
	if (m_pad) SDL_GameControllerClose(m_pad);
}

void GameSession::SetCameraTemplate(const Camera& c)
{
	m_camera = std::make_shared<Camera>(c.GetCameraPosition(), glm::vec3(0, 1, 0), 90.0f, 0.0f, 50.0f,
		std::max(c.GetCameraNearPlane(), 0.05f), std::max(c.GetCameraFarPlane(), 600.0f), 10.0f, 0.1f);
	m_camera->SetProjectionType(Camera::ProjectionType::Perspective);
}

// VehicleFX test harness (NOX_VFX_TEST=1): scene game.vfx_test_emitters, else the nfs_sprint spawn layout
// (x=-2, z=-40, road at y=0). The engine keeps no positions of its own.
static void ApplyVfxTestEmitters(const json& game)
{
	if (!VehicleFX::TestHarnessActive()) return;
	std::vector<VehicleFX::TestEmitter> emitters;
	if (game.contains("vfx_test_emitters") && game["vfx_test_emitters"].is_array()) {
		for (const json& e : game["vfx_test_emitters"]) {
			if (!e.is_object() || !e.contains("pos") || !e["pos"].is_array() || e["pos"].size() != 3) continue;
			VehicleFX::TestEmitter t;
			t.spray = e.value("type", std::string("smoke")) == "spray";
			t.pos = glm::vec3(e["pos"][0].get<float>(), e["pos"][1].get<float>(), e["pos"][2].get<float>());
			t.floorY = e.value("floor_y", 0.0f);
			if (e.contains("velocity") && e["velocity"].is_array() && e["velocity"].size() == 3) {
				t.velocity = glm::vec3(e["velocity"][0].get<float>(), e["velocity"][1].get<float>(), e["velocity"][2].get<float>());
			}
			t.intensity = e.value("intensity", t.intensity);
			t.wobble = e.value("wobble", t.wobble);
			emitters.push_back(t);
		}
	}
	else {
		emitters.push_back({ false, glm::vec3(-2.9f, 0.3f, -41.0f), glm::vec3(0.0f), 1.0f, 0.0f, 0.0f });   // burnout, left rear
		emitters.push_back({ false, glm::vec3(-1.1f, 0.3f, -41.0f), glm::vec3(0.0f), 0.5f, 0.4f, 0.0f });   // wheelspin, right rear
		emitters.push_back({ true, glm::vec3(-2.0f, 0.02f, -44.0f), glm::vec3(0.0f, 0.0f, -6.0f), 0.9f, 0.0f, 0.0f }); // wet-road spray
	}
	VehicleFX::SetTestEmitters(emitters);
}

void GameSession::Configure(const json& game, const json& sceneRoot, SceneGraph& sceneGraph)
{
	m_game = game.is_object() ? game : json::object();
	m_playing = false;
	m_paused = false;
	m_phase = Phase::FreeRoam;
	m_activeEvent = -1;
	m_menuTitle.clear();
	m_menuItems.clear();
	m_route.clear();

	// Static collision (stacked roads, tunnels, barriers): enables DriveMode's surface mode.
	m_collision.Clear();
	m_heightTolerance = 4.0f;
	if (m_game.contains("collision") && m_game["collision"].is_object()) {
		const json& c = m_game["collision"];
		m_heightTolerance = c.value("height_tolerance", m_heightTolerance);
		// Wall sliver rules for this game's collision set (engine defaults are neutral).
		CollisionWorld::TriangleFilter filter;
		filter.groundMinNormalY = c.value("ground_min_normal_y", filter.groundMinNormalY);
		filter.minWallHeight = c.value("wall_min_height", 0.15f);
		filter.minWallAspect = c.value("wall_min_aspect", 0.05f);
		m_collision.filter = filter;
		if (c.contains("files") && c["files"].is_array()) {
			for (const json& f : c["files"]) {
				if (f.is_string()) m_collision.LoadGLTF(f.get<std::string>());
			}
		}
		std::cout << "[GameSession] Collision: " << m_collision.MeshCount() << " meshes, " << m_collision.TriangleCount() << " triangles" << std::endl;
	}

	ApplyVfxTestEmitters(m_game);

	// Road network: authored, else derived from legacy drivable rectangles, else none.
	m_roads.Clear();
	if (!m_roads.Load(m_game.value("road_network", json()))) {
		if (m_game.value("default_district", false)) {
			ProceduralStreetKit::BuildDefaultDistrictNetwork(m_roads);
		}
		else if (m_game.contains("drivable_areas") && m_game["drivable_areas"].is_array()) {
			json net = { {"road_half_width", 6.0f}, {"sidewalk_width", 0.0f}, {"nodes", json::array()}, {"edges", json::array()} };
			int k = 0;
			for (const json& a : m_game["drivable_areas"]) {
				if (!a.is_array() || a.size() < 4) continue;
				const float x0 = a[0], z0 = a[1], x1 = a[2], z1 = a[3];
				const bool alongZ = (z1 - z0) >= (x1 - x0);
				const std::string ia = "R" + std::to_string(k) + "a", ib = "R" + std::to_string(k) + "b";
				if (alongZ) {
					net["nodes"].push_back({ {"id", ia}, {"pos", { (x0 + x1) * 0.5f, z0 }} });
					net["nodes"].push_back({ {"id", ib}, {"pos", { (x0 + x1) * 0.5f, z1 }} });
					net["edges"].push_back({ {"a", ia}, {"b", ib}, {"half_width", (x1 - x0) * 0.5f} });
				}
				else {
					net["nodes"].push_back({ {"id", ia}, {"pos", { x0, (z0 + z1) * 0.5f }} });
					net["nodes"].push_back({ {"id", ib}, {"pos", { x1, (z0 + z1) * 0.5f }} });
					net["edges"].push_back({ {"a", ia}, {"b", ib}, {"half_width", (z1 - z0) * 0.5f} });
				}
				++k;
			}
			m_roads.Load(net);
		}
	}

	m_events.clear();
	if (m_game.contains("events") && m_game["events"].is_array()) {
		for (const json& e : m_game["events"]) {
			if (!e.is_object()) continue;
			RaceEvent ev;
			ev.id = e.value("id", ev.id);
			ev.type = e.value("type", ev.type);
			ev.name = e.value("name", ev.name);
			ev.difficulty = e.value("difficulty", ev.difficulty);
			ev.start = ReadXZ(e.value("start", json()), ev.start);
			ev.hasStartY = ReadY(e.value("start", json()), ev.startY);
			ev.startHeadingDeg = e.value("start_heading_deg", ev.startHeadingDeg);
			ev.triggerRadius = e.value("trigger_radius", ev.triggerRadius);
			ev.countdown = e.value("countdown", ev.countdown);
			if (e.contains("checkpoints") && e["checkpoints"].is_array()) {
				for (const json& c : e["checkpoints"]) {
					Checkpoint cp;
					if (c.is_array()) { cp.pos = ReadXZ(c, cp.pos); cp.hasY = ReadY(c, cp.y); }
					else if (c.is_object()) {
						cp.pos = ReadXZ(c.value("pos", json()), cp.pos);
						cp.hasY = ReadY(c.value("pos", json()), cp.y);
						cp.width = c.value("width", cp.width);
						if (c.contains("heading_deg")) {
							cp.headingDeg = c["heading_deg"].get<float>();
							cp.autoHeading = false;
						}
					}
					ev.checkpoints.push_back(cp);
				}
			}
			m_events.push_back(ev);
		}
	}
	m_selectedEvent = std::clamp(m_selectedEvent, 0, std::max(0, static_cast<int>(m_events.size()) - 1));

	m_places.clear();
	m_atPlace = -1;
	if (m_game.contains("places") && m_game["places"].is_array()) {
		for (const json& p : m_game["places"]) {
			if (!p.is_object()) continue;
			Place pl;
			pl.id = p.value("id", std::string());
			pl.type = p.value("type", pl.type);
			pl.name = p.value("name", std::string());
			pl.nameJa = p.value("name_ja", std::string());
			pl.pos = ReadXZ(p.value("pos", json()), pl.pos);
			pl.hasY = ReadY(p.value("pos", json()), pl.y);
			pl.radius = p.value("radius", pl.radius);
			m_places.push_back(pl);
		}
	}

	// World map (scene "game".world_map): base image, labels, carriageway metadata.
	m_worldMap.Configure(m_game.value("world_map", json()), m_roads);
	m_worldMap.Close();
	m_hasWaypoint = false;
	m_waypointHasY = false;
	// Standalone: losing window focus while driving opens the pause menu (never in unattended runs,
	// where other windows take the focus).
	m_pauseOnFocusLoss = m_game.value("pause_on_focus_loss", true);
	if (const char* v = std::getenv("NOX_PAUSE_ON_FOCUS_LOSS")) m_pauseOnFocusLoss = v[0] == '1';
	else if (std::getenv("NOX_DRIVE_AUTOPILOT") || std::getenv("NOX_DRIVE_SCRIPT") || std::getenv("NOX_TEST_POINTER_SCRIPT")) m_pauseOnFocusLoss = false;
	m_previewIndex = -2;

	m_drive.Configure(m_game, sceneGraph, &m_roads, LayoutPathFromScene(sceneRoot), &m_collision);
	if (m_drive.IsAvailable()) m_drive.SetActive(true);

	const std::string hudPath = m_game.value("hud", std::string("config/hud.json"));
	if (hudPath != m_hudPath) {
		RacingHUD::Get().LoadConfig(hudPath);
		m_hudPath = hudPath;
	}
	LoadBestTimes();
	if (!m_pad) {
		for (int i = 0; i < SDL_NumJoysticks(); ++i) {
			if (SDL_IsGameController(i)) { m_pad = SDL_GameControllerOpen(i); break; }
		}
	}
}

json GameSession::SaveGameJson() const
{
	json g = m_game;
	const json tuning = m_drive.SaveTuning();
	g["handling"] = tuning["handling"];
	g["camera"] = tuning["camera"];
	g.erase("top_speed_kmh");
	const glm::vec2 sp = m_drive.SpawnPosition();
	const glm::vec3 sp3 = m_drive.SpawnPosition3();
	g["spawn"] = { {"position", WritePos(sp, m_drive.HasSurface() && sp3.y < 1e4f, sp3.y)}, {"heading_deg", glm::degrees(m_drive.SpawnHeading())} };
	if (!m_roads.Empty() && !g.contains("drivable_areas")) g["road_network"] = m_roads.Save();
	json events = json::array();
	for (const RaceEvent& e : m_events) {
		json je = { {"id", e.id}, {"type", e.type}, {"name", e.name}, {"difficulty", e.difficulty},
			{"start", WritePos(e.start, e.hasStartY, e.startY)}, {"start_heading_deg", e.startHeadingDeg},
			{"trigger_radius", e.triggerRadius}, {"countdown", e.countdown} };
		json cps = json::array();
		for (const Checkpoint& c : e.checkpoints) {
			json jc = { {"pos", WritePos(c.pos, c.hasY, c.y)}, {"width", c.width} };
			if (!c.autoHeading) jc["heading_deg"] = c.headingDeg;
			cps.push_back(jc);
		}
		je["checkpoints"] = cps;
		events.push_back(je);
	}
	g["events"] = events;
	g["hud"] = RacingHUD::Get().ConfigPath();
	return g;
}

float GameSession::GateHeading(const RaceEvent& e, int i) const
{
	const Checkpoint& c = e.checkpoints[i];
	if (!c.autoHeading) return glm::radians(c.headingDeg);
	// Direction of travel into the gate: from the previous gate (or the start) to this one, snapped
	// to the road direction at the gate when a network exists.
	const glm::vec2 prev = i == 0 ? e.start : e.checkpoints[i - 1].pos;
	// A road route per gate is expensive on a large network and the gate tests run every frame:
	// cache by the two gate positions (edits that move a gate simply miss the cache).
	const GateKey key{ prev.x, prev.y, c.pos.x, c.pos.y };
	if (const auto it = m_gateHeadingCache.find(key); it != m_gateHeadingCache.end()) return it->second;
	glm::vec2 dir = c.pos - prev;
	if (!m_roads.Empty()) {
		std::vector<glm::vec2> path = m_roads.FindRoute(prev, c.pos);
		if (path.size() >= 2) dir = path.back() - path[path.size() - 2];
		if (glm::length(dir) < 1e-3f) dir = c.pos - prev;
	}
	if (glm::length(dir) < 1e-3f) dir = glm::vec2(0, 1);
	const float heading = std::atan2(dir.x, dir.y);
	if (m_gateHeadingCache.size() > 4096) m_gateHeadingCache.clear();
	m_gateHeadingCache.emplace(key, heading);
	return heading;
}

void GameSession::BeginPlay()
{
	m_playing = true;
	m_paused = false;
	m_phase = Phase::FreeRoam;
	m_menuTitle.clear();
	m_drive.ResetToSpawn();
	// NOX_AUTOPILOT_EVENT=<index>: scripted traversal starts directly at that event (validation).
	static const int autoEvent = [] { const char* v = std::getenv("NOX_AUTOPILOT_EVENT"); return v ? std::atoi(v) : -1; }();
	if (autoEvent >= 0 && autoEvent < static_cast<int>(m_events.size())) {
		m_selectedEvent = autoEvent;
		std::cout << "[Traversal] Event " << autoEvent << " '" << m_events[autoEvent].name << "'" << std::endl;
		StartEvent(autoEvent);
	}
}

void GameSession::EndPlay()
{
	m_playing = false;
	m_paused = false;
	m_phase = Phase::FreeRoam;
	m_activeEvent = -1;
	m_menuTitle.clear();
	m_message.clear();
	m_drive.ResetToSpawn();
	DriveAudioState silent;
	DriveAudio::Get().Update(silent);
}

void GameSession::SetPaused(bool paused)
{
	if (!m_playing) return;
	m_paused = paused;
	if (paused) {
		m_menuTitle = "PAUSED";
		m_menuItems = { "Resume" };
		if (m_phase == Phase::Countdown || m_phase == Phase::Racing) {
			m_menuItems.push_back("Restart Event");
			m_menuItems.push_back("Quit Event");
		}
		m_menuItems.push_back("Reset Car");
		if (m_standalone) m_menuItems.push_back("Exit Game");
		m_menuSelected = 0;
		DriveAudio::Get().Play(UiSound::Back);
	}
	else if (m_menuTitle == "PAUSED") {
		m_menuTitle.clear();
	}
}

void GameSession::ReadInput(bool enabled, DriveMode::Input& in, Buttons& pressed)
{
	Buttons now;
	if (enabled) {
		// Keyboard state only counts while one of our windows has keyboard focus.
		static Uint8 none[SDL_NUM_SCANCODES] = {};
		const Uint8* k = SDL_GetKeyboardFocus() ? SDL_GetKeyboardState(nullptr) : none;
		in.throttle = (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) ? 1.0f : 0.0f;
		in.brake = (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) ? 1.0f : 0.0f;
		in.steer = ((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) ? 1.0f : 0.0f) - ((k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) ? 1.0f : 0.0f);
		in.handbrake = k[SDL_SCANCODE_SPACE] != 0;
		in.nitrous = (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) != 0;
		now.confirm = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];
		now.start = now.confirm || k[SDL_SCANCODE_PAGEDOWN];
		now.back = k[SDL_SCANCODE_BACKSPACE] != 0;
		now.pause = k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_P];
		now.reset = k[SDL_SCANCODE_R] != 0;
		now.camera = k[SDL_SCANCODE_C] != 0;
		now.map = k[SDL_SCANCODE_M] != 0;
		now.recenter = k[SDL_SCANCODE_F] != 0;
		now.up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W];
		now.down = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
		if (m_pad && SDL_GameControllerGetAttached(m_pad)) {
			const float rt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
			const float lt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
			float lx = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
			const float ly = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
			lx = std::abs(lx) < 0.12f ? 0.0f : (lx - 0.12f * (lx > 0 ? 1.0f : -1.0f)) / 0.88f;
			in.throttle = std::max(in.throttle, rt);
			in.brake = std::max(in.brake, lt);
			if (std::abs(lx) > std::abs(in.steer)) in.steer = lx;
			in.handbrake = in.handbrake || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_B);
			in.nitrous = in.nitrous || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_A);
			now.confirm = now.confirm || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_A);
			now.start = now.start || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_X);
			now.back = now.back || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_B);
			now.pause = now.pause || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_START);
			// View/Back: a tap (released before 0.5 s) toggles the world map; a hold resets the car.
			const bool backDown = SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
			if (backDown) {
				m_padBackHold += m_hudDt;
				if (!m_padBackHoldDone && m_padBackHold >= 0.5f) { now.reset = true; m_padBackHoldDone = true; }
			}
			else {
				if (m_padBackDown && !m_padBackHoldDone) now.map = true;
				m_padBackHold = 0.0f;
				m_padBackHoldDone = false;
			}
			m_padBackDown = backDown;
			now.camera = now.camera || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_Y);
			now.up = now.up || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -0.6f;
			now.down = now.down || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 0.6f;
		}
	}
	// NOX_DRIVE_AUTOPILOT=1: scripted driver for unattended validation (follows the route, starts the
	// event, retries once from the results menu, then returns to free roam).
	// NOX_DRIVE_SCRIPT=maneuvers: timed handling test (burnout, launch, hard brake, weave, handbrake
	// drift, brake-tap drift) on the straight ahead of the spawn; used for validation recordings.
	static const bool maneuvers = [] { const char* v = std::getenv("NOX_DRIVE_SCRIPT"); return v && std::string(v) == "maneuvers"; }();
	if (maneuvers && m_playing) {
		m_scriptTime += m_hudDt;
		const float t = m_scriptTime;
		in = DriveMode::Input();
		now = Buttons();
		// Position-driven test driver: burnout, launch, hard brake, weave, handbrake drift left into
		// 3rd St (z = 200), brake-tap drift right onto Palm Blvd (x = 150), stop.
		const glm::vec2 p = m_drive.Position();
		const float head = m_drive.Heading();
		const float kmh = m_drive.SpeedKmh();
		auto wrap = [](float x) { while (x > kPi) x -= 2.0f * kPi; while (x < -kPi) x += 2.0f * kPi; return x; };
		// Lane keeping toward a target heading/lateral offset (heading grows toward +X = the car's left).
		auto holdLine = [&](float desiredHeading, float lateralError) {
			const float want = desiredHeading + std::clamp(lateralError * 0.12f, -0.4f, 0.4f);
			return std::clamp(-wrap(want - head) * 2.6f / (1.0f + kmh / 140.0f), -1.0f, 1.0f);
		};
		// Blocked against something (wall, pole): reverse out briefly, then resume the phase.
		if (m_scriptPhase >= 2 && m_scriptPhase < 12 && kmh < 2.0f && m_scriptPhase != 3) m_stuckTime += m_hudDt; else if (kmh > 6.0f) m_stuckTime = 0.0f;
		if (m_stuckTime > 1.0f) {
			in.brake = 1.0f; in.throttle = 0.0f; in.steer = 0.8f;
			if (m_stuckTime > 2.4f) m_stuckTime = 0.0f;
		}
		else switch (m_scriptPhase) {
		case 0: if (t > 0.5f) m_scriptPhase = 1; break;
		case 1: in.throttle = 1.0f; in.brake = 1.0f; if (t > 2.5f) m_scriptPhase = 2; break;           // burnout
		case 2: in.throttle = 1.0f; in.steer = holdLine(0.0f, -2.0f - p.x); if (p.y > 15.0f) m_scriptPhase = 3; break; // launch
		case 3: in.brake = 1.0f; in.steer = holdLine(0.0f, -2.0f - p.x); if (kmh < 30.0f) m_scriptPhase = 4; break;   // hard brake
		case 4: {                                                                                       // weave
			in.throttle = 0.65f;
			const float targetX = -2.0f - 2.4f * std::sin(std::max(p.y - 45.0f, 0.0f) / 11.0f);
			in.steer = holdLine(0.0f, targetX - p.x);
			if (p.y > 130.0f) m_scriptPhase = 5;
			break; }
		case 5: in.throttle = kmh < 72.0f ? 1.0f : 0.0f; in.brake = kmh > 82.0f ? 0.6f : 0.0f; in.steer = holdLine(0.0f, -2.0f - p.x); if (p.y > 176.0f) m_scriptPhase = 6; m_phaseTime = 0.0f; break;
		case 6: in.throttle = 0.6f; in.steer = -1.0f; in.handbrake = true; m_phaseTime += m_hudDt;      // handbrake entry (left)
			if (m_phaseTime > 0.35f) { m_scriptPhase = 7; m_phaseTime = 0.0f; } break;
		case 7: in.throttle = 1.0f; m_phaseTime += m_hudDt;                                              // hold the drift
			in.steer = wrap(kPi * 0.5f - head) > 0.6f ? -0.6f : 0.0f;
			if (wrap(kPi * 0.5f - head) < 0.15f || m_phaseTime > 3.5f) m_scriptPhase = 8; break;
		case 8: in.throttle = kmh < (p.x > 80.0f ? 74.0f : 100.0f) ? 0.9f : 0.0f; in.brake = kmh > (p.x > 80.0f ? 84.0f : 120.0f) ? 0.7f : 0.0f;
			in.steer = holdLine(kPi * 0.5f, p.y - 202.0f);
			if (p.x > 129.0f) { m_scriptPhase = 9; m_phaseTime = 0.0f; } break;
		case 9: in.brake = 1.0f; in.steer = -0.8f; m_phaseTime += m_hudDt;                                // brake tap
			if (m_phaseTime > 0.25f) { m_scriptPhase = 10; m_phaseTime = 0.0f; } break;
		case 10: in.throttle = 1.0f; m_phaseTime += m_hudDt;                                             // brake-tap drift left onto Palm Blvd
			in.steer = wrap(kPi - head) > 0.3f ? -0.8f : holdLine(kPi, p.x - 146.0f);
			if (std::abs(wrap(kPi - head)) < 0.3f || m_phaseTime > 3.5f) m_scriptPhase = 11; break;
		case 11: in.throttle = 0.7f; in.steer = holdLine(kPi, p.x - 146.0f); m_phaseTime += m_hudDt; if (m_phaseTime > 3.0f) m_scriptPhase = 12; break;
		default: in.brake = 1.0f; break;
		}
		static float logTimer = 0.0f;
		logTimer += m_hudDt;
		if (logTimer >= 0.25f) {
			logTimer = 0.0f;
			std::printf("[Telemetry] ph=%d t=%5.2f v=%6.1f km/h gear=%d rpm=%5.0f drift=%d beta=%5.1f spin=%.2f slip=%.2f pos=(%.1f,%.1f) head=%.1f fx=%d\n",
				m_scriptPhase, t, m_drive.SpeedKmh(), m_drive.Gear(), m_drive.Rpm(), m_drive.Drifting() ? 1 : 0, m_drive.DriftAngleDeg(),
				m_drive.Wheel(2).wheelspin, m_drive.Slip01(), m_drive.Position().x, m_drive.Position().y, glm::degrees(m_drive.Heading()), static_cast<int>(VehicleFX::ParticleCount()));
			std::fflush(stdout);
		}
	}
	// NOX_DRIVE_SCRIPT=gearbox: full-throttle run up Ventura Ave (gear changes), stop, hold brake at rest
	// (reverse must engage and back up), then throttle (stop and drive forward again). Logs telemetry.
	static const bool gearboxTest = [] { const char* v = std::getenv("NOX_DRIVE_SCRIPT"); return v && std::string(v) == "gearbox"; }();
	if (gearboxTest && m_playing) {
		m_scriptTime += m_hudDt;
		m_phaseTime += m_hudDt;
		in = DriveMode::Input();
		now = Buttons();
		const glm::vec2 p = m_drive.Position();
		const float kmh = m_drive.SpeedKmh();
		const float head = m_drive.Heading();
		auto wrapA = [](float x) { while (x > kPi) x -= 2.0f * kPi; while (x < -kPi) x += 2.0f * kPi; return x; };
		const float keep = std::clamp(-wrapA(std::clamp((-3.8f - p.x) * 0.12f, -0.3f, 0.3f) - head) * 2.6f / (1.0f + kmh / 140.0f), -1.0f, 1.0f);
		auto next = [&](int phase) { m_scriptPhase = phase; m_phaseTime = 0.0f; };
		switch (m_scriptPhase) {
		case 0: if (m_phaseTime > 0.5f) next(1); break;
		case 1: in.throttle = 1.0f; in.steer = keep; if (p.y > 250.0f || m_phaseTime > 30.0f) next(2); break;   // full throttle
		case 2: in.brake = 1.0f; in.steer = keep; if (kmh < 0.5f && m_phaseTime > 0.5f) next(3); break;          // stop
		case 3: in.brake = 1.0f; if (m_phaseTime > 4.0f) next(4); break;                                        // hold S at rest: reverse
		case 4: in.throttle = 1.0f; if (m_phaseTime > 4.0f) next(5); break;                                     // W: stop, drive forward
		default: in.brake = 1.0f; break;
		}
		static float logTimer = 0.0f;
		logTimer += m_hudDt;
		if (logTimer >= 0.25f) {
			logTimer = 0.0f;
			std::printf("[Gearbox] ph=%d t=%5.2f thr=%.1f brk=%.1f v=%6.1f km/h vx=%6.2f gear=%d rpm=%5.0f y=%.1f\n", m_scriptPhase, m_scriptTime,
				in.throttle, in.brake, kmh, m_drive.SpeedMs(), m_drive.Gear(), m_drive.Rpm(), p.y);
			std::fflush(stdout);
		}
	}
	static const bool autopilot = [] { const char* v = std::getenv("NOX_DRIVE_AUTOPILOT"); return v && v[0] == '1'; }();
	if (autopilot && m_playing) {
		now = Buttons();
		in = DriveMode::Input();
		m_autoTimer += m_hudDt;
		const glm::vec2 pos = m_drive.Position();
		glm::vec2 target = pos;
		if (m_route.size() >= 2) {
			// Look-ahead point along the route polyline.
			float want = std::clamp(m_drive.SpeedMs() * 0.9f, 10.0f, 30.0f);
			target = m_route.back();
			for (size_t i = 1; i < m_route.size(); ++i) {
				const float seg = glm::length(m_route[i] - m_route[i - 1]);
				if (want <= seg) { target = m_route[i - 1] + (m_route[i] - m_route[i - 1]) * (want / std::max(seg, 1e-3f)); break; }
				want -= seg;
			}
		}
		const glm::vec2 d = target - pos;
		float diff = std::atan2(d.x, d.y) - m_drive.Heading();
		while (diff > kPi) diff -= 2.0f * kPi;
		while (diff < -kPi) diff += 2.0f * kPi;
		in.steer = std::clamp(-diff * 2.5f, -1.0f, 1.0f); // heading grows toward +X = the car's left
		const float kmh = m_drive.SpeedKmh();
		// Traversal diagnostics: report and recover when stuck or after falling off a level.
		if (m_phase == Phase::Racing) {
			m_apStuck = kmh < 3.0f ? m_apStuck + m_hudDt : 0.0f;
			const float y = m_drive.Position3().y;
			m_apFallTimer += m_hudDt;
			if (m_apFallTimer > 1.0f) {
				if (m_apLastY - y > 4.0f) {
					std::printf("[Traversal] FELL from y=%.2f to y=%.2f at (%.1f,%.1f)\n", m_apLastY, y, pos.x, pos.y);
					++m_apIncidents;
				}
				m_apLastY = y;
				m_apFallTimer = 0.0f;
			}
			if (m_apStuck > 3.0f) {
				std::printf("[Traversal] STUCK at (%.1f,%.2f,%.1f) heading %.1f\n", pos.x, y, pos.y, glm::degrees(m_drive.Heading()));
				++m_apIncidents;
				m_apStuck = 0.0f;
				// Skip ~40 m ahead along the route (same level) so one run reports every problem spot.
				glm::vec2 target = pos;
				float want = 40.0f;
				for (size_t i = 1; i < m_route.size(); ++i) {
					const float seg = glm::length(m_route[i] - m_route[i - 1]);
					target = m_route[i];
					if (want <= seg) { target = m_route[i - 1] + (m_route[i] - m_route[i - 1]) * (want / std::max(seg, 1e-3f)); break; }
					want -= seg;
				}
				int edge = -1;
				float t = 0.0f;
				m_roads.ClosestOnRoads3D(glm::vec3(target.x, y, target.y), 12.0f, nullptr, &edge, &t);
				// Only skip to a spot that has ground (never past the end of a generated section).
				CollisionWorld::Hit ground;
				float heading = m_drive.Heading();
				const glm::vec3 lane = edge >= 0 ? m_roads.LanePosition(edge, t, heading, -1, heading) : glm::vec3(0.0f);
				if (edge >= 0 && (!m_drive.HasSurface() || m_collision.GroundBelow(lane + glm::vec3(0.0f, 2.5f, 0.0f), 6.0f, ground))) {
					m_drive.Teleport(lane, heading);
				}
				else {
					ResetCar();
				}
			}
		}
		static float apLog = 0.0f;
		apLog += m_hudDt;
		if (apLog > 2.0f) { apLog = 0.0f; std::printf("[Autopilot] pos=(%.1f,%.1f) y=%.2f air=%d v=%.0f near=%d phase=%d route=%d\n", pos.x, pos.y, m_drive.Position3().y, m_drive.Airborne() ? 1 : 0, kmh, m_nearEvent, static_cast<int>(m_phase), static_cast<int>(m_route.size())); std::fflush(stdout); }
		in.throttle = std::abs(diff) < 0.35f ? 1.0f : 0.35f;
		if (std::abs(diff) > 0.5f && kmh > 70.0f) { in.throttle = 0.0f; in.brake = 1.0f; }
		in.nitrous = std::abs(diff) < 0.1f && kmh > 60.0f;
		// Approach the free-roam event marker slowly enough to trigger the start prompt.
		if (m_phase == Phase::FreeRoam && m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size())) {
			const float dist = glm::length(m_events[m_selectedEvent].start - pos);
			const float limit = 12.0f + dist * 1.2f;
			if (kmh > limit) { in.throttle = 0.0f; in.brake = 0.8f; in.nitrous = false; }
		}
		// Brake before sharp route corners in races.
		if (m_phase == Phase::Racing && std::abs(diff) > 0.25f && kmh > 75.0f) { in.throttle = 0.0f; in.brake = 0.7f; }
		if (m_route.size() >= 2) {
			// Distance to the first real corner (> ~25 deg turn) along the route; dense polylines
			// (expressway curves sampled every few metres) have no single-vertex corners.
			float toCorner = 1e9f, along = glm::length(m_route[1] - pos);
			for (size_t i = 1; i + 1 < m_route.size() && along < 200.0f; ++i) {
				const glm::vec2 d0 = m_route[i] - m_route[i - 1], d1 = m_route[i + 1] - m_route[i];
				if (glm::length(d0) > 1e-3f && glm::length(d1) > 1e-3f && glm::dot(glm::normalize(d0), glm::normalize(d1)) < 0.9f) { toCorner = along; break; }
				along += glm::length(d1);
			}
			// The turn after the current gate counts as a corner at the route end.
			if (m_phase == Phase::Racing && m_activeEvent >= 0) {
				const RaceEvent& ev = m_events[m_activeEvent];
				if (m_nextCheckpoint + 1 < static_cast<int>(ev.checkpoints.size())) {
					const glm::vec2 gate = ev.checkpoints[m_nextCheckpoint].pos, after = ev.checkpoints[m_nextCheckpoint + 1].pos;
					const glm::vec2 in2 = m_route.back() - m_route[m_route.size() - 2];
					const glm::vec2 out2 = after - gate;
					if (glm::length(in2) > 1e-3f && glm::length(out2) > 1e-3f &&
						glm::dot(glm::normalize(in2), glm::normalize(out2)) < 0.85f) {
						float toEnd = 0.0f;
						for (size_t i = 1; i < m_route.size(); ++i) toEnd += glm::length(m_route[i] - m_route[i - 1]);
						toCorner = std::min(toCorner, toEnd);
					}
				}
			}
			if (toCorner < 45.0f && kmh > 30.0f + toCorner * 1.1f) { in.throttle = 0.0f; in.brake = 1.0f; in.nitrous = false; }
			// Gentle curves and diverges: limit speed by the route's total turn over the next ~80 m.
			float turn = 0.0f, dist = 0.0f;
			for (size_t i = 1; i + 1 < m_route.size() && dist < 80.0f; ++i) {
				const glm::vec2 d0 = m_route[i] - m_route[i - 1], d1 = m_route[i + 1] - m_route[i];
				if (glm::length(d0) < 1e-3f || glm::length(d1) < 1e-3f) continue;
				turn += std::acos(std::clamp(glm::dot(glm::normalize(d0), glm::normalize(d1)), -1.0f, 1.0f));
				dist += glm::length(d1);
			}
			// NOX_AUTOPILOT_MAXKMH: speed cap for coverage runs (takes the routed branch at diverges).
			static const float capKmh = [] { const char* v = std::getenv("NOX_AUTOPILOT_MAXKMH"); return v ? static_cast<float>(std::atof(v)) : 150.0f; }();
			const float vmax = std::clamp(150.0f - glm::degrees(turn) * 2.2f, 45.0f, capKmh);
			if (kmh > vmax + 5.0f) { in.throttle = 0.0f; in.brake = 0.6f; in.nitrous = false; }
			else if (kmh > vmax) { in.throttle = 0.0f; in.nitrous = false; }
		}
		if (m_phase == Phase::FreeRoam && m_nearEvent >= 0 && m_autoRuns < 2) {
			in.throttle = 0.0f; in.brake = kmh > 3.0f ? 1.0f : 0.0f; // holding the brake at rest would engage reverse
			now.start = kmh < 5.0f && std::fmod(m_autoTimer, 0.5f) < 0.25f;
			if (now.start && !m_prevButtons.start && m_autoRuns == 0) ++m_autoRuns;
		}
		// Results: the scripted driver reads the results screen for 3 s before choosing (captures).
		static float resultsFor = 0.0f;
		resultsFor = (m_phase == Phase::Finished && !m_menuTitle.empty()) ? resultsFor + m_hudDt : 0.0f;
		if (m_phase == Phase::Finished && !m_menuTitle.empty() && resultsFor > 3.0f) {
			const bool tick = std::fmod(m_autoTimer, 1.0f) < 0.5f;
			if (m_autoRuns >= 2 && m_menuSelected == 0) now.down = tick;
			else {
				now.confirm = tick;
				if (now.confirm && !m_prevButtons.confirm && m_menuSelected == 0) ++m_autoRuns;
			}
		}
	}
	pressed.confirm = now.confirm && !m_prevButtons.confirm;
	pressed.start = now.start && !m_prevButtons.start;
	pressed.back = now.back && !m_prevButtons.back;
	pressed.pause = now.pause && !m_prevButtons.pause;
	pressed.reset = now.reset && !m_prevButtons.reset;
	pressed.camera = now.camera && !m_prevButtons.camera;
	pressed.map = now.map && !m_prevButtons.map;
	pressed.recenter = now.recenter && !m_prevButtons.recenter;
	pressed.up = now.up && !m_prevButtons.up;
	pressed.down = now.down && !m_prevButtons.down;
	m_prevButtons = now;
}

void GameSession::ShowMessage(const std::string& text, float seconds)
{
	m_message = text;
	m_messageTimer = seconds;
}

void GameSession::StartEvent(int index)
{
	if (index < 0 || index >= static_cast<int>(m_events.size()) || m_events[index].checkpoints.empty()) return;
	const RaceEvent& e = m_events[index];
	m_activeEvent = index;
	m_phase = Phase::Countdown;
	m_countdown = std::max(e.countdown, 1.0f);
	m_lastCountdownBeep = -1;
	m_raceTime = 0.0f;
	m_nextCheckpoint = 0;
	m_splits.clear();
	m_newRecord = false;
	m_wrongWayTimer = 0.0f;
	m_menuTitle.clear();
	m_paused = false;
	if (e.hasStartY) m_drive.Teleport(glm::vec3(e.start.x, e.startY, e.start.y), glm::radians(e.startHeadingDeg));
	else m_drive.Teleport(e.start, glm::radians(e.startHeadingDeg));
	DriveAudio::Get().Play(UiSound::Select);
	m_apLastY = m_drive.Position3().y;
	m_apFallTimer = 0.0f;
	m_apStuck = 0.0f;
	std::cout << "[Race] Event '" << e.name << "' countdown" << std::endl;
}

void GameSession::QuitEvent()
{
	m_phase = Phase::FreeRoam;
	m_activeEvent = -1;
	m_menuTitle.clear();
	m_paused = false;
	m_route.clear();
}

void GameSession::ResetCar()
{
	if (m_phase == Phase::Racing && m_activeEvent >= 0) {
		// Back onto the road at the last cleared gate, facing the next one.
		const RaceEvent& e = m_events[m_activeEvent];
		const glm::vec2 from = m_nextCheckpoint == 0 ? e.start : e.checkpoints[m_nextCheckpoint - 1].pos;
		const float heading = m_nextCheckpoint == 0 ? glm::radians(e.startHeadingDeg) : GateHeading(e, m_nextCheckpoint - 1);
		const bool hasY = m_nextCheckpoint == 0 ? e.hasStartY : e.checkpoints[m_nextCheckpoint - 1].hasY;
		const float y = m_nextCheckpoint == 0 ? e.startY : e.checkpoints[m_nextCheckpoint - 1].y;
		if (hasY) m_drive.Teleport(glm::vec3(from.x, y, from.y), heading);
		else m_drive.Teleport(from, heading);
	}
	else {
		m_drive.RecoverToRoad();
	}
}

void GameSession::UpdateMenus(const Buttons& b, MenuAction& action)
{
	if (m_menuTitle.empty() || m_menuItems.empty()) return;
	if (b.up) { m_menuSelected = (m_menuSelected + static_cast<int>(m_menuItems.size()) - 1) % static_cast<int>(m_menuItems.size()); DriveAudio::Get().Play(UiSound::Select); }
	if (b.down) { m_menuSelected = (m_menuSelected + 1) % static_cast<int>(m_menuItems.size()); DriveAudio::Get().Play(UiSound::Select); }
	if (m_menuTitle == "PAUSED" && b.back) { SetPaused(false); return; }
	if (!b.confirm) return;
	ActivateMenuItem(m_menuSelected, action);
}

void GameSession::ActivateMenuItem(int index, MenuAction& action)
{
	if (index < 0 || index >= static_cast<int>(m_menuItems.size())) return;
	const std::string item = m_menuItems[index];
	DriveAudio::Get().Play(UiSound::Select);
	if (item == "Resume") SetPaused(false);
	else if (item == "Restart Event" || item == "Retry") { const int e = m_activeEvent; m_paused = false; StartEvent(e); }
	else if (item == "Quit Event" || item == "Free Roam") { QuitEvent(); }
	else if (item == "Reset Car") { SetPaused(false); ResetCar(); }
	else if (item == "Exit Game") action = MenuAction::ExitApplication;
}

glm::vec2 GameSession::RouteTarget() const
{
	if (m_hasWaypoint && m_phase == Phase::FreeRoam) return m_waypoint;
	if (m_activeEvent >= 0 && (m_phase == Phase::Racing || m_phase == Phase::Countdown)) {
		const RaceEvent& e = m_events[m_activeEvent];
		return e.checkpoints[std::min(m_nextCheckpoint, static_cast<int>(e.checkpoints.size()) - 1)].pos;
	}
	if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size())) return m_events[m_selectedEvent].start;
	return m_drive.Position();
}

glm::vec3 GameSession::RouteTarget3() const
{
	if (m_hasWaypoint && m_phase == Phase::FreeRoam) return glm::vec3(m_waypoint.x, m_waypointHasY ? m_waypointY : m_drive.Position3().y, m_waypoint.y);
	const glm::vec2 t = RouteTarget();
	float y = m_drive.Position3().y;
	if (m_activeEvent >= 0 && (m_phase == Phase::Racing || m_phase == Phase::Countdown)) {
		const RaceEvent& e = m_events[m_activeEvent];
		const Checkpoint& c = e.checkpoints[std::min(m_nextCheckpoint, static_cast<int>(e.checkpoints.size()) - 1)];
		if (c.hasY) y = c.y;
	}
	else if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size()) && m_events[m_selectedEvent].hasStartY) {
		y = m_events[m_selectedEvent].startY;
	}
	return glm::vec3(t.x, y, t.y);
}

bool GameSession::AtHeight(bool hasY, float y) const
{
	return !hasY || !m_drive.HasSurface() || std::abs(m_drive.Position3().y - y) <= m_heightTolerance;
}

void GameSession::UpdateRace(float dt)
{
	const glm::vec2 pos = m_drive.Position();
	const glm::vec2 prev = m_drive.PreviousPosition();
	m_messageTimer = std::max(0.0f, m_messageTimer - dt);

	if (m_phase == Phase::FreeRoam) {
		// Waypoint reached: guidance returns to the selected event.
		if (m_hasWaypoint && glm::length(pos - m_waypoint) < 30.0f) {
			m_hasWaypoint = false;
			ShowMessage("WAYPOINT REACHED", 2.5f);
		}
		// Places: arrival (slow, on the place's level) shows the place and rests the car.
		int at = -1;
		for (size_t i = 0; i < m_places.size(); ++i) {
			const Place& pl = m_places[i];
			if (glm::length(pos - pl.pos) < pl.radius && AtHeight(pl.hasY, pl.y)) { at = static_cast<int>(i); break; }
		}
		if (at != m_atPlace) {
			m_atPlace = at;
			if (at >= 0 && m_drive.SpeedKmh() < 80.0f) {
				const Place& pl = m_places[at];
				int nearby = 0;
				for (const RaceEvent& e : m_events) nearby += glm::length(e.start - pl.pos) < 1500.0f ? 1 : 0;
				ShowMessage(pl.name + " " + pl.type + " - REST / MEET / PHOTO" + (nearby ? " - " + std::to_string(nearby) + " EVENT(S) NEARBY" : std::string()), 3.5f);
				m_drive.Refuel();
				std::cout << "[Place] Arrived at " << pl.name << " (" << pl.id << ")" << std::endl;
			}
		}
		m_nearEvent = -1;
		for (size_t i = 0; i < m_events.size(); ++i) {
			if (glm::length(pos - m_events[i].start) < m_events[i].triggerRadius && m_drive.SpeedKmh() < 60.0f &&
				AtHeight(m_events[i].hasStartY, m_events[i].startY)) {
				m_nearEvent = static_cast<int>(i);
				m_selectedEvent = static_cast<int>(i);
				break;
			}
		}
	}
	else if (m_phase == Phase::Countdown) {
		m_countdown -= dt;
		const int beep = static_cast<int>(std::ceil(m_countdown));
		if (beep != m_lastCountdownBeep && beep > 0) {
			DriveAudio::Get().Play(UiSound::Countdown);
			m_lastCountdownBeep = beep;
		}
		if (m_countdown <= 0.0f) {
			m_phase = Phase::Racing;
			m_raceTime = 0.0f;
			DriveAudio::Get().Play(UiSound::Go);
			ShowMessage("GO", 0.8f);
		}
	}
	else if (m_phase == Phase::Racing) {
		m_raceTime += dt;
		const RaceEvent& e = m_events[m_activeEvent];
		const int count = static_cast<int>(e.checkpoints.size());
		// Gate tests on the swept segment of this frame (cannot be skipped at speed).
		for (int i = m_nextCheckpoint; i < count; ++i) {
			const Checkpoint& c = e.checkpoints[i];
			const float h = GateHeading(e, i);
			const glm::vec2 dir(std::sin(h), std::cos(h));
			const glm::vec2 side(dir.y, -dir.x);
			const glm::vec2 g0 = c.pos - side * c.width * 0.5f, g1 = c.pos + side * c.width * 0.5f;
			if (!SegmentsIntersect(prev, pos, g0, g1)) continue;
			if (!AtHeight(c.hasY, c.y)) continue; // a gate on another deck
			if (glm::dot(pos - prev, dir) <= 0.0f) continue; // crossing backwards does not count
			if (i == m_nextCheckpoint) {
				m_splits.push_back(m_raceTime);
				++m_nextCheckpoint;
				std::cout << "[Race] Checkpoint " << m_nextCheckpoint << "/" << count << " at " << m_raceTime << " s" << std::endl;
				if (m_nextCheckpoint >= count) {
					m_phase = Phase::Finished;
					m_exitAfterFinish = std::getenv("NOX_EXIT_ON_FINISH") != nullptr;
					m_lastTime = m_raceTime;
					const float best = BestTime(e.id);
					m_newRecord = best < 0.0f || m_raceTime < best;
					if (m_newRecord) {
						m_bestTimes[e.id] = { {"time", m_raceTime}, {"splits", m_splits} };
						SaveBestTimes();
						DriveAudio::Get().Play(UiSound::Record);
					}
					DriveAudio::Get().Play(UiSound::Finish);
					std::cout << "[Race] Finished in " << m_raceTime << " s" << (m_newRecord ? " (new record)" : "") << std::endl;
					m_menuTitle = "RESULTS";
					m_menuItems = { "Retry", "Free Roam" };
					m_menuSelected = 0;
				}
				else {
					DriveAudio::Get().Play(UiSound::Checkpoint);
				}
			}
			else {
				ShowMessage("MISSED CHECKPOINT - FOLLOW THE ROUTE", 2.5f);
				std::cout << "[Race] Missed checkpoint " << (m_nextCheckpoint + 1) << " (crossed " << (i + 1) << ")" << std::endl;
				DriveAudio::Get().Play(UiSound::Missed);
			}
			break;
		}
		// Wrong way: moving against the route toward the next gate for a while.
		if (m_phase == Phase::Racing && m_route.size() >= 2 && m_drive.SpeedKmh() > 20.0f) {
			const glm::vec2 routeDir = glm::normalize(m_route[1] - m_route[0] + glm::vec2(1e-4f));
			const glm::vec2 v = pos - prev;
			if (glm::length(v) > 1e-4f && glm::dot(glm::normalize(v), routeDir) < -0.6f) m_wrongWayTimer += dt;
			else m_wrongWayTimer = std::max(0.0f, m_wrongWayTimer - dt * 2.0f);
			if (m_wrongWayTimer > 2.0f && m_messageTimer <= 0.0f) ShowMessage("WRONG WAY", 1.5f);
		}
	}

	FrameProfiler::Mark("g_racelogic");
	// Route guidance, refreshed a few times per second.
	m_routeTimer -= dt;
	if (m_routeTimer <= 0.0f) {
		m_routeTimer = 0.2f;
		// Hysteresis: while the car is still on the current route line (within 4 m) toward the same
		// target, trim it instead of re-projecting onto the nearest edge. In merge/diverge zones the
		// nearest edge can be the other branch, which would flip the guidance to the wrong road.
		bool kept = false;
		const glm::vec2 target = RouteTarget();
		if (m_phase != Phase::Finished && m_route.size() >= 2 && glm::length(m_route.back() - target) < 1.0f && m_routeKeep < 10) {
			float best = 1e30f;
			size_t seg = 0;
			glm::vec2 closest = m_route[0];
			for (size_t i = 0; i + 1 < m_route.size(); ++i) {
				const glm::vec2 a = m_route[i], b = m_route[i + 1];
				const glm::vec2 ab = b - a;
				const float t = glm::dot(ab, ab) > 1e-6f ? std::clamp(glm::dot(pos - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
				const glm::vec2 c = a + ab * t;
				const float d = glm::length(pos - c);
				if (d < best) { best = d; seg = i; closest = c; }
			}
			if (best < 4.0f) {
				std::vector<glm::vec2> trimmed;
				trimmed.push_back(closest);
				trimmed.insert(trimmed.end(), m_route.begin() + static_cast<std::ptrdiff_t>(seg) + 1, m_route.end());
				if (trimmed.size() >= 2) {
					m_route = std::move(trimmed);
					kept = true;
					++m_routeKeep;   // a full recompute at least every ~2 s
				}
			}
		}
		if (!kept) {
			m_routeKeep = 0;
			m_route = m_phase == Phase::Finished ? std::vector<glm::vec2>()
				: (m_drive.HasSurface() ? m_roads.FindRoute(m_drive.Position3(), RouteTarget3(), m_heightTolerance) : m_roads.FindRoute(pos, RouteTarget()));
		}
	}
}

void GameSession::InitializeResources()
{
	RacingHUD::Get().LoadFonts();
	RacingHUD::Get().LoadIcons(); // UI icon atlas (cached; used by the HUD and the editor toolbar)
	DriveAudio::Get().Init();
}

bool GameSession::WantsAutoPlay() const
{
	const char* drive = std::getenv("NOX_DRIVE");
	return drive && std::string(drive) == "1";
}

void GameSession::Reconfigure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& sceneGraph)
{
	const int selected = m_selectedEvent;
	Configure(game, sceneRoot, sceneGraph);
	m_selectedEvent = selected;
}

IGameModule::UpdateResult GameSession::Update(float dt, bool inputEnabled, float aspect)
{
	// Window focus lost while driving (standalone): pause. Focus regain never resumes by itself.
	if (ui::Pointer::Get().FocusLost()) {
		m_padBackDown = m_padBackHoldDone = false;
		m_padBackHold = 0.0f;
		const bool driving = m_playing && !m_paused && m_menuTitle.empty() && !m_worldMap.IsOpen();
		if (m_standalone && driving && m_pauseOnFocusLoss) {
			SetPaused(true);
			std::cout << "[GameSession] Window focus lost: paused" << std::endl;
		}
	}
	const MenuAction action = Tick(dt, inputEnabled, aspect);
	UpdateCursorPolicy(dt);
	return action == MenuAction::ExitApplication ? UpdateResult::ExitApplication : UpdateResult::Continue;
}

bool GameSession::WantsPointer() const
{
	return !m_playing || m_paused || !m_menuTitle.empty() || m_worldMap.IsOpen();
}

void GameSession::UpdateCursorPolicy(float dt)
{
	const ui::Pointer& ptr = ui::Pointer::Get();
	// At the START EVENT prompt, moving the mouse shows the cursor for a few seconds (clickable).
	if (m_playing && m_phase == Phase::FreeRoam && m_nearEvent >= 0 && m_menuTitle.empty() && !m_worldMap.IsOpen()) {
		m_cursorReveal = ptr.Moved() ? 3.0f : std::max(0.0f, m_cursorReveal - dt);
	}
	else {
		m_cursorReveal = 0.0f;
	}
	if (!m_standalone) return;   // the editor owns its cursor
	// DriveMode reads keys/pad only, so driving hides the cursor but never captures the mouse.
	ui::Pointer::Get().SetCursorMode(WantsPointer() || m_cursorReveal > 0.0f ? ui::CursorMode::Visible : ui::CursorMode::Hidden);
}

void GameSession::UpdateMenuPointer(MenuAction& action, bool& startEvent)
{
	m_hud.menuHovered = m_hud.menuPressed = -1;
	m_hud.startHovered = m_hud.startPressed = false;
	if (!m_pointerOn) return;
	const ui::Pointer& ptr = ui::Pointer::Get();
	if (!m_menuTitle.empty()) {
		const int n = static_cast<int>(m_menuItems.size());
		int clicked = -1;
		for (int i = 0; i < n; ++i) {
			const ui::PointerId id = ui::HashId(HudRegion::kMenuItem, i);
			if (ptr.IsHovered(id)) {
				m_hud.menuHovered = i;
				// Moving over an item selects it; keys/pad continue from there.
				if ((ptr.Moved() || ptr.Pressed(ui::PointerButton::Left)) && m_menuSelected != i) {
					m_menuSelected = i;
					DriveAudio::Get().Play(UiSound::Select);
				}
				if (ptr.IsActive(id) && ptr.Down(ui::PointerButton::Left)) m_hud.menuPressed = i;
			}
			if (ptr.IsClicked(id)) clicked = i;
		}
		// Wheel anywhere over the menu screen steps the selection (the lists never overflow).
		const float wheel = ptr.WheelFor(ui::HashId(HudRegion::kModal));
		if (wheel != 0.0f && n > 0) {
			m_menuSelected = (m_menuSelected + (wheel > 0.0f ? n - 1 : 1)) % n;
			DriveAudio::Get().Play(UiSound::Select);
		}
		if (clicked >= 0) {
			m_menuSelected = clicked;
			ActivateMenuItem(clicked, action);
		}
		return;
	}
	if (m_phase == Phase::FreeRoam && m_nearEvent >= 0) {
		const ui::PointerId id = ui::HashId(HudRegion::kStart);
		m_hud.startHovered = ptr.IsHovered(id);
		m_hud.startPressed = m_hud.startHovered && ptr.IsActive(id) && ptr.Down(ui::PointerButton::Left);
		startEvent = ptr.IsClicked(id);
	}
}

namespace {
	// Pointer test helpers: "cmd:args" split and "x,y" / "a,b,c,d,e" float lists.
	void SplitStep(const std::string& step, std::string& cmd, std::string& arg) {
		const size_t c = step.find(':');
		cmd = step.substr(0, c);
		arg = c == std::string::npos ? std::string() : step.substr(c + 1);
	}
	std::vector<float> Floats(const std::string& s) {
		std::vector<float> out;
		size_t i = 0;
		while (i < s.size()) {
			const size_t j = s.find(',', i);
			out.push_back(static_cast<float>(std::atof(s.substr(i, j == std::string::npos ? std::string::npos : j - i).c_str())));
			if (j == std::string::npos) break;
			i = j + 1;
		}
		return out;
	}
	ui::PointerButton ButtonArg(const std::string& a) {
		return a == "r" || a == "right" ? ui::PointerButton::Right : (a == "m" || a == "middle" ? ui::PointerButton::Middle : ui::PointerButton::Left);
	}
}

// NOX_TEST_POINTER_SCRIPT="step;step;..." (or "@file"). Steps run in order, one Update apart unless
// a wait says otherwise; injected pointer events reach the game on the next frame (as real input):
//   wait:N            next step N frames later
//   until:COND        wait for results|paused|map|near|countdown|racing|freeroam (FAIL after 20000)
//   pause | map | closemap     open the pause menu / world map, close the map
//   move:X,Y          pointer to normalized coordinates of the game view
//   move:@NAME[#I]    pointer to the centre of the region ui::HashId(NAME, I) of the last draw
//   down[:l|r|m] up[:...] click[:...]    buttons (click = down, next frame up)
//   wheel:N           wheel notches (+ = zoom in / previous item)
//   drag:X0,Y0,X1,Y1[,FRAMES]   press at 0, move to 1 over FRAMES, release
//   mark              remember the map view and the world point under the pointer
//   expect:WHAT       [PointerTest] PASS/FAIL: menu_hover=I, paused=0|1, phase=NAME, map_open=0|1,
//                     map_selected=I, map_drag=DX,DY (normalized), map_still, zoom_anchor, waypoint=0|1,
//                     cursor=visible|hidden
//   resize:W,H        resize the window      log:TEXT      exit
bool GameSession::RunPointerScript()
{
	static const std::string script = [] {
		const char* v = std::getenv("NOX_TEST_POINTER_SCRIPT");
		std::string s = v ? v : "";
		if (!s.empty() && s[0] == '@') {
			std::ifstream f(s.substr(1));
			s.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		return s;
	}();
	if (script.empty()) return false;
	PointerTest& T = m_ptest;
	ui::Pointer& ptr = ui::Pointer::Get();
	static bool parsed = false;
	if (!parsed) {
		parsed = true;
		std::string tok;
		auto flush = [&] {
			size_t a = tok.find_first_not_of(" \t\r\n"), b = tok.find_last_not_of(" \t\r\n");
			const std::string step = a == std::string::npos ? std::string() : tok.substr(a, b - a + 1);
			tok.clear();
			if (step.empty() || step[0] == '#') return;
			std::string cmd, arg;
			SplitStep(step, cmd, arg);
			if (cmd == "click") {
				T.steps.push_back("down:" + arg);
				T.steps.push_back("wait:1");
				T.steps.push_back("up:" + arg);
			}
			else if (cmd == "drag") {
				const std::vector<float> f = Floats(arg);
				if (f.size() < 4) { T.steps.push_back("log:bad drag " + arg); return; }
				const int frames = f.size() >= 5 ? std::max(1, static_cast<int>(f[4])) : 8;
				char buf[96];
				std::snprintf(buf, sizeof(buf), "move:%g,%g", f[0], f[1]);
				T.steps.push_back(buf);
				T.steps.push_back("wait:1");
				T.steps.push_back("down");
				T.steps.push_back("wait:1");
				for (int k = 1; k <= frames; ++k) {
					const float t = static_cast<float>(k) / frames;
					std::snprintf(buf, sizeof(buf), "move:%g,%g", f[0] + (f[2] - f[0]) * t, f[1] + (f[3] - f[1]) * t);
					T.steps.push_back(buf);
					T.steps.push_back("wait:1");
				}
				T.steps.push_back("up");
			}
			else T.steps.push_back(step);
		};
		for (char ch : script) {
			if (ch == ';' || ch == '\n') flush(); else tok += ch;
		}
		flush();
		ptr.SetInjectionOnly(true);   // the real mouse cannot disturb the test
		std::cout << "[PointerTest] Script: " << T.steps.size() << " steps" << std::endl;
	}
	if (T.wait > 0 && --T.wait > 0) return false;
	while (T.next < T.steps.size()) {
		const std::string step = T.steps[T.next];
		std::string cmd, arg;
		SplitStep(step, cmd, arg);
		if (cmd == "wait") {
			T.wait = std::max(1, std::atoi(arg.c_str()));
			++T.next;
			return false;
		}
		if (cmd == "until") {
			bool ok = false;
			if (arg == "results") ok = m_phase == Phase::Finished && m_menuTitle == "RESULTS";
			else if (arg == "paused") ok = m_paused;
			else if (arg == "map") ok = m_worldMap.IsOpen();
			else if (arg == "near") ok = m_phase == Phase::FreeRoam && m_nearEvent >= 0;
			else if (arg == "countdown") ok = m_phase == Phase::Countdown;
			else if (arg == "racing") ok = m_phase == Phase::Racing;
			else if (arg == "freeroam") ok = m_phase == Phase::FreeRoam;
			if (!ok) {
				if (++T.untilFrames <= 20000) return false;
				std::cout << "[PointerTest] FAIL until:" << arg << " (timeout)" << std::endl;
				++T.fail;
			}
			T.untilFrames = 0;
			std::cout << "[PointerTest] f=" << m_updateFrame << " reached " << arg << std::endl;
			++T.next;
			continue;
		}
		++T.next;
		std::cout << "[PointerTest] f=" << m_updateFrame << " " << step << std::endl;
		if (cmd == "pause") { if (!m_paused) SetPaused(true); }
		else if (cmd == "map") { if (!m_worldMap.IsOpen()) OpenWorldMap(); }
		else if (cmd == "closemap") { CloseWorldMap(); }
		else if (cmd == "move") {
			glm::vec2 p(0.0f);
			if (!arg.empty() && arg[0] == '@') {
				const size_t hash = arg.find('#');
				const std::string name = arg.substr(1, hash == std::string::npos ? std::string::npos : hash - 1);
				const int index = hash == std::string::npos ? 0 : std::atoi(arg.c_str() + hash + 1);
				if (!ptr.RegionCentre(ui::HashId(name.c_str(), index), p)) {
					std::cout << "[PointerTest] FAIL move: region " << arg.substr(1) << " not drawn" << std::endl;
					++T.fail;
					continue;
				}
			}
			else {
				const std::vector<float> f = Floats(arg);
				if (f.size() < 2) continue;
				p = m_overlayOrigin + glm::vec2(f[0], f[1]) * m_overlaySize;
			}
			ptr.InjectMove(p);
		}
		else if (cmd == "down") ptr.InjectButton(ButtonArg(arg), true);
		else if (cmd == "up") ptr.InjectButton(ButtonArg(arg), false);
		else if (cmd == "wheel") ptr.InjectWheel(static_cast<float>(std::atof(arg.c_str())));
		else if (cmd == "mark") {
			T.markCentre = m_worldMap.ViewCentre();
			T.markPpm = m_worldMap.PixelsPerMetre();
			T.markPos = ptr.Position();
			T.markWorld = m_worldMap.WorldAt(T.markPos);
		}
		else if (cmd == "expect") {
			if (PointerTestExpect(arg)) ++T.pass; else ++T.fail;
		}
		else if (cmd == "resize") {
			const std::vector<float> f = Floats(arg);
			if (f.size() >= 2 && ptr.Window()) SDL_SetWindowSize(ptr.Window(), static_cast<int>(f[0]), static_cast<int>(f[1]));
		}
		else if (cmd == "log") { /* printed above */ }
		else if (cmd == "exit") {
			std::cout << "[PointerTest] DONE pass=" << T.pass << " fail=" << T.fail << std::endl;
			return true;
		}
		else {
			std::cout << "[PointerTest] FAIL unknown step '" << step << "'" << std::endl;
			++T.fail;
		}
	}
	return false;
}

bool GameSession::PointerTestExpect(const std::string& what)
{
	const ui::Pointer& ptr = ui::Pointer::Get();
	const size_t eq = what.find('=');
	const std::string key = what.substr(0, eq);
	const std::string val = eq == std::string::npos ? std::string() : what.substr(eq + 1);
	const int iv = std::atoi(val.c_str());
	bool ok = false;
	char detail[256] = "";
	if (key == "menu_hover") {
		ok = m_hud.menuHovered == iv && m_menuSelected == iv && ptr.IsHovered(ui::HashId(HudRegion::kMenuItem, iv));
		std::snprintf(detail, sizeof(detail), "hovered=%d selected=%d item='%s'", m_hud.menuHovered, m_menuSelected,
			m_menuSelected >= 0 && m_menuSelected < static_cast<int>(m_menuItems.size()) ? m_menuItems[m_menuSelected].c_str() : "");
	}
	else if (key == "paused") {
		ok = m_paused == (iv != 0);
		std::snprintf(detail, sizeof(detail), "paused=%d menu='%s'", m_paused ? 1 : 0, m_menuTitle.c_str());
	}
	else if (key == "phase") {
		const char* names[] = { "freeroam", "countdown", "racing", "finished" };
		const char* now = names[static_cast<int>(m_phase)];
		ok = val == now;
		std::snprintf(detail, sizeof(detail), "phase=%s menu='%s'", now, m_menuTitle.c_str());
	}
	else if (key == "map_open") {
		ok = m_worldMap.IsOpen() == (iv != 0);
		std::snprintf(detail, sizeof(detail), "open=%d", m_worldMap.IsOpen() ? 1 : 0);
	}
	else if (key == "map_selected") {
		ok = m_mapClickEvent == iv && m_worldMap.SelectedEvent() == iv && m_worldMap.DisplayedEvent() == iv && m_previewIndex == iv &&
			m_mapPreview.size() >= 2;
		std::snprintf(detail, sizeof(detail), "clicked=%d selected=%d displayed=%d preview=%d (%d points)", m_mapClickEvent,
			m_worldMap.SelectedEvent(), m_worldMap.DisplayedEvent(), m_previewIndex, static_cast<int>(m_mapPreview.size()));
	}
	else if (key == "map_drag") {
		const std::vector<float> f = Floats(val);
		const glm::vec2 px = f.size() >= 2 ? glm::vec2(f[0], f[1]) * m_overlaySize : glm::vec2(0.0f);
		const glm::vec2 expect = m_ptest.markCentre - px / m_ptest.markPpm;
		const float errPx = glm::length(m_worldMap.ViewCentre() - expect) * m_worldMap.PixelsPerMetre();
		ok = errPx < 1.5f && glm::length(px) > 1.0f && std::abs(m_worldMap.PixelsPerMetre() - m_ptest.markPpm) < 1e-6f;
		std::snprintf(detail, sizeof(detail), "centre (%.1f,%.1f) -> (%.1f,%.1f), drag %.0fx%.0f px, error %.2f px",
			m_ptest.markCentre.x, m_ptest.markCentre.y, m_worldMap.ViewCentre().x, m_worldMap.ViewCentre().y, px.x, px.y, errPx);
	}
	else if (key == "map_still") {
		const float movePx = glm::length(m_worldMap.ViewCentre() - m_ptest.markCentre) * m_worldMap.PixelsPerMetre();
		ok = movePx < 0.01f && std::abs(m_worldMap.PixelsPerMetre() - m_ptest.markPpm) < 1e-6f;
		std::snprintf(detail, sizeof(detail), "view moved %.2f px", movePx);
	}
	else if (key == "zoom_anchor") {
		const float ratio = m_worldMap.PixelsPerMetre() / std::max(m_ptest.markPpm, 1e-9f);
		const glm::vec2 now = m_worldMap.WorldAt(m_ptest.markPos);
		const float errPx = glm::length(now - m_ptest.markWorld) * m_worldMap.PixelsPerMetre();
		ok = std::abs(ratio - 1.0f) > 0.05f && errPx < 1.0f && glm::length(ptr.Position() - m_ptest.markPos) < 0.5f;
		std::snprintf(detail, sizeof(detail), "zoom x%.3f, world under cursor (%.1f,%.1f) drift %.3f px", ratio, now.x, now.y, errPx);
	}
	else if (key == "waypoint") {
		float length = 0.0f;
		for (size_t i = 1; i < m_route.size(); ++i) length += glm::length(m_route[i] - m_route[i - 1]);
		ok = (iv != 0) == m_hasWaypoint && (!m_hasWaypoint || m_route.size() >= 2);
		std::snprintf(detail, sizeof(detail), "waypoint=%d at (%.1f,%.1f) y=%.1f, route %d points %.0f m", m_hasWaypoint ? 1 : 0,
			m_waypoint.x, m_waypoint.y, m_waypointY, static_cast<int>(m_route.size()), length);
	}
	else if (key == "cursor") {
		const ui::CursorMode m = ptr.RequestedCursorMode();
		const char* now = m == ui::CursorMode::Visible ? "visible" : (m == ui::CursorMode::Hidden ? "hidden" : "captured");
		ok = val == now;
		std::snprintf(detail, sizeof(detail), "cursor=%s", now);
	}
	else {
		std::snprintf(detail, sizeof(detail), "unknown expectation");
	}
	std::cout << "[PointerTest] " << (ok ? "PASS " : "FAIL ") << what << " (" << detail << ", window "
		<< static_cast<int>(m_overlaySize.x) << "x" << static_cast<int>(m_overlaySize.y) << ")" << std::endl;
	return ok;
}

GameSession::MenuAction GameSession::Tick(float dt, bool inputEnabled, float aspect)
{
	MenuAction action = MenuAction::None;
	m_aspect = aspect;
	m_hudDt = dt;
	if (!m_drive.IsAvailable()) return action;
	// NOX_TEST_PAUSE_AT_FRAME=<n>: opens the pause menu after n driving frames (capture/test hook).
	static const int pauseAtFrame = [] { const char* v = std::getenv("NOX_TEST_PAUSE_AT_FRAME"); return v ? std::atoi(v) : 0; }();
	static int playFrames = 0;
	if (pauseAtFrame > 0 && m_playing && !m_paused && playFrames >= 0 && ++playFrames == pauseAtFrame) SetPaused(true);
	if (m_exitAfterFinish) {
		std::cout << "[Traversal] Finished; incidents=" << m_apIncidents << std::endl;
		return MenuAction::ExitApplication;
	}
	if (!m_playing) {
		// Editing: the car rests at spawn and the camera frames the reference composition.
		DriveMode::Input none;
		m_drive.Update(dt, none, true, m_camera.get(), aspect);
		m_route = m_drive.HasSurface() ? m_roads.FindRoute(m_drive.Position3(), RouteTarget3(), m_heightTolerance)
			: m_roads.FindRoute(m_drive.Position(), RouteTarget());
		BuildHudState(dt);
		return action;
	}

	DriveMode::Input in;
	Buttons b;
	++m_updateFrame;
	ReadInput(inputEnabled, in, b);
	FrameProfiler::Mark("g_input");
	// Game UI takes the mouse only while the game view is focused and under the pointer, and (in
	// the standalone game) while the cursor is shown.
	m_pointerOn = inputEnabled && m_overlayHovered && (!m_standalone || WantsPointer() || m_cursorReveal > 0.0f);
	if (RunPointerScript()) return MenuAction::ExitApplication;
	// NOX_TEST_OPEN_MAP_AT_FRAME=<n>, NOX_TEST_MAP_ENTER_AT_FRAME=<n>, NOX_TEST_MAP_CLOSE_AT_FRAME=<n>:
	// scripted world-map open / ENTER (waypoint) / close for capture and validation runs.
	static const unsigned openMapAt = [] { const char* v = std::getenv("NOX_TEST_OPEN_MAP_AT_FRAME"); return v ? static_cast<unsigned>(std::atoi(v)) : 0u; }();
	static const unsigned enterMapAt = [] { const char* v = std::getenv("NOX_TEST_MAP_ENTER_AT_FRAME"); return v ? static_cast<unsigned>(std::atoi(v)) : 0u; }();
	static const unsigned closeMapAt = [] { const char* v = std::getenv("NOX_TEST_MAP_CLOSE_AT_FRAME"); return v ? static_cast<unsigned>(std::atoi(v)) : 0u; }();
	if (openMapAt && m_updateFrame == openMapAt) b.map = true;
	if (enterMapAt && m_updateFrame == enterMapAt && m_worldMap.IsOpen()) b.confirm = true;
	if (closeMapAt && m_updateFrame == closeMapAt && m_worldMap.IsOpen()) b.map = true;
	// World map: M / pad View opens it; while open it owns the input and freezes the car.
	const bool mapWasOpen = m_worldMap.IsOpen();
	if (!mapWasOpen && b.map) OpenWorldMap();
	else if (mapWasOpen) UpdateWorldMap(b, inputEnabled, dt);
	if (mapWasOpen || m_worldMap.IsOpen()) {
		DriveMode::Input none;
		m_drive.Update(0.0f, none, true, m_camera.get(), aspect);
		DriveAudioState silent;
		DriveAudio::Get().Update(silent);
		BuildHudState(dt);
		return action;
	}
	if (b.pause) {
		if (m_paused) SetPaused(false);
		else if (m_menuTitle.empty()) SetPaused(true);
	}
	const bool menuOpen = !m_menuTitle.empty();
	bool pointerStart = false;
	if (menuOpen) UpdateMenus(b, action);
	UpdateMenuPointer(action, pointerStart);
	if (!menuOpen) {
		if (b.camera) m_drive.CycleCamera();
		if (b.reset) ResetCar();
		if ((b.start || pointerStart) && m_phase == Phase::FreeRoam && m_nearEvent >= 0) StartEvent(m_nearEvent);
	}

	const bool frozen = m_paused || m_phase == Phase::Countdown || (!m_menuTitle.empty() && m_phase != Phase::Finished);
	if (m_paused) {
		DriveMode::Input none;
		m_drive.Update(0.0f, none, true, m_camera.get(), aspect);
	}
	else {
		DriveMode::Input drive = in;
		if (m_phase == Phase::Finished) { drive = DriveMode::Input(); drive.brake = 0.6f; }
		FrameProfiler::Mark("g_menus");
		m_drive.Update(dt, drive, frozen, m_camera.get(), aspect);
		FrameProfiler::Mark("g_drive");
		EmitTyreEffects(dt);
		FrameProfiler::Mark("g_tyrefx");
		UpdateRace(dt);
		FrameProfiler::Mark("g_race");
	}

	DriveAudioState audio;
	audio.active = true;
	audio.paused = m_paused;
	audio.rpm = m_drive.Rpm();
	audio.rpmMax = RacingHUD::Get().RpmMax();
	audio.throttle = m_drive.Throttle();
	audio.speed01 = m_drive.Speed01();
	audio.slip01 = m_drive.Slip01();
	audio.nitro = m_drive.NitroActive();
	audio.limiter = m_drive.LimiterHit();
	DriveAudio::Get().Update(audio);
	if (m_drive.ConsumeShift() && !m_paused) DriveAudio::Get().Play(UiSound::Shift);

	BuildHudState(dt);
	return action;
}

bool GameSession::Project(const glm::vec3& world, glm::vec2& ndc, float& distance) const
{
	if (!m_camera) return false;
	const glm::mat4 view = m_camera->GetViewMatrix();
	const glm::mat4 proj = glm::perspective(glm::radians(m_camera->GetCameraFov()), m_aspect, 0.1f, 2000.0f);
	const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
	distance = glm::length(world - m_camera->GetCameraPosition());
	if (clip.w <= 0.1f) return false;
	const glm::vec3 n = glm::vec3(clip) / clip.w;
	ndc = glm::vec2(n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f);
	return ndc.x > -0.2f && ndc.x < 1.2f && ndc.y > -0.2f && ndc.y < 1.2f;
}

void GameSession::BuildHudState(float dt)
{
	HudState& h = m_hud;
	h.speedKmh = m_drive.SpeedKmh();
	h.rpm = m_drive.Rpm();
	h.throttle = m_drive.Throttle();
	h.load = m_drive.Load();
	h.nitro = m_drive.Nitro();
	h.nitroActive = m_drive.NitroActive();
	h.nitroRecharging = m_drive.NitroRecharging();
	h.gear = m_drive.Gear();
	h.redline = m_drive.AtRedline();
	h.limiter = m_drive.LimiterHit();
	h.carPos = m_drive.Position();
	h.carY = m_drive.Position3().y;
	h.layered = m_drive.HasSurface();
	h.carHeading = m_drive.Heading();
	h.roads = &m_roads;
	h.route = m_route;
	h.markers.clear();

	const RaceEvent* shown = nullptr;
	if (m_activeEvent >= 0 && m_phase != Phase::FreeRoam) shown = &m_events[m_activeEvent];
	else if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size())) shown = &m_events[m_selectedEvent];

	if (m_phase == Phase::FreeRoam) {
		for (const RaceEvent& e : m_events) h.markers.push_back({ e.start, HudMapMarker::Kind::Event, e.type });
		for (const Place& p : m_places) h.markers.push_back({ p.pos, HudMapMarker::Kind::Place });
	}
	else if (m_activeEvent >= 0) {
		const RaceEvent& e = m_events[m_activeEvent];
		const int count = static_cast<int>(e.checkpoints.size());
		for (int i = m_nextCheckpoint; i < count; ++i) {
			HudMapMarker::Kind k = i == count - 1 ? HudMapMarker::Kind::Finish : (i == m_nextCheckpoint ? HudMapMarker::Kind::NextCheckpoint : HudMapMarker::Kind::Checkpoint);
			h.markers.push_back({ e.checkpoints[i].pos, k });
		}
	}

	switch (m_phase) {
	case Phase::FreeRoam: h.phase = m_nearEvent >= 0 ? HudState::Phase::NearEvent : HudState::Phase::FreeRoam; break;
	case Phase::Countdown: h.phase = HudState::Phase::Countdown; break;
	case Phase::Racing: h.phase = HudState::Phase::Racing; break;
	case Phase::Finished: h.phase = HudState::Phase::Finished; break;
	}
	if (m_paused) h.phase = HudState::Phase::Paused;
	if (shown) {
		h.eventType = shown->type;
		h.eventName = shown->name;
		h.eventDifficulty = shown->difficulty;
		h.bestTime = BestTime(shown->id);
		h.checkpointCount = static_cast<int>(shown->checkpoints.size());
	}
	else {
		h.eventType.clear();
		h.eventName.clear();
		h.bestTime = -1.0f;
		h.checkpointCount = 0;
	}
	h.startKey = (m_pad && SDL_GameControllerGetAttached(m_pad)) ? "X" : "Enter";
	h.countdown = m_countdown;
	h.raceTime = m_raceTime;
	h.checkpoint = m_nextCheckpoint;
	h.lastTime = m_lastTime;
	h.newRecord = m_newRecord;
	h.splits = m_splits;
	h.message = m_messageTimer > 0.0f ? m_message : std::string();
	h.messageAlpha = std::clamp(m_messageTimer * 2.0f, 0.0f, 1.0f);
	h.menuTitle = m_menuTitle;
	h.menuItems = m_menuItems;
	h.menuSelected = m_menuSelected;
	const char* views[] = { "CHASE", "FAR CHASE", "BUMPER", "FREE" };
	h.hint = m_playing ? views[m_drive.CameraMode()] : "EDIT - press Play";

	// World-anchored markers.
	h.eventMarkerVisible = false;
	h.checkpointMarkerVisible = false;
	if (m_phase == Phase::FreeRoam && shown) {
		const float baseY = shown->hasStartY ? shown->startY : 0.0f;
		h.eventMarkerVisible = Project(glm::vec3(shown->start.x, baseY + 10.5f, shown->start.y), h.eventMarkerNdc, h.eventMarkerDistance);
	}
	else if ((m_phase == Phase::Racing || m_phase == Phase::Countdown) && m_activeEvent >= 0) {
		const RaceEvent& e = m_events[m_activeEvent];
		if (m_nextCheckpoint < static_cast<int>(e.checkpoints.size())) {
			const Checkpoint& cp = e.checkpoints[m_nextCheckpoint];
			const glm::vec2 c = cp.pos;
			h.checkpointMarkerVisible = Project(glm::vec3(c.x, (cp.hasY ? cp.y : 0.0f) + 2.5f, c.y), h.checkpointMarkerNdc, h.checkpointMarkerDistance);
		}
	}
	(void)dt;
}

WorldMapContext GameSession::BuildWorldMapContext() const
{
	WorldMapContext c;
	for (const RaceEvent& e : m_events) {
		WorldMapMarker mk;
		mk.pos = e.start;
		mk.title = e.name;
		mk.type = e.type;
		if (!e.checkpoints.empty()) { mk.end = e.checkpoints.back().pos; mk.hasEnd = true; }
		c.events.push_back(mk);
	}
	for (const Place& p : m_places) c.places.push_back(p.pos);
	c.player = m_drive.Position();
	c.playerHeading = m_drive.Heading();
	c.route = &m_route;
	c.previewRoute = &m_mapPreview;
	c.hasWaypoint = m_hasWaypoint;
	c.waypoint = m_waypoint;
	c.selectedEvent = m_selectedEvent;
	c.padConnected = m_pad && SDL_GameControllerGetAttached(m_pad);
	return c;
}

void GameSession::OpenWorldMap()
{
	if (!m_drive.IsAvailable()) return;
	m_previewIndex = -2;
	m_worldMap.Open(BuildWorldMapContext());
	UpdateMapPreview(m_worldMap.DisplayedEvent());
	DriveAudio::Get().Play(UiSound::Select);
}

void GameSession::RefreshRoute()
{
	m_routeTimer = 0.0f;
	m_route = m_drive.HasSurface() ? m_roads.FindRoute(m_drive.Position3(), RouteTarget3(), m_heightTolerance)
		: m_roads.FindRoute(m_drive.Position(), RouteTarget());
}

void GameSession::SetWaypoint(const glm::vec2& p)
{
	m_waypoint = p;
	m_hasWaypoint = true;
	m_waypointHasY = false;
	RefreshRoute();
}

void GameSession::SetWaypoint(const glm::vec2& p, float y)
{
	m_waypoint = p;
	m_waypointY = y;
	m_hasWaypoint = true;
	m_waypointHasY = true;
	RefreshRoute();
}

void GameSession::ClearWaypoint()
{
	m_hasWaypoint = false;
	RefreshRoute();
}

// Route preview: start -> checkpoints -> finish, each leg routed through the road graph. Cached per
// event index, so the Dijkstra runs only when the hovered/selected event changes.
void GameSession::UpdateMapPreview(int index)
{
	if (index == m_previewIndex) return;
	m_previewIndex = index;
	m_mapPreview.clear();
	if (index < 0 || index >= static_cast<int>(m_events.size()) || m_roads.Empty()) return;
	const RaceEvent& e = m_events[index];
	glm::vec2 from = e.start;
	m_mapPreview.push_back(from);
	std::vector<glm::vec2> stops;
	for (const Checkpoint& c : e.checkpoints) stops.push_back(c.pos);
	for (const glm::vec2& to : stops) {
		const std::vector<glm::vec2> leg = m_roads.FindRoute(from, to);
		if (leg.size() >= 2) m_mapPreview.insert(m_mapPreview.end(), leg.begin() + 1, leg.end());
		from = to;
	}
}

void GameSession::UpdateWorldMap(const Buttons& b, bool inputEnabled, float dt)
{
	WorldMapInput in;
	in.confirm = b.confirm;
	in.recenter = b.recenter || b.camera;   // F, C, pad Y
	in.close = b.pause || b.back || b.map;  // ESC, Backspace, pad B, M, pad View
	if (inputEnabled) {
		static Uint8 none[SDL_NUM_SCANCODES] = {};
		const Uint8* k = SDL_GetKeyboardFocus() ? SDL_GetKeyboardState(nullptr) : none;
		in.pan.x = ((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) ? 1.0f : 0.0f) - ((k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) ? 1.0f : 0.0f);
		in.pan.y = ((k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) ? 1.0f : 0.0f) - ((k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) ? 1.0f : 0.0f);
		// Q / E step through the events (edge-triggered).
		static bool prevQ = false, prevE = false;
		const bool q = k[SDL_SCANCODE_Q] != 0, e = k[SDL_SCANCODE_E] != 0;
		if (q && !prevQ) in.cycle = -1;
		if (e && !prevE) in.cycle = 1;
		prevQ = q;
		prevE = e;
		if (m_pad && SDL_GameControllerGetAttached(m_pad)) {
			float lx = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
			float ly = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
			lx = std::abs(lx) < 0.12f ? 0.0f : lx;
			ly = std::abs(ly) < 0.12f ? 0.0f : ly;
			in.pan += glm::vec2(lx, ly);
			const float rt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
			const float lt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
			in.zoomRate = rt - lt;
			if (SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) in.cycle = -1;
			if (SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) in.cycle = 1;
		}
	}
	// NOX_TEST_MAP_ZOOM=<steps>: one-off zoom after opening (capture of the zoomed-in view).
	static const float testZoom = [] { const char* v = std::getenv("NOX_TEST_MAP_ZOOM"); return v ? static_cast<float>(std::atof(v)) : 0.0f; }();
	static bool testZoomDone = false;
	if (testZoom != 0.0f && !testZoomDone) { in.zoomSteps += testZoom; testZoomDone = true; }
	in.pointer = m_pointerOn;   // mouse: engine pointer layer (regions from the map's last draw)
	UpdateMapPreview(m_worldMap.DisplayedEvent());
	const WorldMapAction act = m_worldMap.Update(in, BuildWorldMapContext(), dt);
	if (act.selectedByClick) {
		m_mapClickEvent = m_worldMap.SelectedEvent();
		DriveAudio::Get().Play(UiSound::Select);
		std::cout << "[WorldMap] Event " << m_mapClickEvent << " selected by click" << std::endl;
	}
	if (act.toggleWaypoint && act.eventIndex >= 0 && act.eventIndex < static_cast<int>(m_events.size())) {
		const RaceEvent& ev = m_events[act.eventIndex];
		if (m_hasWaypoint && glm::length(m_waypoint - ev.start) < 1.0f) {
			ClearWaypoint();
			ShowMessage("WAYPOINT CLEARED", 2.0f);
		}
		else {
			if (ev.hasStartY) SetWaypoint(ev.start, ev.startY);
			else SetWaypoint(ev.start);
			ShowMessage("WAYPOINT SET - " + ev.name, 2.5f);
		}
		DriveAudio::Get().Play(UiSound::Select);
	}
	// Click on the map: waypoint at the nearest road point (within the snap radius of the click).
	if (act.placeWaypoint && !m_roads.Empty()) {
		glm::vec2 c(0.0f);
		int edge = -1;
		float t = 0.0f;
		const float d = m_roads.ClosestOnRoads(act.worldPos, &c, &edge, &t);
		if (edge >= 0 && d <= act.snapRadius) {
			SetWaypoint(c, m_roads.EdgePoint(edge, t).y);
			float length = 0.0f;
			for (size_t i = 1; i < m_route.size(); ++i) length += glm::length(m_route[i] - m_route[i - 1]);
			ShowMessage("WAYPOINT SET", 2.5f);
			DriveAudio::Get().Play(UiSound::Select);
			std::printf("[WorldMap] Waypoint (%.1f, %.1f) y=%.1f on a road %.1f m from the click; route %d points, %.0f m\n",
				c.x, c.y, m_waypointY, d, static_cast<int>(m_route.size()), length);
		}
		else {
			DriveAudio::Get().Play(UiSound::Back);
			std::printf("[WorldMap] No road within %.0f m of (%.1f, %.1f)\n", act.snapRadius, act.worldPos.x, act.worldPos.y);
		}
		std::fflush(stdout);
	}
	if (act.clearWaypoint && m_hasWaypoint) {
		ClearWaypoint();
		ShowMessage("WAYPOINT CLEARED", 2.0f);
		DriveAudio::Get().Play(UiSound::Back);
		std::cout << "[WorldMap] Waypoint cleared" << std::endl;
	}
	if (act.close) DriveAudio::Get().Play(UiSound::Back);
}

void GameSession::DrawHUD(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, float dt)
{
	if (!m_drive.IsAvailable() || !dl) return;
	// Pointer gating: standalone, the overlay is the whole window; in the editor the Game window
	// must be under the pointer (not covered by another window or a popup).
	m_overlayOrigin = origin;
	m_overlaySize = size;
	m_overlayHovered = m_standalone || ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	if (m_worldMap.IsOpen()) {
		m_worldMap.Draw(dl, origin, size, BuildWorldMapContext(), dt);
		return;
	}
	RacingHUD::Get().Draw(dl, ImVec2(origin.x, origin.y), ImVec2(size.x, size.y), m_hud, dt);
}

float GameSession::BestTime(const std::string& eventId) const
{
	if (m_bestTimes.contains(eventId) && m_bestTimes[eventId].is_object()) return m_bestTimes[eventId].value("time", -1.0f);
	return -1.0f;
}

void GameSession::ClearBestTimes()
{
	m_bestTimes = json::object();
	SaveBestTimes();
}

void GameSession::LoadBestTimes()
{
	std::ifstream in(kBestTimesPath);
	if (!in) { m_bestTimes = json::object(); return; }
	m_bestTimes = json::parse(in, nullptr, false);
	if (!m_bestTimes.is_object()) m_bestTimes = json::object();
}

void GameSession::SaveBestTimes() const
{
	std::ofstream out(kBestTimesPath);
	if (out) out << m_bestTimes.dump(2);
}

// Tyre smoke (burnouts, wheelspin, drifting, locked wheels) and wet-road spray, tied to wheel contact
// and slip (docs/nfs2015/game.md).
void GameSession::EmitTyreEffects(float dt)
{
	if (m_drive.ConsumeTeleportForEffects()) VehicleFX::Clear();
	const float speed = m_drive.SpeedKmh();
	for (int i = 0; i < 4; ++i) {
		const DriveMode::WheelState& w = m_drive.Wheel(i);
		if (!w.grounded) continue;
		const bool rear = i >= 2;
		// Smoke needs real slip energy: wheelspin, a sliding rear in a drift, or locked wheels.
		const float slipSmoke = std::clamp((w.slip - 0.45f) / 0.55f, 0.0f, 1.0f) * std::clamp(speed / 25.0f + w.wheelspin, 0.0f, 1.0f);
		const float smoke = std::max(w.wheelspin, slipSmoke) * (rear ? 1.0f : 0.35f);
		if (smoke > 0.03f) {
			// Spinning tyres fling smoke rearward; sliding tyres leave it behind along the path.
			const float h = m_drive.Heading();
			const glm::vec3 fwd(std::sin(h), 0.0f, std::cos(h));
			const glm::vec3 p = w.contact + glm::vec3(0.0f, 0.18f, 0.0f) - fwd * 0.45f;
			const glm::vec3 v = w.velocity * 0.3f - fwd * (w.wheelspin * 1.8f + 0.3f) + glm::vec3(0.0f, 0.3f, 0.0f);
			VehicleFX::EmitSmoke(p, v, smoke, dt, w.contact.y);
		}
		// Spray from the wet surface behind the tyres, growing with speed.
		const float spray = std::clamp((speed - 20.0f) / 90.0f, 0.0f, 1.0f) * (rear ? 1.0f : 0.45f);
		if (spray > 0.02f) {
			VehicleFX::EmitSpray(w.contact + glm::vec3(0.0f, 0.05f, 0.0f), w.velocity * 0.75f, spray, dt, w.contact.y);
		}
	}
	VehicleFX::Update(dt);
}
