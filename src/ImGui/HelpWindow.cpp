#include "HelpWindow.h"
#include <IMGUI/imgui.h>

HelpWindow::HelpWindow()
    : BaseWindow("Help & Controls", "F12")
{
    m_position = ImVec2(580, 10);
    m_size = ImVec2(350, 400);
}

void HelpWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "NOX Engine - Help & Controls");
    ImGui::Separator();
    
    if (ImGui::BeginTabBar("HelpTabs")) {
        
        // Window Controls Tab
        if (ImGui::BeginTabItem("Windows")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Window Controls:");
            ImGui::Separator();
            
            ImGui::Text("F1  - Engine Status");
            ImGui::Text("F2  - Camera Controls");
            ImGui::Text("F3  - Lighting System");
            ImGui::Text("F4  - Scene Hierarchy");
            ImGui::Text("F5  - Gizmo Controls");
            ImGui::Text("F6  - Rendering Settings");
            ImGui::Text("F7  - Performance Monitor");
            ImGui::Text("F12 - Help Window");
            
            ImGui::Separator();
            ImGui::Text("All windows can be:");
            ImGui::BulletText("Moved by dragging the title bar");
            ImGui::BulletText("Resized by dragging edges/corners");
            ImGui::BulletText("Collapsed by clicking the title");
            ImGui::BulletText("Toggled with function keys");
            
            ImGui::EndTabItem();
        }
        
        // Camera Controls Tab
        if (ImGui::BeginTabItem("Camera")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Camera Navigation:");
            ImGui::Separator();
            
            ImGui::Text("WASD     - Move camera");
            ImGui::Text("Mouse    - Look around (when locked)");
            ImGui::Text("Scroll   - Zoom in/out");
            ImGui::Text("Shift+Click - Toggle mouse lock");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Camera Settings:");
            ImGui::BulletText("Movement speed: Adjustable in Camera window (F2)");
            ImGui::BulletText("Mouse sensitivity: Adjustable in Camera window (F2)");
            ImGui::BulletText("Field of view: Camera-dependent");
            
            ImGui::EndTabItem();
        }
        
        // Gizmo Controls Tab
        if (ImGui::BeginTabItem("Gizmos")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "3D Gizmo Controls:");
            ImGui::Separator();
            
            ImGui::Text("Q - Translate mode");
            ImGui::Text("E - Rotate mode");
            ImGui::Text("R - Scale mode");
            ImGui::Text("G - Toggle gizmo visibility");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Object Selection:");
            ImGui::BulletText("Left-click objects to select");
            ImGui::BulletText("Use Scene Hierarchy (F4) for navigation");
            ImGui::BulletText("Selected objects show transform gizmo");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Transform Modes:");
            ImGui::BulletText("Local: Transform relative to object");
            ImGui::BulletText("World: Transform in world coordinates");
            ImGui::BulletText("Ctrl: Snap to grid (when implemented)");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.8f, 1.0f), "Note:");
            ImGui::BulletText("Gizmo renders as 3D overlay on selected objects");
            ImGui::BulletText("Mouse must be unlocked (Shift) for gizmo interaction");
            ImGui::BulletText("Use Gizmo window (F5) for detailed controls");
            
            ImGui::EndTabItem();
        }
        
        // Lighting Tab
        if (ImGui::BeginTabItem("Lighting")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Lighting System:");
            ImGui::Separator();
            
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Multi-Light Support:");
            ImGui::BulletText("Directional Lights (sun-like)");
            ImGui::BulletText("Point Lights (omnidirectional)");
            ImGui::BulletText("Spot Lights (cone-shaped)");
            ImGui::BulletText("Real-time shadow mapping");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Light Controls:");
            ImGui::BulletText("Color and intensity adjustment");
            ImGui::BulletText("Position and direction control");
            ImGui::BulletText("Shadow enable/disable");
            ImGui::BulletText("Attenuation parameters");
            
            ImGui::EndTabItem();
        }
        
        // Scene Management Tab
        if (ImGui::BeginTabItem("Scenes")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Scene Management:");
            ImGui::Separator();
            
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Scene Loading:");
            ImGui::BulletText("Use Status window (F1) scene selector");
            ImGui::BulletText("JSON-based scene configuration");
            ImGui::BulletText("glTF model support");
            ImGui::BulletText("Animation and physics support");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Scene Hierarchy:");
            ImGui::BulletText("Tree view of all scene nodes");
            ImGui::BulletText("Node type identification");
            ImGui::BulletText("Click nodes to select");
            ImGui::BulletText("Right-click for context menu");
            
            ImGui::EndTabItem();
        }
        
        // Performance Tab
        if (ImGui::BeginTabItem("Performance")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Performance Monitoring:");
            ImGui::Separator();
            
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Frame Rate:");
            ImGui::BulletText("Real-time FPS display");
            ImGui::BulletText("Frame time statistics");
            ImGui::BulletText("Performance classification");
            ImGui::BulletText("Frame time graph");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Rendering Stats:");
            ImGui::BulletText("Active light count");
            ImGui::BulletText("Shadow casting lights");
            ImGui::BulletText("Light type breakdown");
            ImGui::BulletText("Memory usage (when available)");
            
            ImGui::EndTabItem();
        }
        
        // About Tab
        if (ImGui::BeginTabItem("About")) {
            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "NOX Engine");
            ImGui::Separator();
            
            ImGui::Text("Modern OpenGL rendering engine with:");
            ImGui::BulletText("Deferred rendering pipeline");
            ImGui::BulletText("Multi-light shadow mapping");
            ImGui::BulletText("PBR material system");
            ImGui::BulletText("Animation support");
            ImGui::BulletText("Physics integration");
            ImGui::BulletText("ImGui-based editor interface");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Features:");
            ImGui::BulletText("glTF 2.0 model loading");
            ImGui::BulletText("Cascaded shadow maps");
            ImGui::BulletText("Temporal Anti-Aliasing (TAA)");
            ImGui::BulletText("Bloom post-processing");
            ImGui::BulletText("Real-time light manipulation");
            ImGui::BulletText("Scene graph visualization");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Built with:");
            ImGui::BulletText("OpenGL 4.5+");
            ImGui::BulletText("Dear ImGui");  
            ImGui::BulletText("ImGuizmo");
            ImGui::BulletText("glm mathematics");
            ImGui::BulletText("SDL2");
            
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    EndWindow();
}