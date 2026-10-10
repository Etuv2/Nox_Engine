#include "GameSession.h"

#include "Camera.h"
#include "DriveAudio.h"
#include "VehicleFX.h"
#include "SceneGraph.h"
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

	glm::vec2 ReadXZ(const json& j, const glm::vec2& fallback) {
		if (j.is_array() && j.size() >= 2 && j[0].is_number() && j[1].is_number()) return glm::vec2(j[0].get<float>(), j[1].get<float>());
		return fallback;
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

	// Road network: authored, else derived from legacy drivable rectangles, else none.
	m_roads.Clear();
	if (!m_roads.Load(m_game.value("road_network", json()))) {
		if (m_game.value("default_district", false)) {
			m_roads.BuildDefaultDistrict();
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
			ev.startHeadingDeg = e.value("start_heading_deg", ev.startHeadingDeg);
			ev.triggerRadius = e.value("trigger_radius", ev.triggerRadius);
			ev.countdown = e.value("countdown", ev.countdown);
			if (e.contains("checkpoints") && e["checkpoints"].is_array()) {
				for (const json& c : e["checkpoints"]) {
					Checkpoint cp;
					if (c.is_array()) cp.pos = ReadXZ(c, cp.pos);
					else if (c.is_object()) {
						cp.pos = ReadXZ(c.value("pos", json()), cp.pos);
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

	m_drive.Configure(m_game, sceneGraph, &m_roads, LayoutPathFromScene(sceneRoot));
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
	g["spawn"] = { {"position", { sp.x, sp.y }}, {"heading_deg", glm::degrees(m_drive.SpawnHeading())} };
	if (!m_roads.Empty() && !g.contains("drivable_areas")) g["road_network"] = m_roads.Save();
	json events = json::array();
	for (const RaceEvent& e : m_events) {
		json je = { {"id", e.id}, {"type", e.type}, {"name", e.name}, {"difficulty", e.difficulty},
			{"start", { e.start.x, e.start.y }}, {"start_heading_deg", e.startHeadingDeg},
			{"trigger_radius", e.triggerRadius}, {"countdown", e.countdown} };
		json cps = json::array();
		for (const Checkpoint& c : e.checkpoints) {
			json jc = { {"pos", { c.pos.x, c.pos.y }}, {"width", c.width} };
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
	glm::vec2 dir = c.pos - prev;
	if (!m_roads.Empty()) {
		std::vector<glm::vec2> path = m_roads.FindRoute(prev, c.pos);
		if (path.size() >= 2) dir = path.back() - path[path.size() - 2];
		if (glm::length(dir) < 1e-3f) dir = c.pos - prev;
	}
	if (glm::length(dir) < 1e-3f) dir = glm::vec2(0, 1);
	return std::atan2(dir.x, dir.y);
}

void GameSession::BeginPlay()
{
	m_playing = true;
	m_paused = false;
	m_phase = Phase::FreeRoam;
	m_menuTitle.clear();
	m_drive.ResetToSpawn();
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
			now.reset = now.reset || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_BACK);
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
		static float apLog = 0.0f;
		apLog += m_hudDt;
		if (apLog > 2.0f) { apLog = 0.0f; std::printf("[Autopilot] pos=(%.1f,%.1f) v=%.0f near=%d phase=%d route=%d\n", pos.x, pos.y, kmh, m_nearEvent, static_cast<int>(m_phase), static_cast<int>(m_route.size())); std::fflush(stdout); }
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
		if (m_route.size() > 2) {
			const float toCorner = glm::length(m_route[1] - pos);
			if (toCorner < 45.0f && kmh > 30.0f + toCorner * 1.1f) { in.throttle = 0.0f; in.brake = 1.0f; in.nitrous = false; }
		}
		if (m_phase == Phase::FreeRoam && m_nearEvent >= 0 && m_autoRuns < 2) {
			in.throttle = 0.0f; in.brake = kmh > 3.0f ? 1.0f : 0.0f; // holding the brake at rest would engage reverse
			now.start = kmh < 5.0f && std::fmod(m_autoTimer, 0.5f) < 0.25f;
			if (now.start && !m_prevButtons.start && m_autoRuns == 0) ++m_autoRuns;
		}
		if (m_phase == Phase::Finished && !m_menuTitle.empty()) {
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
	m_drive.Teleport(e.start, glm::radians(e.startHeadingDeg));
	DriveAudio::Get().Play(UiSound::Select);
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
		m_drive.Teleport(from, heading);
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
	const std::string item = m_menuItems[m_menuSelected];
	DriveAudio::Get().Play(UiSound::Select);
	if (item == "Resume") SetPaused(false);
	else if (item == "Restart Event" || item == "Retry") { const int e = m_activeEvent; m_paused = false; StartEvent(e); }
	else if (item == "Quit Event" || item == "Free Roam") { QuitEvent(); }
	else if (item == "Reset Car") { SetPaused(false); ResetCar(); }
	else if (item == "Exit Game") action = MenuAction::ExitApplication;
}

glm::vec2 GameSession::RouteTarget() const
{
	if (m_activeEvent >= 0 && (m_phase == Phase::Racing || m_phase == Phase::Countdown)) {
		const RaceEvent& e = m_events[m_activeEvent];
		return e.checkpoints[std::min(m_nextCheckpoint, static_cast<int>(e.checkpoints.size()) - 1)].pos;
	}
	if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size())) return m_events[m_selectedEvent].start;
	return m_drive.Position();
}

void GameSession::UpdateRace(float dt)
{
	const glm::vec2 pos = m_drive.Position();
	const glm::vec2 prev = m_drive.PreviousPosition();
	m_messageTimer = std::max(0.0f, m_messageTimer - dt);

	if (m_phase == Phase::FreeRoam) {
		m_nearEvent = -1;
		for (size_t i = 0; i < m_events.size(); ++i) {
			if (glm::length(pos - m_events[i].start) < m_events[i].triggerRadius && m_drive.SpeedKmh() < 60.0f) {
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
			if (glm::dot(pos - prev, dir) <= 0.0f) continue; // crossing backwards does not count
			if (i == m_nextCheckpoint) {
				m_splits.push_back(m_raceTime);
				++m_nextCheckpoint;
				std::cout << "[Race] Checkpoint " << m_nextCheckpoint << "/" << count << " at " << m_raceTime << " s" << std::endl;
				if (m_nextCheckpoint >= count) {
					m_phase = Phase::Finished;
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

	// Route guidance, refreshed a few times per second.
	m_routeTimer -= dt;
	if (m_routeTimer <= 0.0f) {
		m_routeTimer = 0.2f;
		m_route = m_phase == Phase::Finished ? std::vector<glm::vec2>() : m_roads.FindRoute(pos, RouteTarget());
	}
}

GameSession::MenuAction GameSession::Update(float dt, bool inputEnabled, float aspect)
{
	MenuAction action = MenuAction::None;
	m_aspect = aspect;
	m_hudDt = dt;
	if (!m_drive.IsAvailable()) return action;
	if (!m_playing) {
		// Editing: the car rests at spawn and the camera frames the reference composition.
		DriveMode::Input none;
		m_drive.Update(dt, none, true, m_camera.get(), aspect);
		m_route = m_roads.FindRoute(m_drive.Position(), RouteTarget());
		BuildHudState(dt);
		return action;
	}

	DriveMode::Input in;
	Buttons b;
	ReadInput(inputEnabled, in, b);
	if (b.pause) {
		if (m_paused) SetPaused(false);
		else if (m_menuTitle.empty()) SetPaused(true);
	}
	const bool menuOpen = !m_menuTitle.empty();
	if (menuOpen) UpdateMenus(b, action);
	else {
		if (b.camera) m_drive.CycleCamera();
		if (b.reset) ResetCar();
		if (b.start && m_phase == Phase::FreeRoam && m_nearEvent >= 0) StartEvent(m_nearEvent);
	}

	const bool frozen = m_paused || m_phase == Phase::Countdown || (!m_menuTitle.empty() && m_phase != Phase::Finished);
	if (m_paused) {
		DriveMode::Input none;
		m_drive.Update(0.0f, none, true, m_camera.get(), aspect);
	}
	else {
		DriveMode::Input drive = in;
		if (m_phase == Phase::Finished) { drive = DriveMode::Input(); drive.brake = 0.6f; }
		m_drive.Update(dt, drive, frozen, m_camera.get(), aspect);
		EmitTyreEffects(dt);
		UpdateRace(dt);
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
	h.carHeading = m_drive.Heading();
	h.roads = &m_roads;
	h.route = m_route;
	h.markers.clear();

	const RaceEvent* shown = nullptr;
	if (m_activeEvent >= 0 && m_phase != Phase::FreeRoam) shown = &m_events[m_activeEvent];
	else if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<int>(m_events.size())) shown = &m_events[m_selectedEvent];

	if (m_phase == Phase::FreeRoam) {
		for (const RaceEvent& e : m_events) h.markers.push_back({ e.start, HudMapMarker::Kind::Event });
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
		h.eventMarkerVisible = Project(glm::vec3(shown->start.x, 10.5f, shown->start.y), h.eventMarkerNdc, h.eventMarkerDistance);
	}
	else if ((m_phase == Phase::Racing || m_phase == Phase::Countdown) && m_activeEvent >= 0) {
		const RaceEvent& e = m_events[m_activeEvent];
		if (m_nextCheckpoint < static_cast<int>(e.checkpoints.size())) {
			const glm::vec2 c = e.checkpoints[m_nextCheckpoint].pos;
			h.checkpointMarkerVisible = Project(glm::vec3(c.x, 2.5f, c.y), h.checkpointMarkerNdc, h.checkpointMarkerDistance);
		}
	}
	(void)dt;
}

void GameSession::DrawHUD(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, float dt)
{
	if (!m_drive.IsAvailable() || !dl) return;
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
			VehicleFX::EmitSmoke(p, v, smoke, dt);
		}
		// Spray from the wet surface behind the tyres, growing with speed.
		const float spray = std::clamp((speed - 20.0f) / 90.0f, 0.0f, 1.0f) * (rear ? 1.0f : 0.45f);
		if (spray > 0.02f) {
			VehicleFX::EmitSpray(w.contact + glm::vec3(0.0f, 0.05f, 0.0f), w.velocity * 0.75f, spray, dt);
		}
	}
	VehicleFX::Update(dt);
}
