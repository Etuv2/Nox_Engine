#include "DebugBBoxPass.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../Scene.h"
#include "../MeshComponent.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include "../ShaderLoader.h"
#include <algorithm>
#include <cmath>
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

namespace {
void ExpandLocalBoundsWithMesh(const MeshComponent& mesh,
                               const glm::mat4& localTransform,
                               glm::vec3& minBounds,
                               glm::vec3& maxBounds,
                               bool& valid)
{
    if (!mesh.boundingVolumeValid) {
        return;
    }

    const glm::vec3 corners[8] = {
        {mesh.boundingMin.x, mesh.boundingMin.y, mesh.boundingMin.z},
        {mesh.boundingMax.x, mesh.boundingMin.y, mesh.boundingMin.z},
        {mesh.boundingMin.x, mesh.boundingMax.y, mesh.boundingMin.z},
        {mesh.boundingMax.x, mesh.boundingMax.y, mesh.boundingMin.z},
        {mesh.boundingMin.x, mesh.boundingMin.y, mesh.boundingMax.z},
        {mesh.boundingMax.x, mesh.boundingMin.y, mesh.boundingMax.z},
        {mesh.boundingMin.x, mesh.boundingMax.y, mesh.boundingMax.z},
        {mesh.boundingMax.x, mesh.boundingMax.y, mesh.boundingMax.z}
    };

    for (const glm::vec3& corner : corners) {
        const glm::vec3 transformed = glm::vec3(localTransform * glm::vec4(corner, 1.0f));
        if (!valid) {
            minBounds = transformed;
            maxBounds = transformed;
            valid = true;
        }
        else {
            minBounds = glm::min(minBounds, transformed);
            maxBounds = glm::max(maxBounds, transformed);
        }
    }
}

glm::mat4 ResolveMeshLocalTransformForDebug(const SceneNode& node, const MeshComponent& mesh)
{
    const auto model = node.GetModel();
    if (!node.renderWholeModel || mesh.sourceNodeIndex < 0 || !model) {
        return glm::mat4(1.0f);
    }

    const int referenceNodeIndex = node.nodeIndex;
    if (referenceNodeIndex < 0 || referenceNodeIndex == mesh.sourceNodeIndex) {
        return mesh.localTransform;
    }

    const auto& nodeWorldTransforms = model->GetNodeWorldTransforms();
    if (referenceNodeIndex >= static_cast<int>(nodeWorldTransforms.size()) ||
        mesh.sourceNodeIndex >= static_cast<int>(nodeWorldTransforms.size())) {
        return mesh.localTransform;
    }

    const glm::mat4 referenceInverse = glm::inverse(nodeWorldTransforms[referenceNodeIndex]);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(referenceInverse[c][r])) {
                return mesh.localTransform;
            }
        }
    }

    return referenceInverse * mesh.localTransform;
}

bool ComputeDebugLocalBounds(const SceneNode& node, glm::vec3& minBounds, glm::vec3& maxBounds)
{
    const auto model = node.GetModel();
    if (model) {
        bool valid = false;
        if (node.renderWholeModel) {
            for (const MeshComponent& mesh : model->meshes) {
                ExpandLocalBoundsWithMesh(mesh, ResolveMeshLocalTransformForDebug(node, mesh),
                                          minBounds, maxBounds, valid);
            }
        }
        else {
            for (uint32_t meshIndex : node.renderMeshIndices) {
                if (meshIndex < model->meshes.size()) {
                    const MeshComponent& mesh = model->meshes[meshIndex];
                    ExpandLocalBoundsWithMesh(mesh, glm::mat4(1.0f), minBounds, maxBounds, valid);
                }
            }
        }

        if (valid) {
            return true;
        }
    }

    switch (node.GetNodeType()) {
        case SceneNode::LIGHT:
        case SceneNode::CAMERA:
        case SceneNode::NODE:
        default:
            minBounds = glm::vec3(-0.5f);
            maxBounds = glm::vec3(0.5f);
            return true;
        case SceneNode::AUDIO:
            minBounds = glm::vec3(-0.25f);
            maxBounds = glm::vec3(0.25f);
            return true;
        case SceneNode::GUI:
            minBounds = glm::vec3(-0.1f);
            maxBounds = glm::vec3(0.1f);
            return true;
    }
}
}

DebugBBoxPass::DebugBBoxPass() {}

DebugBBoxPass::~DebugBBoxPass() {
    if (m_cubeVAO) glDeleteVertexArrays(1, &m_cubeVAO);
    if (m_shaderProgram) glDeleteProgram(m_shaderProgram);
    // GLBuffer smart pointers auto-cleanup
}

bool DebugBBoxPass::Initialize(RenderContext& context) {
    // Load instanced bounding box shader
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

    m_instanceSSBO = std::make_unique<GLBuffer>(
        BufferType::ShaderStorage,
        BufferUsage::DynamicDraw
    );
    m_instanceSSBO->SetLabel("DebugBBox_InstanceSSBO");

    // Reserve initial capacity
    m_instances.reserve(256);

    m_initialized = true;
    std::cout << "[DebugBBoxPass] Initialized successfully with instanced batching\n";
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

    // Collect all nodes and build instance data
    CollectNodesRecursive(sceneGraph->GetRoot(), glm::mat4(1.0f));

    if (m_instances.empty()) {
        return;
    }

    // Upload to GPU
    UploadToGPU();

    // Setup OpenGL state for wireframe rendering
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  // Render to backbuffer
    glViewport(0, 0, ctx.width, ctx.height);
    
    // Save only state this pass actually changes. GL_LINES does not need polygon mode or cull tweaks.
    GLboolean depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLint oldDepthFunc;
    glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
    
    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldBlendSrc, oldBlendDst;
    glGetIntegerv(GL_BLEND_SRC, &oldBlendSrc);
    glGetIntegerv(GL_BLEND_DST, &oldBlendDst);

    // Configure state for debug box rendering
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);  // Allow drawing on same depth
    glLineWidth(2.0f);
    
    // Enable blending for semi-transparent boxes
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Render using one instanced draw
    glm::mat4 viewProj = ctx.proj * ctx.view;
    RenderInstanced(viewProj);

    // Restore OpenGL state precisely
    glLineWidth(1.0f);
    glDepthFunc(oldDepthFunc);
    if (!depthTestWasEnabled) glDisable(GL_DEPTH_TEST);
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

    glm::vec3 minBounds(0.0f);
    glm::vec3 maxBounds(0.0f);
    if (!ComputeDebugLocalBounds(*node, minBounds, maxBounds)) {
        return;
    }
    
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
    }

    // Recursively process children
    for (const auto& child : node->children) {
        CollectNodesRecursive(child, worldTransform);
    }
}

void DebugBBoxPass::UploadToGPU() {
    if (m_instances.empty()) return;

    const size_t instanceDataSize = m_instances.size() * sizeof(DebugBBoxInstance);
    if (m_instances.size() > m_instanceCapacity) {
        m_instanceCapacity = std::max<size_t>(256, m_instanceCapacity);
        while (m_instanceCapacity < m_instances.size()) {
            m_instanceCapacity *= 2;
        }
        m_instanceSSBO->Allocate(m_instanceCapacity * sizeof(DebugBBoxInstance), nullptr);
    }

    m_instanceSSBO->SubData(0, instanceDataSize, m_instances.data());
    
    m_lastInstanceCount = m_instances.size();
}

void DebugBBoxPass::RenderInstanced(const glm::mat4& viewProj) {
    // Use shader
    glUseProgram(m_shaderProgram);

    // Set view-projection matrix
    glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProj));

    // Bind instance SSBO at binding point
    m_instanceSSBO->BindBase(INSTANCE_SSBO_BINDING);

    // Bind VAO (contains VBO and EBO)
    glBindVertexArray(m_cubeVAO);

    // Issue one draw for all bounding boxes. This avoids one indirect command per box.
    glDrawElementsInstanced(
        GL_LINES,
        24,
        GL_UNSIGNED_INT,
        nullptr,
        static_cast<GLsizei>(m_instances.size())
    );

    // Cleanup state
    glBindVertexArray(0);
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
