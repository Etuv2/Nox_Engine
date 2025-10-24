#pragma once

#include "BaseWindow.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <set>

// Forward declarations
class SceneNode;

/**
 * @brief Scene hierarchy tree view window
 */
class SceneHierarchyWindow : public BaseWindow {
public:
    SceneHierarchyWindow();
    
    void Render() override;
    
    // Callback for when a node is selected
    void SetSelectionCallback(const std::function<void(std::shared_ptr<SceneNode>)>& callback) {
        m_selectionCallback = callback;
    }
    
    // Scene management
    void SetSceneList(const std::vector<std::string>& scenes);
    void SetSceneSwapCallback(const std::function<void(const std::string&)>& callback) {
        m_onSceneSwap = callback;  
    }

private:
    std::function<void(std::shared_ptr<SceneNode>)> m_selectionCallback;
    std::function<void(const std::string&)> m_onSceneSwap;
    std::vector<std::string> m_sceneList;
    std::set<SceneNode*> m_expandedNodes; // Track which nodes are expanded
    
    // Helper methods for tree rendering
    void RenderNode(std::shared_ptr<SceneNode> node);
    int CountNodes(std::shared_ptr<SceneNode> node);
    void CollectAllNodes(std::shared_ptr<SceneNode> node, std::set<SceneNode*>& nodes);
};