#pragma once

#include "../RenderPass.h"
#include <memory>
#include <vector>
#include <GL/glew.h>

class GuiNode;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;
struct RenderContext;

/**
 * @brief GUIPass renders internal engine GUI elements (not ImGui editor UI)
 * 
 * This pass handles rendering of in-game GUI elements defined via GuiNode.
 * It executes after post-processing but before ImGui editor rendering.
 * 
 * Rendering Pipeline Position:
 * 1. All 3D rendering (G-buffer, lighting, post-process) ? Backbuffer
 * 2. **GUI Pass renders here** ? Directly to backbuffer
 * 3. ImGui editor UI ? Overlaid on top
 * 
 * Features:
 * - Text rendering using TTF fonts
 * - Image/sprite rendering
 * - Solid, gradient, and beveled rectangles
 * - Screen-space 2D rendering with proper depth management
 * - Collects all GuiNode elements from scene graph
 * 
 * Rendering State:
 * - Renders to default framebuffer (backbuffer) after post-processing
 * - Disables depth testing (2D overlay)
 * - Enables alpha blending for transparency
 * - Uses orthographic projection matching screen resolution
 */
class GUIPass : public RenderPass {
public:
    GUIPass();
    ~GUIPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    /**
     * @brief Collect all GUI nodes from the scene graph hierarchy
     * @param sceneGraph The scene graph to traverse
     * @return Vector of all GuiNode instances found in the scene
     */
    std::vector<std::shared_ptr<GuiNode>> CollectGuiNodes(const std::shared_ptr<SceneGraph>& sceneGraph);
    
    /**
     * @brief Recursively traverse scene node hierarchy to find GUI nodes
     * @param node Current node being examined
     * @param guiNodes Output vector to accumulate GUI nodes
     */
    void CollectGuiNodesRecursive(const std::shared_ptr<class SceneNode>& node, 
                                   std::vector<std::shared_ptr<GuiNode>>& guiNodes);

    int m_screenWidth = 1920;
    int m_screenHeight = 1080;
};
