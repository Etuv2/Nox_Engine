#include "GUIPass.h"
#include "../GuiNode.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include <iostream>

GUIPass::GUIPass() {}

GUIPass::~GUIPass() {}

bool GUIPass::Initialize(RenderContext& context) {
    m_screenWidth = context.width;
    m_screenHeight = context.height;
    
    std::cout << "[GUIPass] Initialized successfully for " << m_screenWidth 
              << "x" << m_screenHeight << " resolution\n";
    return true;
}

void GUIPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    m_screenWidth = newWidth;
    m_screenHeight = newHeight;
    
    std::cout << "[GUIPass] Resized to " << newWidth << "x" << newHeight << "\n";
    
    // Notify all GUI nodes about screen size change
    // This will be done during Execute() when we collect GUI nodes
}

void GUIPass::Execute(RenderContext& ctx,
                      const std::shared_ptr<SceneGraph>& sceneGraph,
                      const std::shared_ptr<Camera>& camera,
                      const std::shared_ptr<DirectionalLight>& dirLight,
                      const std::shared_ptr<Skybox>& skybox) {
    if (!sceneGraph) {
        return;
    }
    
    // Collect all GUI nodes from the scene graph
    auto guiNodes = CollectGuiNodes(sceneGraph);
    
    if (guiNodes.empty()) {
        // No GUI elements to render - this is normal for many scenes
        return;
    }
    
    std::cout << "[GUIPass] Rendering " << guiNodes.size() << " GUI node(s)\n";
    
    // Ensure we're rendering to the default framebuffer (backbuffer)
    // PostProcessPass should have already done this, but let's be explicit
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Set viewport to full screen
    glViewport(0, 0, ctx.width, ctx.height);
    
    // Configure OpenGL state for 2D GUI rendering
    glDisable(GL_DEPTH_TEST);  // GUI is always on top
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Render each GUI node
    for (auto& guiNode : guiNodes) {
        if (!guiNode) continue;
        
        // Update screen size if it changed (for responsive GUI)
        guiNode->UpdateScreenSize(ctx.width, ctx.height);
        
        // Render the GUI node's elements
        guiNode->RenderHUD();
    }
    
    // Restore OpenGL state
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    std::cout << "[GUIPass] Rendering complete\n";
}

std::vector<std::shared_ptr<GuiNode>> GUIPass::CollectGuiNodes(const std::shared_ptr<SceneGraph>& sceneGraph) {
    std::vector<std::shared_ptr<GuiNode>> guiNodes;
    
    if (!sceneGraph || !sceneGraph->GetRoot()) {
        return guiNodes;
    }
    
    // Traverse the scene graph to find all GUI nodes
    auto root = sceneGraph->GetRoot();
    
    for (auto& child : root->children) {
        CollectGuiNodesRecursive(child, guiNodes);
    }
    
    return guiNodes;
}

void GUIPass::CollectGuiNodesRecursive(const std::shared_ptr<SceneNode>& node, 
                                       std::vector<std::shared_ptr<GuiNode>>& guiNodes) {
    if (!node) return;
    
    // Check if this node is a GUI node
    if (node->GetNodeType() == SceneNode::GUI) {
        // Cast to GuiNode and add to collection
        auto guiNode = std::dynamic_pointer_cast<GuiNode>(node);
        if (guiNode) {
            guiNodes.push_back(guiNode);
            std::cout << "[GUIPass] Found GUI node with " << guiNode->GetElementCount() << " elements\n";
        }
    }
    
    // Recursively traverse children
    for (auto& child : node->children) {
        CollectGuiNodesRecursive(child, guiNodes);
    }
}
