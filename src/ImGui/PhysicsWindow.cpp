#include "PhysicsWindow.h"

#include "../EditorExtensions.h"
#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"
#include "../physics/PhysicsDebugView.h"
#include "../reflect/Reflection.h"

#include <IMGUI/imgui.h>
#include <algorithm>
#include <cstdio>
#include <glm/glm.hpp>
#include <vector>

namespace {

const char* BodyTypeName(RigidBody::BodyType type) {
	switch (type) {
	case RigidBody::BodyType::STATIC: return "Static";
	case RigidBody::BodyType::DYNAMIC: return "Dynamic";
	case RigidBody::BodyType::KINEMATIC: return "Kinematic";
	}
	return "?";
}

const char* ShapeName(RigidBody::ShapeType shape) {
	switch (shape) {
	case RigidBody::ShapeType::SPHERE: return "Sphere";
	case RigidBody::ShapeType::BOX: return "Box";
	case RigidBody::ShapeType::PLANE: return "Plane";
	case RigidBody::ShapeType::CAPSULE: return "Capsule";
	case RigidBody::ShapeType::MESH: return "Mesh";
	}
	return "?";
}

bool DragF(const char* id, float& v, float speed, float lo, float hi, const char* format) {
	ImGui::SetNextItemWidth(-1.0f);
	return ImGui::DragFloat(id, &v, speed, lo, hi, format);
}

bool DragI(const char* id, int& v, int lo, int hi) {
	ImGui::SetNextItemWidth(-1.0f);
	return ImGui::DragInt(id, &v, 1.0f, lo, hi);
}

// Hover hint for the rows whose value has a second unit.
void HintOnHover(const char* text) {
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void SetPair(SimulationConfig& cfg, int a, int b, bool allowed) {
	const uint32_t bitB = 1u << b;
	const uint32_t bitA = 1u << a;
	if (allowed) {
		cfg.layerMatrix[a] |= bitB;
		cfg.layerMatrix[b] |= bitA;
	}
	else {
		cfg.layerMatrix[a] &= ~bitB;
		cfg.layerMatrix[b] &= ~bitA;
	}
}

} // namespace

void PhysicsWindow::Apply(const Host& host, const SimulationConfig& cfg) {
	// Play mode: the runtime values only; Stop resets them to the authored values (Core::RequestStop).
	if (host.playing) host.physics->SetRuntimeConfig(cfg);
	else host.physics->SetConfig(cfg);
}

bool PhysicsWindow::Edit(const Host& host, const std::string& label, const std::function<void(SimulationConfig&)>& mutate) {
	if (!host.physics) return false;
	const SimulationConfig before = host.playing ? host.physics->GetConfig() : host.physics->GetAuthoredConfig();
	SimulationConfig after = before;
	mutate(after);
	after.Sanitize();
	Apply(host, after);
	if (host.markDirty) host.markDirty();
	PushEdit(host, label, before, after);
	return true;
}

void PhysicsWindow::PushEdit(const Host& host, const std::string& label, const SimulationConfig& before, const SimulationConfig& after) {
	if (!host.undo || before.ToJson() == after.ToJson()) return;
	std::shared_ptr<PhysicsEngine> physics = host.physics;
	std::function<void()> dirty = host.markDirty;
	host.undo(label,
		[physics, before, dirty] { physics->SetConfig(before); if (dirty) dirty(); },
		[physics, after, dirty] { physics->SetConfig(after); if (dirty) dirty(); });
}

void PhysicsWindow::Draw(const Host& host) {
	if (!host.physics) {
		ImGui::TextDisabled("No physics world. Open a scene to tune its physics.");
		return;
	}
	PhysicsEngine& physics = *host.physics;

	ImGui::TextDisabled("Saved per scene in its physics block. Defaults come from config/config.json.");
	if (ImGui::Button("Reset to defaults")) {
		Edit(host, "Reset physics settings", [&physics](SimulationConfig& c) { c = physics.GetDefaultConfig(); });
	}
	ImGui::Separator();

	// Edits go to a copy of the values in effect; any change applies live, and the whole edit becomes one undo step.
	const SimulationConfig start = host.playing ? physics.GetConfig() : physics.GetAuthoredConfig();
	SimulationConfig cfg = start;

	if (ui::Section("World", true)) DrawWorldSettings(cfg);
	if (ui::Section("Collision layers", false)) DrawLayers(cfg);
	if (ui::Section("Debug overlay (Scene view)", true)) DrawDebugToggles(physics);
	if (ui::Section("Live stats", true)) DrawStats(host);
	if (ui::Section("Selected body", true)) DrawSelectedBody(host);

	if (cfg.ToJson() != start.ToJson()) {
		cfg.Sanitize();
		if (!m_editPending) {
			m_editPending = true;
			m_editBefore = start;
		}
		Apply(host, cfg);
		if (host.markDirty) host.markDirty();
	}
	if (m_editPending && !ImGui::IsAnyItemActive()) {
		m_editPending = false;
		PushEdit(host, "Physics settings", m_editBefore, physics.GetAuthoredConfig());
	}
}

void PhysicsWindow::DrawWorldSettings(SimulationConfig& cfg) {
	ui::Grid("##physics_world");

	ui::Label("Gravity (m/s2)");
	{
		float g[3] = { cfg.gravityX, cfg.gravityY, cfg.gravityZ };
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3("##gravity", g, 0.05f, -1000.0f, 1000.0f, "%.2f")) {
			cfg.gravityX = g[0];
			cfg.gravityY = g[1];
			cfg.gravityZ = g[2];
		}
	}

	ui::Label("Fixed timestep (s)");
	DragF("##dt", cfg.fixedDeltaTime, 0.0001f, 0.001f, 0.1f, "%.5f");
	if (cfg.fixedDeltaTime > 0.0f) HintOnHover(("Step rate " + std::to_string(1.0f / cfg.fixedDeltaTime).substr(0, 6) + " Hz").c_str());

	ui::Label("Max substeps / frame");
	DragI("##substeps", cfg.maxSubSteps, 1, 64);

	ui::Label("Max accumulated time (s)");
	DragF("##maxacc", cfg.maxAccumulatedTime, 0.001f, 0.001f, 10.0f, "%.3f");
	HintOnHover("Frame time is clamped to this before it is accumulated (stops a spiral after a hitch).");

	ui::Label("Velocity iterations");
	DragI("##veliter", cfg.velocityIterations, 1, 100);

	ui::Label("Position iterations");
	DragI("##positer", cfg.positionIterations, 0, 100);

	ui::Label("Baumgarte factor");
	DragF("##baum", cfg.baumgarteFactor, 0.005f, 0.0f, 1.0f, "%.3f");

	ui::Label("Allowed penetration (m)");
	DragF("##slop", cfg.allowedPenetration, 0.0005f, 0.0f, 1.0f, "%.4f");

	ui::Label("Restitution threshold (m/s)");
	DragF("##restthr", cfg.restitutionThreshold, 0.05f, 0.0f, 1000.0f, "%.2f");

	ui::Label("Contact breaking (m)");
	DragF("##breakd", cfg.contactBreakingThreshold, 0.001f, 0.0f, 1.0f, "%.4f");

	ui::Label("Warm starting");
	DragF("##warm", cfg.warmStartingFactor, 0.005f, 0.0f, 1.0f, "%.3f");

	ui::Label("Max contact points");
	DragI("##maxcp", cfg.maxContactPoints, 1, 4);

	ui::Label("Sleeping");
	ImGui::Checkbox("##sleep", &cfg.enableSleeping);

	ui::Label("Sleep linear (m/s)");
	DragF("##slin", cfg.sleepLinearThreshold, 0.002f, 0.0f, 100.0f, "%.3f");

	ui::Label("Sleep angular (rad/s)");
	DragF("##sang", cfg.sleepAngularThreshold, 0.002f, 0.0f, 100.0f, "%.3f");

	ui::Label("Sleep time (s)");
	DragF("##stime", cfg.sleepTimeThreshold, 0.01f, 0.0f, 1000.0f, "%.2f");

	ui::Label("New body friction");
	DragF("##friction", cfg.defaultFriction, 0.005f, 0.0f, 1.0f, "%.3f");
	HintOnHover("Used by bodies created after the change (scene colliders without a friction key, Add Node). Existing bodies keep their value.");

	ui::Label("New body restitution");
	DragF("##restit", cfg.defaultRestitution, 0.005f, 0.0f, 1.0f, "%.3f");
	HintOnHover("Used by bodies created after the change. Existing bodies keep their value.");

	ui::Label("Fat AABB margin (m)");
	DragF("##fat", cfg.fatAABBMargin, 0.005f, 0.0f, 10.0f, "%.3f");

	ui::Label("AABB prediction (m)");
	DragF("##pred", cfg.aabbPrediction, 0.01f, 0.0f, 10.0f, "%.3f");

	ui::Label("Continuous collision");
	ImGui::TextDisabled("Not available: the engine has no CCD.");

	ui::EndGrid();
}

void PhysicsWindow::DrawLayers(SimulationConfig& cfg) {
	ImGui::TextDisabled("A pair collides only when some layer of one body and some layer of the other are allowed in both directions.");
	ImGui::TextDisabled("Bodies keep their own layer and mask on top of this matrix.");
	if (ImGui::Button("Allow all")) cfg.layerMatrix = SimulationConfig::AllLayersMatrix();
	ImGui::SameLine();
	if (ImGui::Button("Clear names")) cfg.layerNames = SimulationConfig::LayerNames{};

	ImGui::BeginChild("##layer_matrix", ImVec2(0.0f, 300.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
	if (ImGui::BeginTable("##layer_table", SimulationConfig::kLayerCount + 1, flags)) {
		ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		for (int j = 0; j < SimulationConfig::kLayerCount; ++j) {
			char label[8];
			std::snprintf(label, sizeof(label), "%d", j);
			ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, 22.0f);
		}
		ImGui::TableHeadersRow();
		for (int i = 0; i < SimulationConfig::kLayerCount; ++i) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(i);
			char name[64];
			std::snprintf(name, sizeof(name), "%s", cfg.layerNames[i].c_str());
			char hint[24];
			std::snprintf(hint, sizeof(hint), "Layer %d", i);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputTextWithHint("##name", hint, name, sizeof(name))) cfg.layerNames[i] = name;
			for (int j = 0; j < SimulationConfig::kLayerCount; ++j) {
				ImGui::TableSetColumnIndex(j + 1);
				ImGui::PushID(j);
				bool allowed = ((cfg.layerMatrix[i] >> j) & 1u) != 0;
				if (ImGui::Checkbox("##allow", &allowed)) SetPair(cfg, i, j, allowed);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();
}

void PhysicsWindow::DrawDebugToggles(PhysicsEngine& physics) {
	PhysicsDebugView& view = physics.DebugView();
	ImGui::Checkbox("Colliders", &view.colliders);
	ImGui::SameLine(170.0f);
	ImGui::Checkbox("AABBs", &view.aabbs);
	ImGui::SameLine(300.0f);
	ImGui::Checkbox("Contacts and normals", &view.contacts);
	ImGui::Checkbox("Sleeping bodies", &view.sleeping);
	ImGui::SameLine(170.0f);
	ImGui::Checkbox("Triggers", &view.triggers);
	ImGui::SameLine(300.0f);
	ImGui::Checkbox("Character capsules", &view.characters);
	ImGui::Checkbox("Velocity vectors", &view.velocities);
	ImGui::TextDisabled("Drawn over the Scene view only. Contacts appear while the simulation runs.");
}

void PhysicsWindow::SampleStepTime(float ms) {
	m_stepMs[m_sampleHead] = ms;
	m_sampleHead = (m_sampleHead + 1) % kSamples;
	m_sampleCount = std::min(m_sampleCount + 1, kSamples);
}

void PhysicsWindow::DrawStats(const Host& host) {
	PhysicsEngine& physics = *host.physics;
	const PhysicsEngine::ProfilingData& prof = physics.GetProfilingData();
	if (host.playing && !host.paused) SampleStepTime(prof.stepTimeMs);

	int total = 0, dynamicCount = 0, staticCount = 0, kinematicCount = 0;
	for (const auto& body : physics.GetBodies()) {
		if (!body) continue;
		++total;
		if (body->IsDynamic()) ++dynamicCount;
		else if (body->IsStatic()) ++staticCount;
		else if (body->IsKinematic()) ++kinematicCount;
	}
	int triggers = 0, characters = 0;
	physics.ForEachTrigger([&triggers](const SceneNode&, const TriggerSettings&) { ++triggers; });
	physics.ForEachCharacter([&characters](const SceneNode&, const CharacterSettings&) { ++characters; });

	ImGui::Text("Bodies %d  (dynamic %d, static %d, kinematic %d)", total, dynamicCount, staticCount, kinematicCount);
	ImGui::Text("Active %d   sleeping %d", prof.activeBodyCount, prof.sleepingBodyCount);
	ImGui::Text("Broadphase pairs %d   contacts %d   manifolds %d", prof.pairCount, prof.contactCount, prof.activeManifoldCount);
	ImGui::Text("Triggers %d   characters %d", triggers, characters);

	std::vector<float> ordered;
	ordered.reserve(static_cast<size_t>(m_sampleCount));
	for (int i = 0; i < m_sampleCount; ++i) {
		ordered.push_back(m_stepMs[(m_sampleHead - m_sampleCount + i + kSamples) % kSamples]);
	}
	if (ordered.empty()) {
		ImGui::TextDisabled("Step time: press Play to sample it (the last 240 frames are kept).");
		return;
	}
	float peak = 0.0f, sum = 0.0f;
	for (float v : ordered) {
		peak = std::max(peak, v);
		sum += v;
	}
	char overlay[64];
	std::snprintf(overlay, sizeof(overlay), "now %.3f ms  avg %.3f  max %.3f", ordered.back(), sum / static_cast<float>(ordered.size()), peak);
	ImGui::PlotLines("##step_ms", ordered.data(), static_cast<int>(ordered.size()), 0, overlay, 0.0f,
		std::max(0.5f, peak * 1.2f), ImVec2(-1.0f, 70.0f));
	if (!host.playing) ImGui::TextDisabled("Counts and step time are from the last simulated frame.");
}

void PhysicsWindow::DrawSelectedBody(const Host& host) {
	const std::shared_ptr<SceneNode>& node = host.selected;
	if (!node) {
		ImGui::TextDisabled("Select a node in the Hierarchy or the Scene view.");
		return;
	}
	PhysicsEngine& physics = *host.physics;
	const std::shared_ptr<RigidBody> rb = node->GetRigidBody();
	if (rb) {
		const glm::vec3 v = rb->getVelocity();
		const glm::vec3 w = rb->getAngularVelocity();
		ImGui::Text("%s %s   mass %.3f", BodyTypeName(rb->GetBodyType()), ShapeName(rb->getShapeType()), rb->getMass());
		ImGui::Text("Velocity (%.2f, %.2f, %.2f) m/s   speed %.2f", v.x, v.y, v.z, glm::length(v));
		ImGui::Text("Angular  (%.2f, %.2f, %.2f) rad/s", w.x, w.y, w.z);
		ImGui::Text("Sleeping %s   sleep timer %.2f s", rb->isSleeping() ? "yes" : "no", rb->getSleepTime());
		int contacts = 0;
		for (const ContactManifold* manifold : physics.GetActiveManifolds()) {
			if (manifold && (manifold->bodyA == rb || manifold->bodyB == rb)) contacts += manifold->pointCount;
		}
		ImGui::Text("Contacts %d", contacts);

		ImGui::BeginDisabled(!host.playing);
		if (ImGui::Button("Wake")) rb->wakeUp();
		ImGui::SameLine();
		if (ImGui::Button("Sleep")) rb->setSleeping(true);
		ImGui::SetNextItemWidth(-130.0f);
		ImGui::DragFloat3("##impulse", m_impulse, 0.1f, -1000.0f, 1000.0f, "%.2f");
		ImGui::SameLine();
		ImGui::BeginDisabled(!rb->IsDynamic());
		if (ImGui::Button("Apply impulse")) rb->applyImpulse(glm::vec3(m_impulse[0], m_impulse[1], m_impulse[2]));
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (!host.playing) ImGui::TextDisabled("Wake, Sleep and impulses act on the running simulation and are not saved.");
	}
	else {
		ImGui::TextDisabled("No RigidBody on this node. Add one in the Inspector to simulate it.");
	}

	// The node's physics components, edited with the Inspector widgets (mass, friction, restitution, damping, body
	// type = kinematic, shape and layers, trigger volume, character capsule). Each is drawn only when present.
	static const char* const kPhysicsComponents[] = { "RigidBody", "Collider", "Trigger", "CharacterController" };
	bool any = false;
	for (const char* name : kPhysicsComponents) {
		const nox::reflect::ComponentDesc* desc = nox::reflect::Registry::Get().Find(name);
		if (!desc || !desc->has || !desc->has(*node)) continue;
		any = true;
		if (host.drawComponent) host.drawComponent(node, *desc);
	}
	if (!any) ImGui::TextDisabled("This node has no physics components. Add a RigidBody, Trigger or CharacterController in the Inspector.");
}
