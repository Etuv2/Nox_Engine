#include "ImGuiInterface.h"
#include "ImGui/ImGuiWindowManager.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "ModularRenderer.h"
#include <IMGUI/imgui.h>
#include <IMGUI/ImGuizmo.h>
#include <iostream>

ImGuiInterface::ImGuiInterface() : m_windowManager(std::make_unique<ImGuiWindowManager>()) {
	if (!m_windowManager->Initialize()) {
		std::cerr << "[ImGuiInterface] Failed to initialize window manager!" << std::endl;
	}
}

ImGuiInterface::~ImGuiInterface() {
	if (m_windowManager) m_windowManager->Shutdown();
}

void ImGuiInterface::SetCamera(const std::shared_ptr<Camera>& camera) { if (m_windowManager) m_windowManager->SetCamera(camera); }
void ImGuiInterface::SetLighting(const std::shared_ptr<DirectionalLight>& lighting) { if (m_windowManager) m_windowManager->SetLighting(lighting); }
void ImGuiInterface::SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph) { if (m_windowManager) m_windowManager->SetSceneGraph(sceneGraph); }
void ImGuiInterface::SetRenderer(const std::shared_ptr<Renderer>& renderer) { if (m_windowManager) m_windowManager->SetRenderer(renderer); }
void ImGuiInterface::SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer) { if (m_windowManager) m_windowManager->SetModularRenderer(renderer); }
void ImGuiInterface::SetPhysicsEngine(const std::shared_ptr<class PhysicsEngine>& physicsEngine) { if (m_windowManager) m_windowManager->SetPhysicsEngine(physicsEngine); }
void ImGuiInterface::SetFrameData(const float* frameData, size_t count) { if (m_windowManager) m_windowManager->SetFrameTimeData(frameData, count); }

void ImGuiInterface::Render(int windowWidth, int windowHeight, float fps, std::string sceneName, glm::vec3 lightPos, glm::vec3 lightDir) {
	if (m_windowManager) m_windowManager->Render(windowWidth, windowHeight, fps, sceneName, lightPos, lightDir);
}

void ImGuiInterface::RenderGizmoOverlay(int windowWidth, int windowHeight) {
	if (m_windowManager) m_windowManager->RenderGizmoOverlay(windowWidth, windowHeight);
}

void ImGuiInterface::ProcessKeyboardInput() { if (m_windowManager) m_windowManager->ProcessKeyboardInput(); }

void ImGuiInterface::SetSelectedNode(const std::shared_ptr<SceneNode>& node) { if (m_windowManager) m_windowManager->SetSelectedNode(node); }
std::shared_ptr<SceneNode> ImGuiInterface::GetSelectedNode() const { return m_windowManager ? m_windowManager->GetSelectedNode() : nullptr; }
void ImGuiInterface::SetGizmoVisible(bool visible) { if (m_windowManager) m_windowManager->SetGizmoVisible(visible); }
bool ImGuiInterface::IsGizmoVisible() const { return m_windowManager ? m_windowManager->IsGizmoVisible() : false; }
void ImGuiInterface::SetGizmoOperation(int operation) { if (m_windowManager) m_windowManager->SetGizmoOperation(operation); }
int ImGuiInterface::GetGizmoOperation() const { return m_windowManager ? m_windowManager->GetGizmoOperation() : ImGuizmo::TRANSLATE; }
void ImGuiInterface::SetGizmoMode(int mode) { if (m_windowManager) m_windowManager->SetGizmoMode(mode); }
int ImGuiInterface::GetGizmoMode() const { return m_windowManager ? m_windowManager->GetGizmoMode() : ImGuizmo::LOCAL; }
bool ImGuiInterface::IsSnapEnabled() const { return m_windowManager ? m_windowManager->IsSnapEnabled() : false; }
float ImGuiInterface::GetTranslateSnap() const { return m_windowManager ? m_windowManager->GetTranslateSnap() : 0.5f; }
float ImGuiInterface::GetRotateSnap() const { return m_windowManager ? m_windowManager->GetRotateSnap() : 15.0f; }
float ImGuiInterface::GetScaleSnap() const { return m_windowManager ? m_windowManager->GetScaleSnap() : 0.1f; }
void ImGuiInterface::SetSceneSwapCallback(const std::function<void(const std::string&)>& callback) { if (m_windowManager) m_windowManager->SetSceneSwapCallback(callback); }
void ImGuiInterface::SetSceneList(const std::vector<std::string>& scenes) { if (m_windowManager) m_windowManager->SetSceneList(scenes); }
void ImGuiInterface::SetSceneSaveCallback(const std::function<void()>& callback) { if (m_windowManager) m_windowManager->SetSceneSaveCallback(callback); }
void ImGuiInterface::SetStateExportCallbacks(
	const std::function<bool(const std::string&)>& saveCallback,
	const std::function<bool(const std::string&)>& loadCallback,
	const std::function<bool()>& quickSaveCallback,
	const std::function<bool(const std::string&)>& exportCSVCallback,
	const std::function<bool(const std::string&)>& exportJSONCallback,
	const std::function<void()>& startRecordingCallback,
	const std::function<void()>& stopRecordingCallback) {
	if (m_windowManager) {
		m_windowManager->SetStateExportCallbacks(
			saveCallback, loadCallback, quickSaveCallback,
			exportCSVCallback, exportJSONCallback,
			startRecordingCallback, stopRecordingCallback
		);
	}
}
void ImGuiInterface::SetRecordingState(bool recording) { if (m_windowManager) m_windowManager->SetRecordingState(recording); }
void ImGuiInterface::ToggleWindow(const std::string& windowName) { if (m_windowManager) m_windowManager->ToggleWindow(windowName); }
void ImGuiInterface::SetWindowVisible(const std::string& windowName, bool visible) { if (m_windowManager) m_windowManager->SetWindowVisible(windowName, visible); }
bool ImGuiInterface::IsWindowVisible(const std::string& windowName) const { return m_windowManager ? m_windowManager->IsWindowVisible(windowName) : false; }
