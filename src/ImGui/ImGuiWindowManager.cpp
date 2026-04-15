#include "ImGuiWindowManager.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../ModularRenderer.h"
#include "../PhysicsEngine.h"
#include "../LightNode.h"
#include "../AudioNode.h"
#include "StateExportWindow.h"
#include "AnimationWindow.h"
#include <IMGUI/imgui.h>
#include <IMGUI/ImGuizmo.h>
#include <iostream>
#include <algorithm>
#include <fstream>
#include "../json.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

using json = nlohmann::json;

ImGuiWindowManager::ImGuiWindowManager()
{
	// Initialize windows
	m_statusWindow = std::make_unique<StatusWindow>();
	m_cameraWindow = std::make_unique<CameraWindow>();
	m_lightingWindow = std::make_unique<LightingWindow>();
	m_sceneHierarchyWindow = std::make_unique<SceneHierarchyWindow>();
	m_renderingSettingsWindow = std::make_unique<RenderingSettingsWindow>();
	m_performanceWindow = std::make_unique<PerformanceWindow>();
	m_helpWindow = std::make_unique<HelpWindow>();
	m_stateExportWindow = std::make_unique<StateExportWindow>();
	m_animationWindow = std::make_unique<AnimationWindow>();

	// Populate window map
	m_windowMap["Status"] = m_statusWindow.get();
	m_windowMap["Camera"] = m_cameraWindow.get();
	m_windowMap["Lighting"] = m_lightingWindow.get();
	m_windowMap["Hierarchy"] = m_sceneHierarchyWindow.get();
	m_windowMap["Rendering"] = m_renderingSettingsWindow.get();
	m_windowMap["Performance"] = m_performanceWindow.get();
	m_windowMap["Help"] = m_helpWindow.get();
	m_windowMap["StateExport"] = m_stateExportWindow.get();
	m_windowMap["Animation"] = m_animationWindow.get();

	// Scene hierarchy selection callback
	m_sceneHierarchyWindow->SetSelectionCallback([this](std::shared_ptr<SceneNode> node) {
		SetSelectedNode(node);
		});

	// Window toggle callback from status window
	m_statusWindow->SetWindowToggleCallback([this](const std::string& windowName) {
		ToggleWindow(windowName);
		});
}

ImGuiWindowManager::~ImGuiWindowManager() = default;

bool ImGuiWindowManager::Initialize() {
	std::cout << "[ImGuiWindowManager] Initialized window system (gizmo overlay mode)" << std::endl;
	LoadWindowStates();
	return true;
}

void ImGuiWindowManager::Shutdown() {
	SaveWindowStates();
}

void ImGuiWindowManager::Render(int windowWidth, int windowHeight, float fps,
	const std::string& sceneName, glm::vec3 lightPos, glm::vec3 lightDir) {
	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;

	// Update windows
	m_statusWindow->SetFPS(fps);
	m_statusWindow->SetSceneName(sceneName);
	m_statusWindow->SetLightPosition(lightPos);
	m_statusWindow->SetLightDirection(lightDir);
	m_statusWindow->SetWindowDimensions(windowWidth, windowHeight);

	m_lightingWindow->SetLightPosition(lightPos);
	m_lightingWindow->SetLightDirection(lightDir);
	m_performanceWindow->SetFPS(fps);

	// F-key hint window
	ImGui::SetNextWindowPos(ImVec2(10, static_cast<float>(windowHeight) - 60), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(220, 50), ImGuiCond_Always);
	if (ImGui::Begin("F-Key Status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
		ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "F-Keys Active");
		ImGui::Text("F1-F9,F12: Windows | F8:Anim");
	}
	ImGui::End();

	// Render windows
	m_statusWindow->Render();
	m_cameraWindow->Render();
	m_lightingWindow->Render();
	m_sceneHierarchyWindow->Render();
	m_renderingSettingsWindow->Render();
	m_performanceWindow->Render();
	m_helpWindow->Render();
	m_stateExportWindow->Render();
	m_animationWindow->Render();

	// Lightweight gizmo panel (not a BaseWindow, optional)
	if (m_showGizmoPanel) {
		ImGui::SetNextWindowBgAlpha(0.9f);
		ImGui::SetNextWindowPos(ImVec2(windowWidth * 0.5f - 140.0f, 10.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(280, 155), ImGuiCond_Always);
		if (ImGui::Begin("Gizmo Panel", &m_showGizmoPanel,
			ImGuiWindowFlags_NoCollapse)) {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "3D Gizmo");
			if (m_selectedNode) {
				ImGui::Checkbox("Visible", &m_gizmoVisible);
				ImGui::SameLine();
				if (ImGui::Button("Focus") && m_selectedNode) {
					// Optional: could implement camera focus
				}
				ImGui::Separator();
				if (ImGui::RadioButton("T", m_gizmoOperation == ImGuizmo::TRANSLATE)) m_gizmoOperation = ImGuizmo::TRANSLATE;
				ImGui::SameLine();
				if (ImGui::RadioButton("R", m_gizmoOperation == ImGuizmo::ROTATE)) m_gizmoOperation = ImGuizmo::ROTATE;
				ImGui::SameLine();
				if (ImGui::RadioButton("S", m_gizmoOperation == ImGuizmo::SCALE)) m_gizmoOperation = ImGuizmo::SCALE;
				if (m_gizmoOperation != ImGuizmo::SCALE) {
					if (ImGui::RadioButton("Local", m_gizmoMode == ImGuizmo::LOCAL)) m_gizmoMode = ImGuizmo::LOCAL;
					ImGui::SameLine();
					if (ImGui::RadioButton("World", m_gizmoMode == ImGuizmo::WORLD)) m_gizmoMode = ImGuizmo::WORLD;
				}
				ImGui::Separator();
				ImGui::Checkbox("Snap", &m_snapEnabled);
				if (m_snapEnabled) {
					if (m_gizmoOperation == ImGuizmo::TRANSLATE)
						ImGui::DragFloat("Move", &m_translateSnap, 0.1f, 0.01f, 50.0f, "%.2f");
					else if (m_gizmoOperation == ImGuizmo::ROTATE)
						ImGui::DragFloat("Rotate", &m_rotateSnap, 1.0f, 1.0f, 180.0f, "%.0f");
					else if (m_gizmoOperation == ImGuizmo::SCALE)
						ImGui::DragFloat("Scale", &m_scaleSnap, 0.01f, 0.01f, 10.0f, "%.2f");
				}
			}
			else {
				ImGui::TextDisabled("No selection");
			}
		}
		ImGui::End();
	}
}

void ImGuiWindowManager::ProcessKeyboardInput() {
	ImGuiIO& io = ImGui::GetIO();

	// Function keys always
	if (ImGui::IsKeyPressed(ImGuiKey_F1)) ToggleWindow("Status");
	if (ImGui::IsKeyPressed(ImGuiKey_F2)) ToggleWindow("Camera");
	if (ImGui::IsKeyPressed(ImGuiKey_F3)) ToggleWindow("Lighting");
	if (ImGui::IsKeyPressed(ImGuiKey_F4)) ToggleWindow("Hierarchy");
	if (ImGui::IsKeyPressed(ImGuiKey_F5)) m_showGizmoPanel = !m_showGizmoPanel; // now toggles panel only
	if (ImGui::IsKeyPressed(ImGuiKey_F6)) ToggleWindow("Rendering");
	if (ImGui::IsKeyPressed(ImGuiKey_F7)) ToggleWindow("Performance");
	if (ImGui::IsKeyPressed(ImGuiKey_F8)) ToggleWindow("Animation");
	if (ImGui::IsKeyPressed(ImGuiKey_F9)) ToggleWindow("StateExport");
	if (ImGui::IsKeyPressed(ImGuiKey_F12)) ToggleWindow("Help");

	if (io.WantCaptureKeyboard) return; // avoid interfering with text inputs

	if (m_selectedNode && m_gizmoVisible) {
		if (ImGui::IsKeyPressed(ImGuiKey_Q)) m_gizmoOperation = ImGuizmo::TRANSLATE;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOperation = ImGuizmo::ROTATE;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOperation = ImGuizmo::SCALE;
		if (ImGui::IsKeyPressed(ImGuiKey_G)) m_gizmoVisible = !m_gizmoVisible;
	}
}

// Data setters
void ImGuiWindowManager::SetCamera(const std::shared_ptr<Camera>& camera) {
	m_camera = camera;
	m_cameraWindow->SetCamera(camera);
}

void ImGuiWindowManager::SetLighting(const std::shared_ptr<DirectionalLight>& lighting) {
	m_lighting = lighting;
	m_lightingWindow->SetLighting(lighting);
}

void ImGuiWindowManager::SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph) {
	m_sceneGraph = sceneGraph;
	m_lightingWindow->SetSceneGraph(sceneGraph);
	m_sceneHierarchyWindow->SetSceneGraph(sceneGraph);
	m_performanceWindow->SetSceneGraph(sceneGraph);
	m_animationWindow->SetSceneGraph(sceneGraph);
}

void ImGuiWindowManager::SetRenderer(const std::shared_ptr<Renderer>& renderer) {
	m_renderer = renderer;
	m_renderingSettingsWindow->SetRenderer(renderer);
}

void ImGuiWindowManager::SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer) {
	// Pass ModularRenderer to RenderingSettingsWindow for real-time control
	if (m_renderingSettingsWindow) {
		m_renderingSettingsWindow->SetModularRenderer(renderer);
		std::cout << "[ImGuiWindowManager] Connected ModularRenderer to RenderingSettingsWindow" << std::endl;
	}
}

void ImGuiWindowManager::SetPhysicsEngine(const std::shared_ptr<class PhysicsEngine>& physicsEngine) {
	m_physicsEngine = physicsEngine;
}

void ImGuiWindowManager::SetSelectedNode(const std::shared_ptr<SceneNode>& node) {
	if (m_selectedNode != node) {
		if (auto previousNode = m_lastManipulatedNode.lock()) {
			if (auto rb = previousNode->GetRigidBody()) {
				if (m_physicsEngine && rb->isGizmoGrabbed()) {
					m_physicsEngine->EndGizmoGrab(rb);
				}
			}
			m_lastManipulatedNode.reset();
			m_wasManipulatingGizmo = false;
		}
	}

	m_selectedNode = node;
	m_sceneHierarchyWindow->SetSelectedNode(node);
	m_animationWindow->SetSelectedNode(node);
}

void ImGuiWindowManager::SetFrameTimeData(const float* data, size_t count) {
	// Create a vector view from the data without copying
	std::vector<float> frameDataView(data, data + count);
	m_performanceWindow->SetFrameTimeData(frameDataView);
}

// Scene management
void ImGuiWindowManager::SetSceneSwapCallback(const std::function<void(const std::string&)>& callback) {
	m_sceneSwapCallback = callback;
	m_statusWindow->SetSceneSwapCallback(callback);
}

void ImGuiWindowManager::SetSceneSaveCallback(const std::function<void()>& callback) {
	m_sceneSaveCallback = callback;
	m_statusWindow->SetSceneSaveCallback(callback);
}

void ImGuiWindowManager::SetSceneList(const std::vector<std::string>& scenes) {
	m_statusWindow->SetSceneList(scenes);
}

// State export callbacks
void ImGuiWindowManager::SetStateExportCallbacks(
	const std::function<bool(const std::string&)>& saveCallback,
	const std::function<bool(const std::string&)>& loadCallback,
	const std::function<bool()>& quickSaveCallback,
	const std::function<bool(const std::string&)>& exportCSVCallback,
	const std::function<bool(const std::string&)>& exportJSONCallback,
	const std::function<void()>& startRecordingCallback,
	const std::function<void()>& stopRecordingCallback) {

	if (m_stateExportWindow) {
		m_stateExportWindow->SetSaveSceneStateCallback(saveCallback);
		m_stateExportWindow->SetLoadSceneStateCallback(loadCallback);
		m_stateExportWindow->SetQuickSaveCallback(quickSaveCallback);
		m_stateExportWindow->SetExportCSVCallback(exportCSVCallback);
		m_stateExportWindow->SetExportJSONCallback(exportJSONCallback);
		m_stateExportWindow->SetStartRecordingCallback(startRecordingCallback);
		m_stateExportWindow->SetStopRecordingCallback(stopRecordingCallback);
	}
}

void ImGuiWindowManager::SetRecordingState(bool recording) {
	if (m_stateExportWindow) {
		m_stateExportWindow->SetRecording(recording);
	}
}

// Window controls
void ImGuiWindowManager::ToggleWindow(const std::string& windowName) {
	auto it = m_windowMap.find(windowName);
	if (it != m_windowMap.end() && it->second) {
		bool current = it->second->IsVisible();
		it->second->SetVisible(!current);
	}
}

void ImGuiWindowManager::SetWindowVisible(const std::string& windowName, bool visible) {
	auto it = m_windowMap.find(windowName);
	if (it != m_windowMap.end() && it->second) it->second->SetVisible(visible);
}

bool ImGuiWindowManager::IsWindowVisible(const std::string& windowName) const {
	auto it = m_windowMap.find(windowName);
	return (it != m_windowMap.end() && it->second) ? it->second->IsVisible() : false;
}

// Gizmo overlay rendering - renders as true viewport overlay, not in a window
void ImGuiWindowManager::RenderGizmoOverlay(int windowWidth, int windowHeight) {
	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;

	// Always call BeginFrame to reset gizmo state for this frame
	ImGuizmo::BeginFrame();

	// Check if gizmo manipulation just ended - release kinematic mode
	bool isCurrentlyManipulating = ImGuizmo::IsUsing();
	if (m_wasManipulatingGizmo && !isCurrentlyManipulating) {
		std::cout << "[ImGuiWindowManager] Gizmo released - attempting EndGizmoGrab" << std::endl;

		// Gizmo was released - restore dynamic body behavior through physics engine
		if (auto lastNode = m_lastManipulatedNode.lock()) {
			if (auto rb = lastNode->GetRigidBody()) {
				if (rb->isGizmoGrabbed() && m_physicsEngine) {
					m_physicsEngine->EndGizmoGrab(rb);
					std::cout << "[ImGuiWindowManager] Ended gizmo grab on node: " << lastNode->GetName() << std::endl;
				}
			}
			m_lastManipulatedNode.reset();
		}

	}

	if (!m_selectedNode || !m_gizmoVisible || !m_camera) {
		if (auto lastNode = m_lastManipulatedNode.lock()) {
			if (auto rb = lastNode->GetRigidBody()) {
				if (m_physicsEngine && rb->isGizmoGrabbed()) {
					m_physicsEngine->EndGizmoGrab(rb);
				}
			}
			m_lastManipulatedNode.reset();
		}
		m_wasManipulatingGizmo = false;
		return;
	}

	// Configure gizmo for perspective rendering
	ImGuizmo::SetOrthographic(false);

	// Use the foreground draw list for true overlay rendering
	ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

	// Set the gizmo rect to cover the entire viewport
	ImGuizmo::SetRect(0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight));

	glm::mat4 view = m_camera->GetViewMatrix();
	glm::mat4 projection = m_camera->GetProjectionMatrix();

	// Use WORLD transform for gizmo manipulation
	// This ensures physics bodies are properly positioned
	glm::mat4 model = m_selectedNode->GetWorldPosition4x4();

	// Configure snap values based on current operation
	float snapValue = 0.0f;
	if (m_snapEnabled) {
		if (m_gizmoOperation == ImGuizmo::TRANSLATE) snapValue = m_translateSnap;
		else if (m_gizmoOperation == ImGuizmo::ROTATE) snapValue = m_rotateSnap;
		else if (m_gizmoOperation == ImGuizmo::SCALE) snapValue = m_scaleSnap;
	}
	float snapArray[3] = { snapValue, snapValue, snapValue };
	float* snap = m_snapEnabled ? snapArray : nullptr;

	// Render and manipulate the gizmo
	ImGuizmo::Manipulate(
		glm::value_ptr(view),
		glm::value_ptr(projection),
		static_cast<ImGuizmo::OPERATION>(m_gizmoOperation),
		static_cast<ImGuizmo::MODE>(m_gizmoMode),
		glm::value_ptr(model),
		nullptr,
		snap
	);

	// Apply transform changes when user is manipulating the gizmo
	if (ImGuizmo::IsUsing()) {
		// Apply the manipulated WORLD transform through the ECS authority.
		// This keeps deep imported hierarchies coherent and lets child entities
		// follow their authored parents instead of round-tripping through a second
		// local-transform write path in the editor.
		m_selectedNode->SetWorldTransform(model);

		// Handle physics interaction if body exists
		if (auto rb = m_selectedNode->GetRigidBody()) {
			if (!rb->isGizmoGrabbed()) {
				// First frame of manipulation - grab the body
				if (m_physicsEngine) {
					m_physicsEngine->BeginGizmoGrab(rb);
				}
				m_lastManipulatedNode = m_selectedNode;
			}

			// Update gizmo target in physics engine
			if (m_physicsEngine) {
				glm::vec3 gizmoPos = glm::vec3(model[3]);
				glm::quat gizmoRot = glm::quat_cast(glm::mat3(model));
				m_physicsEngine->UpdateGizmoTarget(rb, gizmoPos, gizmoRot);
			}
		}
		else {
			// No rigid body - just track that we're manipulating for UI feedback
			m_lastManipulatedNode = m_selectedNode;
		}
	}

	// Update manipulation tracking
	m_wasManipulatingGizmo = ImGuizmo::IsUsing();
}

// Window state persistence
void ImGuiWindowManager::SaveWindowStates(const std::string& filename) {
	try {
		json j;
		for (const auto& [name, wnd] : m_windowMap) {
			if (!wnd) continue;
			json wd;
			wd["visible"] = wnd->IsVisible();
			j[name] = wd;
		}

		std::ofstream outFile(filename);
		if (outFile.is_open()) {
			outFile << j.dump(2);
			outFile.close();
			std::cout << "[ImGuiWindowManager] Window states saved to " << filename << std::endl;
		}
	}
	catch (const std::exception& e) {
		std::cerr << "[ImGuiWindowManager] Error saving window states: " << e.what() << std::endl;
	}
}

void ImGuiWindowManager::LoadWindowStates(const std::string& filename) {
	try {
		std::ifstream inFile(filename);
		if (!inFile.is_open()) {
			std::cout << "[ImGuiWindowManager] No saved window states found at " << filename << std::endl;
			return;
		}

		json j;
		inFile >> j;
		inFile.close();

		for (const auto& [name, data] : j.items()) {
			auto it = m_windowMap.find(name);
			if (it != m_windowMap.end() && it->second) {
				if (data.contains("visible")) {
					it->second->SetVisible(data["visible"]);
				}
			}
		}

		std::cout << "[ImGuiWindowManager] Window states loaded from " << filename << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "[ImGuiWindowManager] Error loading window states: " << e.what() << std::endl;
	}
}

void ImGuiWindowManager::OnSceneChanged()
{
	// Notify animation window of scene change
	if (m_animationWindow) {
		m_animationWindow->OnSceneChanged();
	}
	
	// Clear selected node
	m_selectedNode.reset();
	m_sceneHierarchyWindow->SetSelectedNode(nullptr);
}
