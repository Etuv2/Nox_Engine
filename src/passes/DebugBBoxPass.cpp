#include "DebugBBoxPass.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include "../ShaderLoader.h"
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

// Unit cube vertices for wireframe box (8 corners)
static const float s_cubeVertices[] = {
    // Unit cube [0,1]^3
    0.0f, 0.0f, 0.0f,  // 0: ---
    1.0f, 0.0f, 0.0f,  // 1: +--
    1.0f, 1.0f, 0.0f,  // 2: ++-
    0.0f, 1.0f, 0.0f,  // 3: -+-
    0.0f, 0.0f, 1.0f,  // 4: --+
    1.0f, 0.0f, 1.0f,  // 5: +-+
    1.0f, 1.0f, 1.0f,  // 6: +++
    0.0f, 1.0f, 1.0f   // 7: -++
};

// Line indices for wireframe cube (12 edges = 24 indices)
static const unsigned int s_cubeIndices[] = {
    // Bottom face edges
    0, 1,  1, 2,  2, 3,  3, 0,
    // Top face edges
    4, 5,  5, 6,  6, 7,  7, 4,
    // Vertical edges
    0, 4,  1, 5,  2, 6,  3, 7
};

DebugBBoxPass::DebugBBoxPass() {}

DebugBBoxPass::~DebugBBoxPass() {
    if (m_cubeVAO) glDeleteVertexArrays(1, &m_cubeVAO);
    if (m_shaderProgram) glDeleteProgram(m_shaderProgram);
    // GLBuffer smart pointers auto-cleanup
}

bool DebugBBoxPass::Initialize(RenderContext& context) {
    // Load MDI-enabled bounding box shader
    m_shaderProgram = CreateShaderProgram(
        "shaders/debug_bbox_vert.glsl",
        "shaders/debug_bbox_frag.glsl"
    );
    
    if (!m_shaderProgram) {
        std::cerr << "[DebugBBoxPass] Failed to create shader program\n";
        return false;
    }

    // Cache uniform locations
    m_locViewProjection = glGetUniformLocation(m_shaderProgram, "u_ViewProjection");

    // Create VAO
    glGenVertexArrays(1, &m_cubeVAO);
    glBindVertexArray(m_cubeVAO);

    // Create vertex buffer using GLBuffer wrapper
    m_vertexBuffer = std::make_unique<GLBuffer>(
        BufferType::Vertex, 
        sizeof(s_cubeVertices), 
        s_cubeVertices,
        BufferUsage::StaticDraw
    );
    m_vertexBuffer->SetLabel("DebugBBox_VertexBuffer");
    
    // Create index buffer using GLBuffer wrapper
    // IMPORTANT: EBO must be bound while VAO is active to be captured by VAO state
    m_indexBuffer = std::make_unique<GLBuffer>(
        BufferType::Index,
        sizeof(s_cubeIndices),
        s_cubeIndices,
        BufferUsage::StaticDraw
    );
    m_indexBuffer->SetLabel("DebugBBox_IndexBuffer");

    // Position attribute (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Unbind VAO first (keeps EBO binding in VAO state)
    glBindVertexArray(0);
    
    // Now safe to unbind VBO (not part of VAO state after glVertexAttribPointer)
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Create MDI buffers (initially empty, will be sized on first use)
    m_indirectBuffer = std::make_unique<GLBuffer>(
        BufferType::DrawIndirect,
        BufferUsage::DynamicDraw
    );
    m_indirectBuffer->SetLabel("DebugBBox_IndirectBuffer");

    m_instanceSSBO = std::make_unique<GLBuffer>(
        BufferType::ShaderStorage,
        BufferUsage::DynamicDraw
    );
    m_instanceSSBO->SetLabel("DebugBBox_InstanceSSBO");

    // Reserve initial capacity
    m_instances.reserve(256);
    m_drawCommands.reserve(256);

    m_initialized = true;
    std::cout << "[DebugBBoxPass] Initialized successfully with MDI batching\n";
    return true;
}

void DebugBBoxPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // No framebuffers to resize
}

void DebugBBoxPass::Execute(RenderContext& ctx,
                            const std::shared_ptr<SceneGraph>& sceneGraph,
                            const std::shared_ptr<Camera>& camera,
                            const std::shared_ptr<DirectionalLight>& dirLight,
                            const std::shared_ptr<Skybox>& skybox) {
    
    // Only execute if debug bounding boxes are enabled
    if (!ctx.showBoundingBoxes) {
        return;
    }

    if (!m_initialized || !sceneGraph || !sceneGraph->GetRoot()) {
        return;
    }

    // Clear previous frame's data
    m_instances.clear();
    m_drawCommands.clear();

    // Collect all nodes and build instance data
    CollectNodesRecursive(sceneGraph->GetRoot(), glm::mat4(1.0f));

    if (m_instances.empty()) {
        std::cout << "[DebugBBoxPass] No bounding boxes to draw\n";
        return;
    }

    std::cout << "[DebugBBoxPass] Collected " << m_instances.size() << " bounding box(es) to render\n";

    // Upload to GPU
    UploadToGPU();

    // Setup OpenGL state for wireframe rendering
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  // Render to backbuffer
    glViewport(0, 0, ctx.width, ctx.height);
    
    // Save state before modifying
    GLint oldPolygonMode[2];
    glGetIntegerv(GL_POLYGON_MODE, oldPolygonMode);
    
    GLboolean depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLint oldDepthFunc;
    glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
    
    GLboolean cullFaceWasEnabled = glIsEnabled(GL_CULL_FACE);
    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldBlendSrc, oldBlendDst;
    glGetIntegerv(GL_BLEND_SRC, &oldBlendSrc);
    glGetIntegerv(GL_BLEND_DST, &oldBlendDst);
    
    GLfloat oldLineWidth;
    glGetFloatv(GL_LINE_WIDTH, &oldLineWidth);
    
    // Configure state for debug box rendering
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);  // Allow drawing on same depth
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(2.0f);
    
    // Enable blending for semi-transparent boxes
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Render using MDI
    glm::mat4 viewProj = ctx.proj * ctx.view;
    RenderMDI(viewProj);

    // Restore OpenGL state precisely
    glPolygonMode(GL_FRONT_AND_BACK, oldPolygonMode[0]);
    glLineWidth(oldLineWidth);
    glDepthFunc(oldDepthFunc);
    if (!depthTestWasEnabled) glDisable(GL_DEPTH_TEST);
    if (cullFaceWasEnabled) glEnable(GL_CULL_FACE);
    if (blendWasEnabled) {
        glEnable(GL_BLEND);
        glBlendFunc(oldBlendSrc, oldBlendDst);
    } else {
        glDisable(GL_BLEND);
    }
}

void DebugBBoxPass::CollectNodesRecursive(
    const std::shared_ptr<SceneNode>& node,
    const glm::mat4& parentTransform) {
    
    if (!node) return;

    // Compute world transform for this node
    glm::mat4 worldTransform = node->GetGlobalTransform(parentTransform);

    // Get bounding box from node
    auto [minBounds, maxBounds] = node->GetBoundingBox();
    
    // Skip nodes with invalid/empty bounding boxes (check for non-zero size)
    glm::vec3 size = maxBounds - minBounds;
    if (glm::length(size) > 0.0001f) {
        // Create instance data
        DebugBBoxInstance instance;
        instance.modelMatrix = worldTransform;
        instance.color = GetColorForNodeType(node->GetNodeType());
        instance.boundsMin = glm::vec4(minBounds, 0.0f);
        instance.boundsMax = glm::vec4(maxBounds, 0.0f);
        
        m_instances.push_back(instance);
        
        // Create corresponding draw command
        DebugBBoxDrawCommand cmd;
        cmd.count = 24;  // 12 edges * 2 vertices
        cmd.instanceCount = 1;
        cmd.firstIndex = 0;
        cmd.baseVertex = 0;
        cmd.baseInstance = static_cast<GLuint>(m_instances.size() - 1);
        
        m_drawCommands.push_back(cmd);
        
        // Debug output
        std::string nodeName = node->GetName();
        std::cout << "[DebugBBoxPass] Added bbox for " << nodeName 
                  << " (type " << static_cast<int>(node->GetNodeType()) << ")"
                  << " size: " << glm::length(size) << std::endl;
    }

    // Recursively process children
    for (const auto& child : node->children) {
        CollectNodesRecursive(child, worldTransform);
    }
}

void DebugBBoxPass::UploadToGPU() {
    if (m_instances.empty()) return;

    // Upload instance data to SSBO
    size_t instanceDataSize = m_instances.size() * sizeof(DebugBBoxInstance);
    
    // Always reallocate to ensure clean state (orphan pattern)
    m_instanceSSBO->Allocate(instanceDataSize, m_instances.data());
    
    // Also upload indirect buffer
    size_t cmdSize = m_drawCommands.size() * sizeof(DebugBBoxDrawCommand);
    m_indirectBuffer->Allocate(cmdSize, m_drawCommands.data());
    
    m_lastInstanceCount = m_instances.size();
}

void DebugBBoxPass::RenderMDI(const glm::mat4& viewProj) {
    // Use shader
    glUseProgram(m_shaderProgram);

    // Set view-projection matrix
    glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));

    // Bind instance SSBO at binding point
    m_instanceSSBO->BindBase(INSTANCE_SSBO_BINDING);

    // Bind VAO (contains VBO and EBO)
    glBindVertexArray(m_cubeVAO);

    // Bind indirect buffer for MDI
    m_indirectBuffer->Bind();

    // Issue single MDI draw call for all bounding boxes
    glMultiDrawElementsIndirect(
        GL_LINES,
        GL_UNSIGNED_INT,
        nullptr,  // Offset in indirect buffer (0)
        static_cast<GLsizei>(m_drawCommands.size()),
        sizeof(DebugBBoxDrawCommand)
    );

    // Cleanup state
    glBindVertexArray(0);
    m_indirectBuffer->Unbind();
    glUseProgram(0);
}

glm::vec4 DebugBBoxPass::GetColorForNodeType(SceneNode::NODE_TYPE nodeType) const {
    switch (nodeType) {
        case SceneNode::NODE_TYPE::MODEL:
            return glm::vec4(0.2f, 1.0f, 0.2f, 0.8f);  // Green for meshes
        
        case SceneNode::NODE_TYPE::LIGHT:
            return glm::vec4(1.0f, 1.0f, 0.2f, 0.8f);  // Yellow for lights
        
        case SceneNode::NODE_TYPE::CAMERA:
            return glm::vec4(0.2f, 0.5f, 1.0f, 0.8f);  // Blue for cameras
        
        case SceneNode::NODE_TYPE::AUDIO:
            return glm::vec4(0.2f, 1.0f, 1.0f, 0.8f);  // Cyan for audio
        
        case SceneNode::NODE_TYPE::LPV_VOLUME:
            return glm::vec4(1.0f, 0.2f, 1.0f, 0.8f);  // Magenta for LPV volumes
        
        case SceneNode::NODE_TYPE::GUI:
            return glm::vec4(1.0f, 0.5f, 0.0f, 0.8f);  // Orange for GUI
        
        case SceneNode::NODE_TYPE::NODE:
        default:
            return glm::vec4(1.0f, 1.0f, 1.0f, 0.6f);  // White for generic nodes
    }
}
