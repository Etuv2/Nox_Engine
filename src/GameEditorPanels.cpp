// Game layer: the racing editor panels (race and routes, drive tuning, racing HUD), the race
// authoring overlay in the Scene viewport, and the racing toolbar icon roles. The engine editor
// reaches them only through the EditorExtensions registry.
#include "GameEditorPanels.h"

#include "BaseLight.h"
#include "EditorExtensions.h"
#include "GameModule.h"
#include "GameSession.h"
#include "LightNode.h"
#include "ModularRenderer.h"
#include "RenderContext.h"
#include "RacingHUD.h"
#include "ImGui/RenderingSettingsWindow.h"
#include "RoadNetwork.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "editor/solace/SolaceUI.h"

#include <IMGUI/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace {
	// Checkpoint highlighted in the Race panel; set by clicking a checkpoint handle in the viewport.
	int g_selectedCheckpoint = -1;

	GameSession* RacingGame(EditorHost& host) { return dynamic_cast<GameSession*>(host.Game()); }

	std::string FormatTime(float t) {
		if (t < 0.0f) return "--:--.--";
		char buf[32];
		const int m = static_cast<int>(t / 60.0f);
		std::snprintf(buf, sizeof(buf), "%d:%05.2f", m, t - m * 60.0f);
		return buf;
	}

	static void DrawRaceTools(EditorHost& host)
	{
		GameSession* game = RacingGame(host);
		if (!game || !game->IsAvailable()) {
			ImGui::TextDisabled("This scene has no \"game\" section (player car).");
			return;
		}
		auto& events = game->Events();
		const bool editable = !host.IsPlaying();
		ImGui::BeginChild("##eventlist", ImVec2(220, 0), true);
		ImGui::TextDisabled("EVENTS");
		for (int i = 0; i < static_cast<int>(events.size()); ++i) {
			ImGui::PushID(i);
			if (ImGui::Selectable(events[i].name.c_str(), game->SelectedEvent() == i)) game->SetSelectedEvent(i);
			ImGui::PopID();
		}
		ImGui::BeginDisabled(!editable);
		if (ui::Btn("+ Event", false)) {
			GameSession::RaceEvent e;
			e.id = "event_" + std::to_string(events.size() + 1);
			e.name = "New Sprint";
			e.start = game->Drive().Position() + glm::vec2(std::sin(game->Drive().Heading()), std::cos(game->Drive().Heading())) * 20.0f;
			e.startHeadingDeg = glm::degrees(game->Drive().Heading());
			e.checkpoints.push_back({ e.start + glm::vec2(std::sin(game->Drive().Heading()), std::cos(game->Drive().Heading())) * 120.0f });
			events.push_back(e);
			game->SetSelectedEvent(static_cast<int>(events.size()) - 1);
			host.CommitGameEdit("Add event");
		}
		ImGui::SameLine();
		if (ui::Btn("Duplicate", false) && !events.empty()) {
			GameSession::RaceEvent e = events[game->SelectedEvent()];
			e.id += "_copy";
			e.name += " (copy)";
			e.start += glm::vec2(6.0f, 0.0f);
			events.push_back(e);
			game->SetSelectedEvent(static_cast<int>(events.size()) - 1);
			host.CommitGameEdit("Duplicate event");
		}
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextDisabled("SPEEDWALL");
		for (const auto& e : events) ImGui::Text("%s  %s", e.id.c_str(), FormatTime(game->BestTime(e.id)).c_str());
		if (ImGui::SmallButton("Clear best times")) game->ClearBestTimes();
		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::BeginChild("##eventprops", ImVec2(0, 0), false);
		if (!events.empty()) {
			auto& e = events[std::clamp(game->SelectedEvent(), 0, static_cast<int>(events.size()) - 1)];
			ImGui::BeginDisabled(!editable);
			auto text = [&](const char* label, std::string& s) {
				char buf[128];
				std::snprintf(buf, sizeof(buf), "%s", s.c_str());
				ui::Label(label);
				if (ImGui::InputText((std::string("##") + label).c_str(), buf, sizeof(buf))) s = buf;
			};
			ui::Grid("##ev");
			text("Id", e.id);
			text("Type", e.type);
			text("Name", e.name);
			text("Difficulty", e.difficulty);
			ui::Label("Start (x, z)"); ImGui::DragFloat2("##st", &e.start.x, 0.25f);
			ui::Label("Start heading"); ImGui::DragFloat("##sh", &e.startHeadingDeg, 1.0f, -180.0f, 180.0f, "%.0f deg");
			ui::Label("Trigger radius"); ImGui::DragFloat("##tr", &e.triggerRadius, 0.1f, 2.0f, 40.0f, "%.1f m");
			ui::Label("Countdown"); ImGui::DragFloat("##cd", &e.countdown, 0.1f, 1.0f, 10.0f, "%.1f s");
			ui::EndGrid();
			if (ui::Btn("Start = car position", false)) {
				e.start = game->Drive().Position();
				e.startHeadingDeg = glm::degrees(game->Drive().Heading());
				host.CommitGameEdit("Set event start");
			}
			ImGui::SameLine();
			if (ui::Btn("Delete event", false)) {
				events.erase(events.begin() + game->SelectedEvent());
				game->SetSelectedEvent(0);
				host.CommitGameEdit("Delete event");
				ImGui::EndDisabled();
				ImGui::EndChild();
				return;
			}
			ImGui::Separator();
			ImGui::TextDisabled("CHECKPOINTS (ordered; the last one is the finish)");
			if (ImGui::BeginTable("##cps", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28);
				ImGui::TableSetupColumn("Position (x, z)");
				ImGui::TableSetupColumn("Width");
				ImGui::TableSetupColumn("Heading");
				ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 56);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
				ImGui::TableHeadersRow();
				int remove = -1, moveUp = -1, moveDown = -1, dup = -1;
				for (int i = 0; i < static_cast<int>(e.checkpoints.size()); ++i) {
					auto& c = e.checkpoints[i];
					ImGui::PushID(i);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (ImGui::Selectable(std::to_string(i + 1).c_str(), g_selectedCheckpoint == i)) g_selectedCheckpoint = i;
					ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat2("##p", &c.pos.x, 0.25f);
					ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##w", &c.width, 0.1f, 4.0f, 60.0f, "%.1f m");
					ImGui::TableNextColumn();
					ImGui::Checkbox("auto", &c.autoHeading);
					if (!c.autoHeading) { ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##h", &c.headingDeg, 1.0f, -180.0f, 180.0f, "%.0f"); }
					ImGui::TableNextColumn();
					if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = i;
					ImGui::SameLine();
					if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) moveDown = i;
					ImGui::TableNextColumn();
					if (ImGui::SmallButton("dup")) dup = i;
					ImGui::SameLine();
					if (ImGui::SmallButton("del")) remove = i;
					ImGui::PopID();
				}
				ImGui::EndTable();
				if (moveUp > 0) { std::swap(e.checkpoints[moveUp], e.checkpoints[moveUp - 1]); host.CommitGameEdit("Reorder checkpoint"); }
				if (moveDown >= 0 && moveDown + 1 < static_cast<int>(e.checkpoints.size())) { std::swap(e.checkpoints[moveDown], e.checkpoints[moveDown + 1]); host.CommitGameEdit("Reorder checkpoint"); }
				if (dup >= 0) { auto c = e.checkpoints[dup]; c.pos += glm::vec2(0, 10); e.checkpoints.insert(e.checkpoints.begin() + dup + 1, c); host.CommitGameEdit("Duplicate checkpoint"); }
				if (remove >= 0 && e.checkpoints.size() > 1) { e.checkpoints.erase(e.checkpoints.begin() + remove); host.CommitGameEdit("Delete checkpoint"); }
			}
			if (ui::Btn("+ Checkpoint at car", false)) {
				e.checkpoints.push_back({ game->Drive().Position() });
				host.CommitGameEdit("Add checkpoint");
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			ImGui::TextDisabled("TEST");
			if (ui::Btn(host.IsPlaying() ? "Restart event" : "Play this event", true)) {
				if (!host.IsPlaying()) host.RequestPlay();
				game->StartEvent(game->SelectedEvent());
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Phase: %s  time %s  next CP %d", game->GetPhase() == GameSession::Phase::Racing ? "racing" :
				game->GetPhase() == GameSession::Phase::Countdown ? "countdown" : game->GetPhase() == GameSession::Phase::Finished ? "finished" : "free roam",
				FormatTime(game->RaceTime()).c_str(), game->NextCheckpoint() + 1);
		}
		ImGui::Separator();
		if (ui::Section("Road network", false)) {
			RoadNetwork& roads = game->Roads();
			ImGui::BeginDisabled(!editable);
			float hw = roads.DefaultHalfWidth(), sw = roads.SidewalkWidth();
			ui::Grid("##roads");
			ui::Label("Road half width"); if (ImGui::DragFloat("##hw", &hw, 0.05f, 2.0f, 15.0f, "%.2f m")) roads.SetDefaultHalfWidth(hw);
			ui::Label("Sidewalk width"); if (ImGui::DragFloat("##sw", &sw, 0.05f, 0.0f, 10.0f, "%.2f m")) roads.SetSidewalkWidth(sw);
			ui::EndGrid();
			if (roads.IsExternal()) {
				ImGui::TextDisabled("Generated network: %s (read-only)", roads.SourceFile().c_str());
			}
			for (size_t i = 0; i < roads.Nodes().size() && !roads.IsExternal(); ++i) {
				auto& n = roads.MutableNodes()[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::SetNextItemWidth(220);
				ImGui::DragFloat2(n.id.c_str(), &n.pos.x, 0.5f);
				ImGui::PopID();
			}
			ImGui::TextDisabled("%d streets. Moving nodes updates routes, minimap and collision immediately;", static_cast<int>(roads.Edges().size()));
			ImGui::TextDisabled("Save + reopen the scene to regenerate the district geometry.");
			ImGui::EndDisabled();
		}
		ImGui::EndChild();
	}

	static void DrawDriveTools(EditorHost& host)
	{
		GameSession* game = RacingGame(host);
		if (!game || !game->IsAvailable()) {
			ImGui::TextDisabled("No drivable car in this scene.");
			return;
		}
		DriveMode& d = game->Drive();
		if (host.IsPlaying()) {
			solace::Badge("RUNTIME", solace::Status::Warning);
			ImGui::SameLine();
			ImGui::TextDisabled("Changes made while playing are discarded on Stop unless applied.");
			ImGui::SameLine();
			if (ui::Btn("Apply to authored", true)) {
				host.ApplyRuntimeChangesToAuthored();
				ui::Toast("Runtime tuning applied to the scene (save to keep)", ui::ToastKind::Success);
			}
		}
		ImGui::Columns(3, "##drivecols", false);
		DriveMode::Handling& h = d.MutableHandling();
		ImGui::TextDisabled("HANDLING");
		ui::Grid("##hand");
		ui::Label("Top speed"); ImGui::DragFloat("##ts", &h.topSpeedKmh, 1.0f, 60.0f, 400.0f, "%.0f km/h");
		ui::Label("Acceleration"); ImGui::DragFloat("##ac", &h.acceleration, 0.1f, 2.0f, 30.0f, "%.1f m/s2");
		ui::Label("Braking"); ImGui::DragFloat("##br", &h.braking, 0.1f, 4.0f, 40.0f, "%.1f m/s2");
		ui::Label("Steer lock low"); ImGui::DragFloat("##sl", &h.steerLockLow, 0.2f, 10.0f, 50.0f, "%.1f deg");
		ui::Label("Steer lock high"); ImGui::DragFloat("##sh", &h.steerLockHigh, 0.1f, 1.0f, 20.0f, "%.1f deg");
		ui::Label("Steer rate"); ImGui::DragFloat("##sr", &h.steerRate, 0.1f, 1.0f, 30.0f);
		ui::Label("Grip"); ImGui::DragFloat("##gr", &h.grip, 0.05f, 0.5f, 20.0f);
		ui::Label("Drift grip"); ImGui::DragFloat("##dg", &h.driftGrip, 0.05f, 0.1f, 10.0f);
		ui::Label("Drift yaw gain"); ImGui::DragFloat("##dy", &h.driftYawGain, 0.02f, 1.0f, 4.0f);
		ui::Label("Redline"); ImGui::DragFloat("##rl", &h.rpmRedline, 10.0f, 4000.0f, 9800.0f, "%.0f rpm");
		ui::Label("Limiter"); ImGui::DragFloat("##lm", &h.rpmLimiter, 10.0f, 4500.0f, 10000.0f, "%.0f rpm");
		ui::EndGrid();
		ImGui::NextColumn();
		ImGui::TextDisabled("NITROUS");
		ui::Grid("##nos");
		ui::Label("Thrust x"); ImGui::DragFloat("##nt", &h.nitroThrust, 0.01f, 1.0f, 3.0f);
		ui::Label("Top speed x"); ImGui::DragFloat("##nv", &h.nitroTopSpeed, 0.01f, 1.0f, 1.5f);
		ui::Label("Drain /s"); ImGui::DragFloat("##nd", &h.nitroDrain, 0.005f, 0.02f, 1.0f);
		ui::Label("Recharge /s"); ImGui::DragFloat("##nr", &h.nitroRecharge, 0.005f, 0.0f, 1.0f);
		ui::Label("Recharge delay"); ImGui::DragFloat("##ny", &h.nitroRechargeDelay, 0.05f, 0.0f, 10.0f, "%.2f s");
		ui::EndGrid();
		ImGui::TextDisabled("SPAWN");
		glm::vec2 sp = d.SpawnPosition();
		float sh = glm::degrees(d.SpawnHeading());
		ui::Grid("##spawn");
		ui::Label("Position (x, z)");
		bool spawnChanged = ImGui::DragFloat2("##sp", &sp.x, 0.1f);
		ui::Label("Heading"); spawnChanged |= ImGui::DragFloat("##sph", &sh, 1.0f, -180.0f, 180.0f, "%.0f deg");
		ui::EndGrid();
		if (spawnChanged) { d.SetSpawn(sp, glm::radians(sh)); if (!host.IsPlaying()) d.ResetToSpawn(); }
		if (ui::Btn("Spawn = car", false)) { d.SetSpawn(d.Position(), d.Heading()); host.CommitGameEdit("Set spawn"); }
		ImGui::NextColumn();
		ImGui::TextDisabled("CHASE CAMERA");
		DriveMode::CameraRig& c = d.MutableCamera();
		ui::Grid("##cam");
		ui::Label("Distance"); ImGui::DragFloat("##cdist", &c.distance, 0.02f, 2.0f, 12.0f, "%.2f m");
		ui::Label("Height"); ImGui::DragFloat("##chei", &c.height, 0.01f, 0.4f, 5.0f, "%.2f m");
		ui::Label("Look ahead"); ImGui::DragFloat("##cla", &c.lookAhead, 0.05f, 0.0f, 20.0f, "%.2f m");
		ui::Label("Look height"); ImGui::DragFloat("##clh", &c.lookHeight, 0.01f, 0.0f, 3.0f, "%.2f m");
		ui::Label("FOV (vertical)"); ImGui::DragFloat("##cfov", &c.fov, 0.1f, 25.0f, 90.0f, "%.1f deg");
		ui::Label("FOV speed gain"); ImGui::DragFloat("##cfs", &c.fovSpeedGain, 0.1f, 0.0f, 30.0f);
		ui::Label("FOV nitrous"); ImGui::DragFloat("##cfn", &c.fovNitro, 0.1f, 0.0f, 20.0f);
		ui::Label("Pull back"); ImGui::DragFloat("##cpb", &c.pullBack, 0.01f, 0.0f, 4.0f, "%.2f m");
		ui::Label("Stiffness"); ImGui::DragFloat("##cst", &c.stiffness, 0.1f, 1.0f, 30.0f);
		ui::Label("Heading lag"); ImGui::DragFloat("##chl", &c.headingLag, 0.05f, 0.5f, 20.0f);
		ui::EndGrid();
		ImGui::TextDisabled("HEADLIGHTS");
		if (SceneGraph* graph = host.GetSceneGraph()) {
			for (const char* n : { "Headlight_L", "Headlight_R" }) {
				std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> find = [&](const std::shared_ptr<SceneNode>& s) -> std::shared_ptr<SceneNode> {
					if (!s) return nullptr;
					if (s->GetName() == n) return s;
					for (const auto& ch : s->children) if (auto f = find(ch)) return f;
					return nullptr;
				};
				if (auto ln = std::dynamic_pointer_cast<LightNode>(find(graph->GetRoot()))) {
					if (auto light = ln->GetLight()) {
						bool on = light->IsEnabled();
						float in = light->GetIntensity();
						ImGui::PushID(n);
						if (ImGui::Checkbox(n, &on)) { light->SetEnabled(on); host.MarkDirty(); }
						ImGui::SameLine();
						ImGui::SetNextItemWidth(-1);
						if (ImGui::DragFloat("##int", &in, 0.01f, 0.0f, 10.0f, "x%.2f")) { light->SetIntensity(in); host.MarkDirty(); }
						ImGui::PopID();
					}
				}
			}
		}
		ImGui::Columns(1);
		ImGui::Separator();
		ImGui::Text("Live: %.0f km/h  gear %d  %.0f rpm  load %.2f  N2O %.0f%%", d.SpeedKmh(), d.Gear(), d.Rpm(), d.Load(), d.Nitro() * 100.0f);
	}

	static void DrawHudTools(EditorHost& host)
	{
		ImGui::TextDisabled("Racing HUD and minimap (%s). Edits apply live in the Game view.", RacingHUD::Get().ConfigPath().c_str());
		RacingHUD::Get().DrawEditorPanel();
	}

	// Viewport overlay: road network, spawn, events and checkpoints with the route to each
	// checkpoint of the selected event. Handles are dragged on the ground plane (one undo step).
	void DrawRacingAuthoringOverlay(EditorHost& host, ImDrawList* dl)
	{
		GameSession* game = RacingGame(host);
		if (!game || !game->IsAvailable()) return;
		EditorHost* hostPtr = &host;
		const ImU32 roadCol = IM_COL32(120, 170, 255, 110);
		const RoadNetwork& roads = game->Roads();
		// Large generated networks (file-backed) are not drawn or edited as handles.
		static const std::vector<RoadNetwork::Edge> kNoEdges;
		for (const auto& e : roads.IsExternal() ? kNoEdges : roads.Edges()) {
			const glm::vec2 a = roads.Nodes()[e.a].pos, b = roads.Nodes()[e.b].pos;
			// Draw in short pieces so lines behind the camera are clipped correctly.
			const int pieces = std::max(1, static_cast<int>(glm::length(b - a) / 10.0f));
			for (int i = 0; i < pieces; ++i) {
				const glm::vec2 p0 = glm::mix(a, b, static_cast<float>(i) / pieces), p1 = glm::mix(a, b, static_cast<float>(i + 1) / pieces);
				ImVec2 s0, s1;
				if (host.ProjectToViewport(glm::vec3(p0.x, 0.05f, p0.y), s0) && host.ProjectToViewport(glm::vec3(p1.x, 0.05f, p1.y), s1)) dl->AddLine(s0, s1, roadCol, 1.5f);
			}
		}
		for (size_t i = 0; i < roads.Nodes().size() && !roads.IsExternal(); ++i) {
			const glm::vec2 p = roads.Nodes()[i].pos;
			ViewportHandle h;
			h.world = glm::vec3(p.x, 0.05f, p.y);
			h.color = roadCol;
			h.radius = 5.0f;
			h.onPress = [hostPtr] { g_selectedCheckpoint = -1; hostPtr->ShowPanel("race"); };
			h.onDrag = [game, i](const glm::vec2& q) { game->Roads().MutableNodes()[i].pos = q; };
			host.AddViewportHandle(h);
		}
		// Spawn.
		const glm::vec2 sp = game->Drive().SpawnPosition();
		{
			ViewportHandle h;
			h.world = glm::vec3(sp.x, 0.1f, sp.y);
			h.color = IM_COL32(90, 255, 140, 255);
			h.onPress = [hostPtr] { g_selectedCheckpoint = -1; hostPtr->ShowPanel("race"); };
			h.onDrag = [game](const glm::vec2& q) { game->Drive().SetSpawn(q, game->Drive().SpawnHeading()); game->Drive().ResetToSpawn(); };
			host.AddViewportHandle(h);
		}
		{
			const float hd = game->Drive().SpawnHeading();
			ImVec2 a, b;
			if (host.ProjectToViewport(glm::vec3(sp.x, 0.1f, sp.y), a) && host.ProjectToViewport(glm::vec3(sp.x + std::sin(hd) * 4.0f, 0.1f, sp.y + std::cos(hd) * 4.0f), b))
				dl->AddLine(a, b, IM_COL32(90, 255, 140, 255), 2.5f);
		}
		// Events, checkpoints and the route between them.
		auto& events = game->Events();
		for (size_t ei = 0; ei < events.size(); ++ei) {
			const auto& e = events[ei];
			const bool selected = static_cast<int>(ei) == game->SelectedEvent();
			const ImU32 evCol = IM_COL32(205, 160, 255, selected ? 255 : 150);
			{
				ViewportHandle h;
				h.world = glm::vec3(e.start.x, 0.1f, e.start.y);
				h.color = evCol;
				h.onPress = [hostPtr, game, ei] { game->SetSelectedEvent(static_cast<int>(ei)); g_selectedCheckpoint = -1; hostPtr->ShowPanel("race"); };
				h.onDrag = [game, ei](const glm::vec2& q) { game->Events()[ei].start = q; };
				host.AddViewportHandle(h);
			}
			ImVec2 s;
			if (host.ProjectToViewport(glm::vec3(e.start.x, 0.1f, e.start.y), s)) dl->AddText(ImVec2(s.x + 8, s.y - 18), evCol, e.name.c_str());
			glm::vec2 prev = e.start;
			for (size_t ci = 0; ci < e.checkpoints.size(); ++ci) {
				const auto& c = e.checkpoints[ci];
				const bool last = ci + 1 == e.checkpoints.size();
				const ImU32 col = last ? IM_COL32(255, 230, 90, selected ? 255 : 140) : IM_COL32(80, 220, 255, selected ? 255 : 140);
				{
					ViewportHandle h;
					h.world = glm::vec3(c.pos.x, 0.1f, c.pos.y);
					h.color = col;
					h.onPress = [hostPtr, game, ei, ci] { game->SetSelectedEvent(static_cast<int>(ei)); g_selectedCheckpoint = static_cast<int>(ci); hostPtr->ShowPanel("race"); };
					h.onDrag = [game, ei, ci](const glm::vec2& q) { game->Events()[ei].checkpoints[ci].pos = q; };
					host.AddViewportHandle(h);
				}
				if (selected) {
					const float hd = game->GateHeading(e, static_cast<int>(ci));
					const glm::vec2 side(std::cos(hd), -std::sin(hd));
					ImVec2 g0, g1, g0t, g1t;
					const glm::vec2 a = c.pos - side * c.width * 0.5f, b = c.pos + side * c.width * 0.5f;
					if (host.ProjectToViewport(glm::vec3(a.x, 0.1f, a.y), g0) && host.ProjectToViewport(glm::vec3(b.x, 0.1f, b.y), g1) &&
						host.ProjectToViewport(glm::vec3(a.x, 3.0f, a.y), g0t) && host.ProjectToViewport(glm::vec3(b.x, 3.0f, b.y), g1t)) {
						dl->AddLine(g0, g1, col, 2.0f);
						dl->AddLine(g0, g0t, col, 2.0f);
						dl->AddLine(g1, g1t, col, 2.0f);
						dl->AddLine(g0t, g1t, col, 1.0f);
						char num[16];
						std::snprintf(num, sizeof(num), last ? "FINISH" : "CP %d", static_cast<int>(ci) + 1);
						dl->AddText(ImVec2((g0t.x + g1t.x) * 0.5f - 12, (g0t.y + g1t.y) * 0.5f - 16), col, num);
					}
					const std::vector<glm::vec2> route = roads.FindRoute(prev, c.pos);
					for (size_t k = 1; k < route.size(); ++k) {
						const int pieces = std::max(1, static_cast<int>(glm::length(route[k] - route[k - 1]) / 8.0f));
						for (int i = 0; i < pieces; ++i) {
							const glm::vec2 p0 = glm::mix(route[k - 1], route[k], static_cast<float>(i) / pieces), p1 = glm::mix(route[k - 1], route[k], static_cast<float>(i + 1) / pieces);
							ImVec2 s0, s1;
							if (host.ProjectToViewport(glm::vec3(p0.x, 0.3f, p0.y), s0) && host.ProjectToViewport(glm::vec3(p1.x, 0.3f, p1.y), s1)) dl->AddLine(s0, s1, IM_COL32(205, 160, 255, 200), 3.0f);
						}
					}
				}
				prev = c.pos;
			}
		}
	}
	// Night Street: look development controls (RenderContext fields, live, not saved).
	void DrawNightStreetPanel(EditorHost& host)
	{
		ModularRenderer* renderer = host.GetRenderer();
		if (!renderer) {
			ImGui::TextDisabled("Renderer not connected");
			return;
		}
		ImGui::TextWrapped("Night-street look (docs/nfs2015). Values start from the scene's render_settings "
			"and the selected quality tier; changes here are live and not saved.");
		ImGui::Separator();
		DrawEngineLookSections(*renderer);   // the engine's Look controls (also in the Rendering window)
		ImGui::SliderFloat("Bloom strength", &renderer->GetContext().bloomStrength, 0.0f, 1.0f);
	}

}

namespace {
	// The native racing panels and overlay belong to the native GameSession module only (EditorPanel::available).
	bool NativeRacingActive(EditorHost& host)
	{
		return dynamic_cast<GameSession*>(host.Game()) != nullptr;
	}
}

void RegisterRacingEditorPanels()
{
	EditorExtensions::RegisterPanel({ "race", "Race & Routes", DrawRaceTools, true, NativeRacingActive });
	EditorExtensions::RegisterPanel({ "drive", "Drive", DrawDriveTools, true, NativeRacingActive });
	EditorExtensions::RegisterPanel({ "hud", "HUD", DrawHudTools, true, NativeRacingActive });
	EditorExtensions::RegisterPanel({ "night_street", "Night Street", DrawNightStreetPanel, true, NativeRacingActive });
	EditorExtensions::SetViewportOverlay([](EditorHost& host, ImDrawList* drawList) {
		if (NativeRacingActive(host)) DrawRacingAuthoringOverlay(host, drawList);
	});
	EditorExtensions::SetToolbarIconResolver([](const char* role) { return RacingHUD::Get().IconId(role); });
}
