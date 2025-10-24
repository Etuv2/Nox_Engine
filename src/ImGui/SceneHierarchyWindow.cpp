#include "SceneHierarchyWindow.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include <IMGUI/imgui.h>
#include <iostream>

SceneHierarchyWindow::SceneHierarchyWindow()
    : BaseWindow("Scene Hierarchy", "F4")
{
    m_position = ImVec2(10, 580);
    m_size = ImVec2(300, 280);
}

void SceneHierarchyWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Scene Graph");
    ImGui::Separator();
    
    if (m_sceneGraph && m_sceneGraph->GetRoot()) {
        // Scene statistics
        int nodeCount = CountNodes(m_sceneGraph->GetRoot());
        ImGui::Text("Scene: %s", m_sceneGraph->GetSceneName().c_str());
        ImGui::Text("Total Nodes: %d", nodeCount);
        
        // Selected node info
        if (m_selectedNode) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Selected: %s", m_selectedNode->GetName().c_str());
            
            // Show node type with color coding
            auto nodeType = m_selectedNode->GetNodeType();
            switch (nodeType) {
                case SceneNode::LIGHT:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "[LIGHT]");
                    break;
                case SceneNode::AUDIO:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 1.0f, 1.0f), "[AUDIO]");
                    break;
                case SceneNode::GUI:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 1.0f, 1.0f), "[GUI]");
                    break;
                case SceneNode::LPV_VOLUME:
                    ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[LPV] Global Illumination");
					break;
                case SceneNode::CAMERA:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[CAMERA]");
                    break;
                case SceneNode::MODEL:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "[MODEL]");
                    break;
                default:
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[NODE]");
                    break;
            }
            
            // Position info for selected node
            glm::vec3 pos = m_selectedNode->GetPosition();
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
            
            // Additional node information
            if (m_selectedNode->GetModel()) {
                ImGui::Text("Has Model: Yes");
            }
            if (m_selectedNode->GetRigidBody()) {
                ImGui::Text("Has Physics: Yes");
            }
            
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No selection");
        }
        
        ImGui::Separator();
        
        // Tree view controls
        if (ImGui::Button("Expand All")) {
            m_expandedNodes.clear();
            CollectAllNodes(m_sceneGraph->GetRoot(), m_expandedNodes);
        }
        ImGui::SameLine();
        if (ImGui::Button("Collapse All")) {
            m_expandedNodes.clear();
        }
        
        ImGui::Separator();
        
        // Tree view of scene hierarchy
        ImGui::BeginChild("Hierarchy", ImVec2(0, -1), true);
        RenderNode(m_sceneGraph->GetRoot());
        ImGui::EndChild();
        
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No scene graph available");
        ImGui::Text("Load a scene to see its hierarchy");
        
        ImGui::Separator();
        
        // Scene loading controls if available
        if (!m_sceneList.empty()) {
            ImGui::Text("Available Scenes:");
            
            for (size_t i = 0; i < m_sceneList.size(); ++i) {
                if (ImGui::Selectable(m_sceneList[i].c_str())) {
                    if (m_onSceneSwap) {
                        m_onSceneSwap(m_sceneList[i]);
                    }
                }
            }
        }
    }
    
    EndWindow();
}

void SceneHierarchyWindow::RenderNode(std::shared_ptr<SceneNode> node) {
    if (!node) return;
    
    std::string nodeName = node->GetName();
    if (nodeName.empty()) {
        nodeName = "Unnamed Node";
    }
    
    // Add type indicator and color coding
    auto nodeType = node->GetNodeType();
    std::string typeIcon;
    ImVec4 typeColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    
    switch (nodeType) {
        case SceneNode::LIGHT:
            typeIcon = "[L] ";
            typeColor = ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
            break;
        case SceneNode::AUDIO:
            typeIcon = "[A] ";
            typeColor = ImVec4(0.3f, 1.0f, 1.0f, 1.0f);
            break;
        case SceneNode::GUI:
            typeIcon = "[UI] ";
            typeColor = ImVec4(1.0f, 0.3f, 1.0f, 1.0f);
            break;
        case SceneNode::CAMERA:
            typeIcon = "[C] ";
            typeColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
            break;
        case SceneNode::MODEL:
            typeIcon = "[M] ";
            typeColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            break;
        default:
            typeIcon = "[N] ";
            typeColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            break;
    }
    
    std::string displayName = typeIcon + nodeName;
    
    // Check if node is expanded
    bool isExpanded = m_expandedNodes.find(node.get()) != m_expandedNodes.end();
    
    // Node flags
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    
    if (node->children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    if (m_selectedNode == node) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    if (isExpanded) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    
    // Render the tree node
    ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
    bool nodeOpen = ImGui::TreeNodeEx(displayName.c_str(), flags);
    ImGui::PopStyleColor();
    
    // Handle selection
    if (ImGui::IsItemClicked()) {
        m_selectedNode = node;
        
        // Notify via callback
        if (m_selectionCallback) {
            m_selectionCallback(node);
        }
        
        std::cout << "[SceneHierarchy] Selected node: " << nodeName 
                  << " (Type: " << static_cast<int>(nodeType) << ")" << std::endl;
    }
    
    // Context menu for node operations
    if (ImGui::BeginPopupContextItem()) {
        ImGui::Text("Node: %s", nodeName.c_str());
        ImGui::Separator();
        
        if (ImGui::MenuItem("Focus on Node")) {
            // Could implement camera focusing
            std::cout << "[SceneHierarchy] Focus on node: " << nodeName << std::endl;
        }
        
        if (ImGui::MenuItem("Print Debug Info")) {
            std::cout << "[SceneHierarchy] Node debug info:" << std::endl;
            std::cout << "  Name: " << nodeName << std::endl;
            std::cout << "  Type: " << static_cast<int>(nodeType) << std::endl;
            std::cout << "  Children: " << node->children.size() << std::endl;
            glm::vec3 pos = node->GetPosition();
            std::cout << "  Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
        }
        
        ImGui::EndPopup();
    }
    
    // Update expanded state and render children
    if (nodeOpen) {
        m_expandedNodes.insert(node.get());
        
        // Render children
        for (auto& child : node->children) {
            RenderNode(child);
        }
        
        ImGui::TreePop();
    } else {
        m_expandedNodes.erase(node.get());
    }
}

int SceneHierarchyWindow::CountNodes(std::shared_ptr<SceneNode> node) {
    if (!node) return 0;
    
    int count = 1;
    for (auto& child : node->children) {
        count += CountNodes(child);
    }
    return count;
}

void SceneHierarchyWindow::CollectAllNodes(std::shared_ptr<SceneNode> node, std::set<SceneNode*>& nodes) {
    if (!node) return;
    
    nodes.insert(node.get());
    for (auto& child : node->children) {
        CollectAllNodes(child, nodes);
    }
}

void SceneHierarchyWindow::SetSceneList(const std::vector<std::string>& scenes) {
    m_sceneList = scenes;
}