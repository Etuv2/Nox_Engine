#pragma once

#include "../RenderPass.h"
#include "../SceneNode.h"
#include "../GLBuffer.h"
#include <memory>
#include <vector>
#include <GL/glew.h>
#include <glm/glm.hpp>

class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;
struct RenderContext;

/**
 * @brief MDI draw command for debug bounding boxes
 */
struct DebugBBoxDrawCommand {
    GLuint count = 24;       // 12 edges * 2 vertices = 24 indices
    GLuint instanceCount = 1;
    GLuint firstIndex = 0;
    GLuint baseVertex = 0;
    GLuint baseInstance = 0;
};

/**
 * @brief Per-instance data for bounding box rendering
 */
struct DebugBBoxInstance {
    glm::mat4 modelMatrix;   // World transform
    glm::vec4 color;         // RGBA color based on node type
    glm::vec4 boundsMin;     // AABB min (w unused)
    glm::vec4 boundsMax;     // AABB max (w unused)
};

/**
 * @brief DebugBBoxPass renders wireframe bounding boxes for all scene nodes
 * 
 * This debug visualization pass draws AABBs (Axis-Aligned Bounding Boxes) 
 * around scene nodes with colors based on node type. Uses Multi-Draw Indirect
 * (MDI) batching for optimal performance with many debug primitives.
 * 
 * Color Legend:
 * - Green:   MODEL nodes (meshes, geometry)
 * - Yellow:  LIGHT nodes (point, spot, directional lights)
 * - Blue:    CAMERA nodes
 * - Cyan:    AUDIO nodes
 * - Magenta: LPV_VOLUME nodes (Light Propagation Volumes)
 * - Orange:  GUI nodes
 * - White:   Generic NODE type
 * 
 * Rendering Pipeline Position:
 * - Executes after lighting pass, renders over the scene
 * - Uses depth testing (GL_LEQUAL) to respect scene geometry
 * - Renders as wireframe lines
 * 
 * MDI Optimization:
 * - All bounding boxes are batched into a single draw call
 * - Instance data (transforms, colors, bounds) stored in SSBO
 * - Draw commands stored in indirect buffer
 * - Significantly reduces CPU overhead for large scenes
 */
class DebugBBoxPass : public RenderPass {
public:
    DebugBBoxPass();
    ~DebugBBoxPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    /**
     * @brief Get color for a specific node type
     * @param nodeType The type of the scene node
     * @return RGBA color vector
     */
    glm::vec4 GetColorForNodeType(SceneNode::NODE_TYPE nodeType) const;

    /**
     * @brief Recursively collect all nodes and build instance data
     * @param node Current node to process
     * @param parentTransform Accumulated parent transform
     */
    void CollectNodesRecursive(
        const std::shared_ptr<SceneNode>& node,
        const glm::mat4& parentTransform);

    /**
     * @brief Upload collected instance data to GPU buffers
     */
    void UploadToGPU();

    /**
     * @brief Render all bounding boxes using MDI
     * @param viewProj Combined view-projection matrix
     */
    void RenderMDI(const glm::mat4& viewProj);

    // Shader program
    GLuint m_shaderProgram = 0;

    // VAO and geometry buffers (using GLBuffer wrapper)
    GLuint m_cubeVAO = 0;
    GLBufferPtr m_vertexBuffer;      // Cube vertices
    GLBufferPtr m_indexBuffer;       // Cube edge indices

    // MDI buffers (using GLBuffer wrapper)
    GLBufferPtr m_indirectBuffer;    // Draw commands
    GLBufferPtr m_instanceSSBO;      // Per-instance data (transforms, colors, bounds)

    // Collected instance data (CPU side)
    std::vector<DebugBBoxInstance> m_instances;
    std::vector<DebugBBoxDrawCommand> m_drawCommands;

    // Uniform locations
    GLint m_locViewProjection = -1;

    // State
    bool m_initialized = false;
    size_t m_lastInstanceCount = 0;

    // SSBO binding point
    static constexpr GLuint INSTANCE_SSBO_BINDING = 4;
};
