#include "DriveMode.h"

#include "Camera.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include <SDL/SDL.h>
#include <IMGUI/imgui.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <iostream>

using nlohmann::json;

namespace {
	ImFont* g_hudFontLarge = nullptr;
	ImFont* g_hudFontSmall = nullptr;
	constexpr float kPi = 3.14159265f;

	std::shared_ptr<SceneNode> FindByName(const std::shared_ptr<SceneNode>& node, const std::string& name) {
		if (!node) return nullptr;
		if (node->GetName() == name) return node;
		for (const auto& child : node->children) {
			if (auto found = FindByName(child, name)) return found;
		}
		return nullptr;
	}

	float Approach(float current, float target, float rate) {
		return current + (target - current) * (1.0f - std::exp(-rate));
	}

	// Gear upper speeds (m/s) for a 6-speed box with a 78 m/s top speed; scaled to the actual top speed.
	const float kGearTop[6] = { 0.20f, 0.36f, 0.52f, 0.68f, 0.84f, 1.0f };
}

void DriveMode::LoadFonts()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.Fonts->Fonts.Size == 0) {
		io.Fonts->AddFontDefault(); // keep the editor's default font first
	}
	g_hudFontLarge = io.Fonts->AddFontFromFileTTF("fonts/arial.ttf", 96.0f);
	g_hudFontSmall = io.Fonts->AddFontFromFileTTF("fonts/arial.ttf", 30.0f);
	if (!g_hudFontLarge || !g_hudFontSmall) {
		std::cerr << "[DriveMode] HUD fonts unavailable; using the default font" << std::endl;
	}
}

void DriveMode::Configure(const std::string& sceneFilePath, SceneGraph& sceneGraph)
{
	m_available = false;
	m_active = false;
	m_areas.clear();
	std::ifstream in(sceneFilePath);
	const json scene = in ? json::parse(in, nullptr, false) : json();
	if (!scene.is_object() || !scene.contains("game") || !scene["game"].is_object()) {
		return;
	}
	const json& g = scene["game"];
	auto root = sceneGraph.GetRoot();
	auto player = FindByName(root, g.value("player_node", std::string("PlayerCar")));
	if (!player) {
		std::cerr << "[DriveMode] player_node not found" << std::endl;
		return;
	}
	m_player = player;
	if (auto body = FindByName(root, g.value("body_node", std::string()))) {
		m_body = body;
		m_bodyBasePos = body->GetPosition();
		m_bodyScale = body->GetScale();
	}
	if (g.contains("drivable_areas") && g["drivable_areas"].is_array()) {
		for (const json& a : g["drivable_areas"]) {
			if (a.is_array() && a.size() >= 4) {
				m_areas.push_back({ glm::vec2(a[0].get<float>(), a[1].get<float>()), glm::vec2(a[2].get<float>(), a[3].get<float>()) });
			}
		}
	}
	m_topSpeed = std::max(g.value("top_speed_kmh", 280.0f), 40.0f) / 3.6f;
	const glm::vec3 p = player->GetPosition();
	m_spawnPos = glm::vec2(p.x, p.z);
	const glm::vec3 fwd = player->GetOrientation() * glm::vec3(0, 0, 1);
	m_spawnHeading = std::atan2(fwd.x, fwd.z);
	ResetToSpawn();
	m_available = true;
	if (g.value("start_in_drive_mode", false)) {
		SetActive(true);
	}
	std::cout << "[DriveMode] Ready (F9 toggles). Areas: " << m_areas.size() << std::endl;
}

void DriveMode::ResetToSpawn()
{
	m_pos = m_spawnPos;
	m_heading = m_spawnHeading;
	m_speed = m_lateral = m_steer = m_yawRate = 0.0f;
	m_nitro = 1.0f;
	m_gear = 1;
	m_camInitialized = false;
	m_teleported = true;
}

void DriveMode::SetActive(bool active)
{
	if (!m_available) {
		m_active = false;
		return;
	}
	m_active = active;
	m_camInitialized = false;
	if (active && !m_pad) {
		for (int i = 0; i < SDL_NumJoysticks(); ++i) {
			if (SDL_IsGameController(i)) {
				m_pad = SDL_GameControllerOpen(i);
				break;
			}
		}
	}
	if (active) {
		// Continue from wherever a benchmark or the editor left the car.
		if (auto player = m_player.lock()) {
			const glm::vec3 p = player->GetPosition();
			m_pos = glm::vec2(p.x, p.z);
		}
	}
	std::cout << "[DriveMode] " << (active ? "Drive mode" : "Editor mode") << std::endl;
}

DriveMode::Input DriveMode::ReadInput()
{
	Input in;
	// NOX_DRIVE_AUTOPILOT=1: scripted input for unattended validation captures (accelerate, weave,
	// nitrous, then a handbrake drift).
	static const bool autopilot = [] {
		const char* v = std::getenv("NOX_DRIVE_AUTOPILOT");
		return v && v[0] == '1';
	}();
	if (autopilot) {
		// Lane keeping toward x = -1.6 (right lane; the car's right is -X when heading +Z).
		float h = m_heading;
		while (h > kPi) h -= 2.0f * kPi;
		while (h < -kPi) h += 2.0f * kPi;
		in.throttle = 1.0f;
		in.steer = std::clamp((m_pos.x + 1.6f) * 0.25f + h * 2.5f + 0.15f * std::sin(m_time * 0.9f), -1.0f, 1.0f);
		in.nitrous = m_time > 3.0f && m_time < 5.5f;
		in.handbrake = m_time > 7.0f && m_time < 7.6f;
		if (m_time > 7.0f && m_time < 8.2f) in.steer = -0.9f;
		return in;
	}
	const Uint8* k = SDL_GetKeyboardState(nullptr);
	in.throttle = (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) ? 1.0f : 0.0f;
	in.brake = (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) ? 1.0f : 0.0f;
	in.steer = ((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) ? 1.0f : 0.0f) - ((k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) ? 1.0f : 0.0f);
	in.handbrake = k[SDL_SCANCODE_SPACE] != 0;
	in.nitrous = (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) != 0;
	if (m_pad) {
		const float rt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
		const float lt = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
		float lx = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
		lx = std::abs(lx) < 0.12f ? 0.0f : (lx - 0.12f * (lx > 0 ? 1.0f : -1.0f)) / 0.88f;
		in.throttle = std::max(in.throttle, rt);
		in.brake = std::max(in.brake, lt);
		if (std::abs(lx) > std::abs(in.steer)) in.steer = lx;
		in.handbrake = in.handbrake || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_B);
		in.nitrous = in.nitrous || SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_A);
	}
	return in;
}

bool DriveMode::InsideDrivable(const glm::vec2& p) const
{
	if (m_areas.empty()) return true;
	for (const Area& a : m_areas) {
		if (p.x >= a.min.x && p.x <= a.max.x && p.y >= a.min.y && p.y <= a.max.y) return true;
	}
	return false;
}

void DriveMode::Simulate(float dt, const Input& in)
{
	const float speed01 = std::clamp(std::abs(m_speed) / m_topSpeed, 0.0f, 1.0f);
	m_steer = Approach(m_steer, in.steer, dt * (std::abs(in.steer) > std::abs(m_steer) ? 9.0f : 12.0f));

	// Longitudinal: engine force falls off toward top speed; nitrous adds thrust and top speed.
	m_nitroActive = in.nitrous && m_nitro > 0.0f && in.throttle > 0.0f;
	const float topSpeed = m_topSpeed * (m_nitroActive ? 1.12f : 1.0f);
	float accel = 0.0f;
	if (in.throttle > 0.0f && m_speed >= -0.5f) {
		const float s = std::clamp(m_speed / topSpeed, 0.0f, 1.0f);
		accel += in.throttle * 11.0f * (1.0f - s * s) * (m_nitroActive ? 1.7f : 1.0f);
	}
	if (in.brake > 0.0f) {
		if (m_speed > 0.5f) accel -= in.brake * 16.0f;
		else accel -= in.brake * 6.0f * (m_speed > -12.0f ? 1.0f : 0.0f); // reverse
	}
	if (in.handbrake) accel -= std::copysign(std::min(std::abs(m_speed), 1.0f) * 7.0f, m_speed);
	accel -= m_speed * (0.012f * std::abs(m_speed) + 0.08f); // aero + rolling drag
	m_nitro = std::clamp(m_nitro + (m_nitroActive ? -0.22f : 0.04f) * dt, 0.0f, 1.0f);
	const float prevSpeed = m_speed;
	m_speed += accel * dt;
	if (in.throttle == 0.0f && in.brake == 0.0f && std::abs(m_speed) < 0.2f) m_speed = 0.0f;

	// Lateral: bicycle-model yaw from steering, reduced steering lock at speed; the handbrake breaks
	// rear grip so the car rotates and slides (drift), with grip recovering afterwards.
	const float maxLock = glm::radians(glm::mix(34.0f, 6.0f, speed01));
	const float wheelbase = 2.6f;
	// Heading increases toward +X, which is the car's left (right = -X when facing +Z).
	float targetYaw = -m_speed / wheelbase * std::tan(m_steer * maxLock);
	if (in.handbrake && std::abs(m_speed) > 8.0f) targetYaw *= 1.9f;
	m_yawRate = Approach(m_yawRate, targetYaw, dt * (in.handbrake ? 5.0f : 8.0f));
	m_heading += m_yawRate * dt;
	const float grip = in.handbrake ? 1.2f : 6.5f;
	m_lateral += (-m_yawRate * m_speed * (in.handbrake ? 0.35f : 0.12f)) * dt;
	m_lateral *= std::exp(-grip * dt);

	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 right(fwd.y, -fwd.x);
	const glm::vec2 velocity = fwd * m_speed + right * m_lateral;
	glm::vec2 next = m_pos + velocity * dt;
	if (!InsideDrivable(next)) {
		// Wall contact: keep the velocity component along the wall, lose the part into it, and turn
		// the car parallel to the wall so it scrapes along instead of sticking.
		const glm::vec2 alongX(next.x, m_pos.y);
		const glm::vec2 alongZ(m_pos.x, next.y);
		float wallHeading = m_heading;
		if (InsideDrivable(alongZ)) {
			next = alongZ;                                       // wall is perpendicular to X
			wallHeading = std::cos(m_heading) >= 0.0f ? 0.0f : kPi;
		}
		else if (InsideDrivable(alongX)) {
			next = alongX;                                       // wall is perpendicular to Z
			wallHeading = std::sin(m_heading) >= 0.0f ? kPi * 0.5f : -kPi * 0.5f;
		}
		else {
			next = m_pos;
		}
		float dh = wallHeading - m_heading;
		while (dh > kPi) dh -= 2.0f * kPi;
		while (dh < -kPi) dh += 2.0f * kPi;
		const float alignment = std::abs(std::cos(dh));        // 1 = already parallel
		m_heading += dh * (1.0f - std::exp(-dt * 6.0f));
		m_speed *= 0.97f * glm::mix(0.6f, 1.0f, alignment);
		m_lateral *= 0.3f;
		m_yawRate *= 0.5f;
	}
	m_pos = next;

	m_longAccel = Approach(m_longAccel, (m_speed - prevSpeed) / std::max(dt, 1e-4f), dt * 6.0f);
	m_latAccel = Approach(m_latAccel, m_yawRate * m_speed, dt * 6.0f);

	// Gearbox / rpm for the HUD.
	const float s = std::abs(m_speed) / m_topSpeed;
	m_gear = 1;
	while (m_gear < 6 && s > kGearTop[m_gear - 1]) ++m_gear;
	const float gearLow = m_gear == 1 ? 0.0f : kGearTop[m_gear - 2];
	const float gearT = std::clamp((s - gearLow) / std::max(kGearTop[m_gear - 1] - gearLow, 1e-3f), 0.0f, 1.0f);
	m_rpm = Approach(m_rpm, 1100.0f + gearT * 6400.0f + (in.throttle > 0.0f ? 300.0f : 0.0f), dt * 10.0f);
	if (m_speed < -0.5f) m_gear = 0; // reverse

	m_roll = Approach(m_roll, std::clamp(-m_latAccel * 0.0045f, -0.06f, 0.06f), dt * 5.0f);
	m_pitch = Approach(m_pitch, std::clamp(-m_longAccel * 0.0035f, -0.04f, 0.04f), dt * 5.0f);
}

void DriveMode::ApplyTransforms()
{
	if (auto player = m_player.lock()) {
		const glm::vec3 pos(m_pos.x, player->GetPosition().y, m_pos.y);
		player->SetLocalTRS(pos, glm::angleAxis(m_heading, glm::vec3(0, 1, 0)), player->GetScale());
	}
	if (auto body = m_body.lock()) {
		// Body roll/pitch about the car's local axes (suspension feel); the glTF node keeps its own
		// orientation below this node.
		const glm::quat q = glm::angleAxis(m_roll, glm::vec3(0, 0, 1)) * glm::angleAxis(m_pitch, glm::vec3(1, 0, 0));
		body->SetLocalTRS(m_bodyBasePos, q, m_bodyScale);
	}
}

void DriveMode::UpdateCamera(float dt, Camera& camera)
{
	const float speed01 = std::clamp(std::abs(m_speed) / m_topSpeed, 0.0f, 1.0f);
	const glm::vec3 carPos(m_pos.x, 0.0f, m_pos.y);
	if (!m_camInitialized) {
		m_camHeading = m_heading;
	}
	// Heading lag: the camera trails the car's yaw, so drifts show the car's flank.
	float dh = m_heading - m_camHeading;
	while (dh > kPi) dh -= 2.0f * kPi;
	while (dh < -kPi) dh += 2.0f * kPi;
	m_camHeading += dh * (1.0f - std::exp(-dt * (m_cameraMode == 2 ? 30.0f : 4.5f)));
	const glm::vec3 camFwd(std::sin(m_camHeading), 0.0f, std::cos(m_camHeading));

	float distance = 5.4f, height = 1.45f, lookAhead = 4.0f, lookHeight = 0.85f;
	if (m_cameraMode == 1) { distance = 8.0f; height = 2.3f; lookAhead = 5.0f; lookHeight = 1.0f; }
	// Pull back with speed and under hard acceleration (NFS-style speed sensation).
	distance += speed01 * 1.2f + std::clamp(m_longAccel * 0.04f, -0.2f, 0.6f);
	glm::vec3 desired = carPos - camFwd * distance + glm::vec3(0.0f, height, 0.0f);
	glm::vec3 target = carPos + camFwd * lookAhead + glm::vec3(0.0f, lookHeight, 0.0f);
	if (m_cameraMode == 2) {
		const glm::vec3 fwd(std::sin(m_heading), 0.0f, std::cos(m_heading));
		desired = carPos + fwd * 1.9f + glm::vec3(0.0f, 0.75f, 0.0f);
		target = desired + fwd * 10.0f;
	}

	if (!m_camInitialized || m_cameraMode == 2) {
		m_camPos = desired;
		m_camVel = glm::vec3(0.0f);
		m_camInitialized = true;
	}
	else {
		// Critically damped spring.
		const float omega = 9.0f;
		const glm::vec3 x = m_camPos - desired;
		const float exp = std::exp(-omega * dt);
		const glm::vec3 temp = (m_camVel + omega * x) * dt;
		m_camVel = (m_camVel - omega * temp) * exp;
		m_camPos = desired + (x + temp) * exp;
	}
	glm::vec3 eye = m_camPos;
	if (speed01 > 0.55f) {
		// Light high-speed shake.
		const float a = (speed01 - 0.55f) * 0.03f;
		eye += glm::vec3(std::sin(m_time * 37.0f), std::sin(m_time * 53.0f + 1.3f), 0.0f) * a;
	}
	const float targetFov = 55.0f + speed01 * 13.0f + (m_nitroActive ? 7.0f : 0.0f);
	m_camFov = Approach(m_camFov, targetFov, dt * 3.0f);

	glm::vec3 f = glm::normalize(target - eye);
	const float pitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
	const float yaw = glm::degrees(std::atan2(f.z, f.x));
	camera.SetPose(eye, glm::vec3(0, 1, 0), yaw, pitch);
	camera.SetPerspective(m_camFov, 1.0f, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
}

bool DriveMode::Update(float dt, Camera& camera)
{
	if (!m_active) return false;
	dt = std::clamp(dt, 0.0f, 0.05f);
	m_time += dt;
	const Uint8* k = SDL_GetKeyboardState(nullptr);
	if (k[SDL_SCANCODE_R]) ResetToSpawn();
	Simulate(dt, ReadInput());
	ApplyTransforms();
	if (m_cameraMode == 3) return false; // free camera: the editor controls drive it
	UpdateCamera(dt, camera);
	return true;
}

void DriveMode::DrawHUD(int width, int height) const
{
	if (!m_active) return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const float scale = static_cast<float>(height) / 1440.0f;
	const ImU32 white = IM_COL32(240, 244, 255, 235);
	const ImU32 dim = IM_COL32(240, 244, 255, 70);
	const ImU32 accent = IM_COL32(64, 214, 255, 235);
	const ImU32 red = IM_COL32(255, 70, 60, 235);

	// --- Speed, gear and rpm arc (bottom right).
	const ImVec2 c(width - 260.0f * scale, height - 230.0f * scale);
	const float r = 150.0f * scale;
	const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
	const int segments = 40;
	const float rpm01 = std::clamp((m_rpm - 800.0f) / 7000.0f, 0.0f, 1.0f);
	for (int i = 0; i < segments; ++i) {
		const float t0 = static_cast<float>(i) / segments;
		const float t1 = (i + 0.7f) / segments;
		const float b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
		const ImU32 col = t0 < rpm01 ? (t0 > 0.85f ? red : white) : dim;
		dl->PathArcTo(c, r, b0, b1, 4);
		dl->PathStroke(col, 0, 10.0f * scale);
	}
	char speedText[16];
	std::snprintf(speedText, sizeof(speedText), "%d", static_cast<int>(std::abs(m_speed) * 3.6f + 0.5f));
	ImFont* big = g_hudFontLarge ? g_hudFontLarge : ImGui::GetFont();
	ImFont* small = g_hudFontSmall ? g_hudFontSmall : ImGui::GetFont();
	const float bigSize = 96.0f * scale, smallSize = 30.0f * scale;
	ImVec2 ts = big->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, speedText);
	dl->AddText(big, bigSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.62f), white, speedText);
	ts = small->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, "KM/H");
	dl->AddText(small, smallSize, ImVec2(c.x - ts.x * 0.5f, c.y + 34.0f * scale), dim, "KM/H");
	char gearText[4];
	std::snprintf(gearText, sizeof(gearText), "%s", m_gear == 0 ? "R" : std::to_string(m_gear).c_str());
	ts = small->CalcTextSizeA(smallSize * 1.4f, FLT_MAX, 0.0f, gearText);
	dl->AddText(small, smallSize * 1.4f, ImVec2(c.x - ts.x * 0.5f, c.y + 78.0f * scale), accent, gearText);

	// Nitrous gauge.
	const ImVec2 n0(c.x - r, c.y + r + 12.0f * scale);
	const ImVec2 n1(c.x + r, n0.y + 10.0f * scale);
	dl->AddRectFilled(n0, n1, dim, 3.0f * scale);
	dl->AddRectFilled(n0, ImVec2(n0.x + (n1.x - n0.x) * m_nitro, n1.y), m_nitroActive ? IM_COL32(150, 240, 255, 255) : accent, 3.0f * scale);

	// --- Rotating minimap (bottom left): drivable areas around the car, heading up.
	const ImVec2 mc(220.0f * scale, height - 220.0f * scale);
	const float mr = 150.0f * scale;
	const float metresToPx = mr / 90.0f;
	dl->AddCircleFilled(mc, mr, IM_COL32(8, 12, 20, 170), 64);
	dl->AddCircle(mc, mr, IM_COL32(240, 244, 255, 90), 64, 2.0f * scale);
	const float ch = std::cos(m_heading), sh = std::sin(m_heading);
	auto toMap = [&](const glm::vec2& world) {
		const glm::vec2 d = world - m_pos;
		// Rotate so the car's forward (+Z rotated by heading) points up on screen.
		const float x = d.x * ch - d.y * sh;
		const float y = d.x * sh + d.y * ch;
		return ImVec2(mc.x - x * metresToPx, mc.y - y * metresToPx);
	};
	dl->PushClipRect(ImVec2(mc.x - mr, mc.y - mr), ImVec2(mc.x + mr, mc.y + mr), true);
	for (const Area& a : m_areas) {
		const ImVec2 p0 = toMap(a.min), p1 = toMap(glm::vec2(a.max.x, a.min.y));
		const ImVec2 p2 = toMap(a.max), p3 = toMap(glm::vec2(a.min.x, a.max.y));
		dl->AddQuadFilled(p0, p1, p2, p3, IM_COL32(120, 150, 190, 120));
	}
	dl->PopClipRect();
	const float tri = 12.0f * scale;
	dl->AddTriangleFilled(ImVec2(mc.x, mc.y - tri * 1.4f), ImVec2(mc.x - tri, mc.y + tri), ImVec2(mc.x + tri, mc.y + tri), accent);

	// Camera / controls hint (top left, fades into the frame like the game's small prompts).
	const char* views[] = { "CHASE", "FAR CHASE", "BUMPER", "FREE" };
	dl->AddText(small, smallSize * 0.8f, ImVec2(40.0f * scale, 36.0f * scale), dim, views[m_cameraMode]);
}
