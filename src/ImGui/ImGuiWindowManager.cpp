#include "ImGuiWindowManager.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../ModularRenderer.h"
#include "../LightNode.h"
#include "../AudioNode.h"
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

    // Populate window map
    m_windowMap["Status"] = m_statusWindow.get();
    m_windowMap["Camera"] = m_cameraWindow.get();
    m_windowMap["Lighting"] = m_lightingWindow.get();
    m_windowMap["Hierarchy"] = m_sceneHierarchyWindow.get();
    m_windowMap["Rendering"] = m_renderingSettingsWindow.get();
    m_windowMap["Performance"] = m_performanceWindow.get();
    m_windowMap["Help"] = m_helpWindow.get();

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
    ImGui::SetNextWindowPos(ImVec2(10, windowHeight - 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 50), ImGuiCond_Always);
    if (ImGui::Begin("F-Key Status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "F-Keys Active");
        ImGui::Text("F1-F7,F12: Windows");
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

    // Lightweight gizmo panel (not a BaseWindow, optional)
    if (m_showGizmoPanel) {
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::SetNextWindowPos(ImVec2(windowWidth * 0.5f - 140.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280, 155), ImGuiCond_Always);
        if (ImGui::Begin("Gizmo Panel", &m_showGizmoPanel,
            ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1.0f), "3D Gizmo");
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
            } else {
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

void ImGuiWindowManager::SetSelectedNode(const std::shared_ptr<SceneNode>& node) {
    m_selectedNode = node;
    m_sceneHierarchyWindow->SetSelectedNode(node);
}

void ImGuiWindowManager::SetFrameTimeData(const std::vector<float>& data) {
    m_performanceWindow->SetFrameTimeData(data);
}

// Scene management
void ImGuiWindowManager::SetSceneSwapCallback(const std::function<void(const std::string&)>& callback) {
    m_sceneSwapCallback = callback;
    m_statusWindow->SetSceneSwapCallback(callback);
}

void ImGuiWindowManager::SetSceneList(const std::vector<std::string>& scenes) {
    m_statusWindow->SetSceneList(scenes);
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

// Persistence
void ImGuiWindowManager::SaveWindowStates(const std::string& filename) {
    try {
        json j;
        for (const auto& [name, wnd] : m_windowMap) {
            if (!wnd) continue;
            json wd;
            wd["visible"] = wnd->IsVisible();
            wd["position"] = { wnd->GetPosition().x, wnd->GetPosition().y };
            wd["size"] = { wnd->GetSize().x, wnd->GetSize().y };
            j[name] = wd;
        }
        j["gizmo"] = {
            {"panel_visible", m_showGizmoPanel},
            {"visible", m_gizmoVisible},
            {"operation", m_gizmoOperation},
            {"mode", m_gizmoMode},
            {"snap_enabled", m_snapEnabled},
            {"translate_snap", m_translateSnap},
            {"rotate_snap", m_rotateSnap},
            {"scale_snap", m_scaleSnap}
        };
        std::ofstream f(filename);
        if (f.is_open()) f << j.dump(2);
    } catch (...) {
        std::cerr << "[ImGuiWindowManager] Failed saving layout" << std::endl;
    }
}

void ImGuiWindowManager::LoadWindowStates(const std::string& filename) {
    try {
        std::ifstream f(filename);
        if (!f.is_open()) return;
        json j; f >> j;
        for (auto& [name, wnd] : m_windowMap) {
            if (!wnd || !j.contains(name)) continue;
            auto& wd = j[name];
            if (wd.contains("visible")) wnd->SetVisible(wd["visible"]);
            if (wd.contains("position") && wd["position"].size()==2) wnd->SetPosition(ImVec2(wd["position"][0], wd["position"][1]));
            if (wd.contains("size") && wd["size"].size()==2) wnd->SetSize(ImVec2(wd["size"][0], wd["size"][1]));
        }
        if (j.contains("gizmo")) {
            auto& g = j["gizmo"];
            if (g.contains("panel_visible")) m_showGizmoPanel = g["panel_visible"];
            if (g.contains("visible")) m_gizmoVisible = g["visible"];
            if (g.contains("operation")) m_gizmoOperation = g["operation"];
            if (g.contains("mode")) m_gizmoMode = g["mode"];
            if (g.contains("snap_enabled")) m_snapEnabled = g["snap_enabled"];
            if (g.contains("translate_snap")) m_translateSnap = g["translate_snap"];
            if (g.contains("rotate_snap")) m_rotateSnap = g["rotate_snap"];
            if (g.contains("scale_snap")) m_scaleSnap = g["scale_snap"];
        }
    } catch (...) {
        std::cerr << "[ImGuiWindowManager] Failed loading layout" << std::endl;
    }
}

void ImGuiWindowManager::RenderGizmoOverlay(int windowWidth, int windowHeight) {
    if (!m_camera || !m_selectedNode || !m_gizmoVisible) return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(false);

    glm::mat4 parentWorld(1.0f);
    if (auto p = m_selectedNode->parentNode.lock()) {
        parentWorld = p->GetGlobalTransform(glm::mat4(1.0f));
    }
    glm::mat4 world = m_selectedNode->GetGlobalTransform(glm::mat4(1.0f));

    glm::mat4 view = m_camera->GetViewMatrix();
    glm::mat4 proj = m_camera->GetProjectionMatrix();

    float viewM[16]; float projM[16]; float worldM[16];
    memcpy(viewM, glm::value_ptr(view), sizeof(float)*16);
    memcpy(projM, glm::value_ptr(proj), sizeof(float)*16);
    memcpy(worldM, glm::value_ptr(world), sizeof(float)*16);

    ImGuizmo::OPERATION op = static_cast<ImGuizmo::OPERATION>(m_gizmoOperation);
    ImGuizmo::MODE mode = static_cast<ImGuizmo::MODE>(m_gizmoMode);

    // Removed: previous restriction forcing AudioNode to TRANSLATE only.

    // Stable ID so multiple gizmos (future) don't conflict
    ImGuizmo::SetID(static_cast<int>(reinterpret_cast<uintptr_t>(m_selectedNode.get()) & 0x7FFFFFFF));

    float snap[3] = {0,0,0};
    const float* pSnap = nullptr;
    if (m_snapEnabled || io.KeyCtrl) {
        switch (op) {
            case ImGuizmo::TRANSLATE: snap[0]=snap[1]=snap[2]=m_translateSnap; break;
            case ImGuizmo::ROTATE:    snap[0]=snap[1]=snap[2]=m_rotateSnap; break;
            case ImGuizmo::SCALE:     snap[0]=snap[1]=snap[2]=m_scaleSnap; break;
            default: break;
        }
        pSnap = snap;
    }

    ImGuizmo::Enable(true);
    ImGuizmo::AllowAxisFlip(true);
    ImGuizmo::SetGizmoSizeClipSpace(0.12f);

    bool manipulated = ImGuizmo::Manipulate(viewM, projM, op, mode, worldM, nullptr, pSnap);

    if (manipulated) {
        auto isFiniteMat4 = [](const glm::mat4& m){
            for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(m[c][r])) return false; return true; };
        auto isFiniteVec3 = [](const glm::vec3& v){ return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); };
        glm::mat4 newWorld = glm::make_mat4(worldM);

        // FIXED: Get proper parent world transform from hierarchy system
        glm::mat4 parentInv(1.0f);
        if (auto parent = m_selectedNode->parentNode.lock()) {
            glm::mat4 parentWorld = parent->GetWorldPosition4x4();
            float det = glm::determinant(parentWorld);
            if (std::abs(det) > 1e-8f) {
                parentInv = glm::inverse(parentWorld);
            }
        }

        glm::mat4 newLocal = parentInv * newWorld;
        if (!isFiniteMat4(newLocal)) {
            // Fallback: keep previous local, only update translation safely
            glm::vec3 worldPos = glm::vec3(newWorld[3]);
            glm::vec3 localPos = glm::vec3(parentInv * glm::vec4(worldPos,1.0f));
            if (isFiniteVec3(localPos)) {
                glm::mat4 prevLocal = m_selectedNode->GetTransform();
                prevLocal[3] = glm::vec4(localPos,1.0f);
                newLocal = prevLocal;
            } else {
                // Abort completely if still invalid
                return;
            }
        }

        if (auto lightNode = std::dynamic_pointer_cast<LightNode>(m_selectedNode)) {
            if (!isFiniteMat4(newLocal)) return; // safety
            lightNode->SetTransform(newLocal);
            lightNode->UpdateLightFromTransform(true);
            lightNode->UpdateSelectionProxy();
        } else {
            glm::vec3 scale; glm::quat rot; glm::vec3 translation; glm::vec3 skew; glm::vec4 persp;
            if (glm::decompose(newLocal, scale, rot, translation, skew, persp) && isFiniteVec3(scale) && isFiniteVec3(translation)) {
                // Sanitize scale (avoid zeros / extreme values leading to singular parent matrices later)
                const float MIN_SCALE = 1e-4f;
                scale = glm::max(scale, glm::vec3(MIN_SCALE));
                rot = glm::normalize(rot);
                if (!isFiniteVec3(glm::vec3(rot.x,rot.y,rot.z))) rot = glm::quat(1,0,0,0);
                m_selectedNode->SetLocalTRS(translation, rot, scale);

                if (auto audioNode = std::dynamic_pointer_cast<AudioNode>(m_selectedNode); audioNode && m_camera) {
                    // The audio position will be automatically updated during the next UpdateAudioNodes call
                    // since AudioNode now uses the unified hierarchy system
                    audioNode->UpdateAudioNodes(m_camera->GetCameraPosition(), m_camera->GetCameraFacingAngle());
                }
            } else {
                // If decompose failed, fallback to translation-only update
                glm::vec3 worldPos = glm::vec3(newWorld[3]);
                glm::vec3 localPos = glm::vec3(parentInv * glm::vec4(worldPos,1.0f));
                if (isFiniteVec3(localPos)) {
                    glm::mat4 prevLocal = m_selectedNode->GetTransform();
                    prevLocal[3] = glm::vec4(localPos,1.0f);
                    m_selectedNode->SetTransform(prevLocal);
                    if (auto audioNode = std::dynamic_pointer_cast<AudioNode>(m_selectedNode); audioNode && m_camera) {
                        audioNode->UpdateAudioNodes(m_camera->GetCameraPosition(), m_camera->GetCameraFacingAngle());
                    }
                }
            }
        }
    }
}