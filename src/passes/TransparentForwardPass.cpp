#include "TransparentForwardPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../Camera.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include "../RenderContext.h"
#include "../Scene.h"
#include "../MeshComponent.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>

// Helper function to recursively collect all transparent nodes from scene graph
static void CollectTransparentNodes(const std::shared_ptr<SceneNode>& node, 
                                   std::vector<std::shared_ptr<SceneNode>>& transparentNodes) {
    if (!node) return;
    
    // Check if this node has a model with transparent meshes
    if (node->GetModel()) {
        auto model = node->GetModel();
        bool hasTransparency = false;
        
        // Check if any mesh in this model requires alpha blending
        for (const auto& mesh : model->meshes) {
            if (mesh.RequiresAlphaBlending()) {
                hasTransparency = true;
                break;
            }
        }
        
        // Add this node if it contains transparent meshes
        if (hasTransparency) {
            transparentNodes.push_back(node);
        }
    }
    
    // Recursively check children
    for (const auto& child : node->children) {
        CollectTransparentNodes(child, transparentNodes);
    }
}

TransparentForwardPass::TransparentForwardPass() {}

TransparentForwardPass::~TransparentForwardPass() {
    if (m_shader) glDeleteProgram(m_shader);
}

bool TransparentForwardPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/forward_transparent_vert.glsl", 
                                   "shaders/forward_transparent_frag.glsl");
    if (!m_shader) {
        std::cerr << "[TransparentForwardPass] Failed to create shader.\n";
        return false;
    }

    std::cout << "[TransparentForwardPass] Initialized successfully.\n";
    return true;
}

void TransparentForwardPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // No internal buffers - renders directly to HDR FBO
}

void TransparentForwardPass::Execute(RenderContext& ctx,
                                     const std::shared_ptr<SceneGraph>& sceneGraph,
                                     const std::shared_ptr<Camera>& camera,
                                     const std::shared_ptr<DirectionalLight>& dirLight,
                                     const std::shared_ptr<Skybox>& skybox) {
    if (!sceneGraph || !camera) {
        std::cerr << "[TransparentForwardPass] Missing sceneGraph or camera!" << std::endl;
        return;
    }

    std::cout << "[TransparentForwardPass] Starting execution..." << std::endl;

    // CRITICAL: HDR FBO should already be bound from previous passes (skybox rendering)
    // We render transparent objects AFTER skybox but BEFORE post-processing
    // DO NOT rebind or unbind the HDR FBO - just verify it's still bound
    if (!ctx.hdrFBO) {
        std::cerr << "[TransparentForwardPass] ERROR: HDR FBO is null!" << std::endl;
        return;
    }

    // Ensure HDR FBO is bound (defensive check - should already be bound)
    ctx.hdrFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);

    std::cout << "[TransparentForwardPass] Rendering to HDR FBO (ID: " << ctx.hdrFBO->GetFBO() << ")" << std::endl;

    // CRITICAL: Configure OpenGL state for physically correct transparency
    // 1. Enable depth testing to respect scene depth (objects behind opaque geometry won't render)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS); // Standard depth comparison
    
    // 2. CRITICAL: Disable depth writes so transparent objects don't block each other
    glDepthMask(GL_FALSE);
    
    // 3. Enable alpha blending with standard pre-multiplied alpha formula
    // This works in linear color space - gamma correction happens in post-processing
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    
    // 4. Disable face culling for glass materials (often double-sided)
    glDisable(GL_CULL_FACE);

    std::cout << "[TransparentForwardPass] Depth test: ENABLED, Depth writes: DISABLED, Blending: ENABLED" << std::endl;

    glUseProgram(m_shader);

    // Upload camera and matrices
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "projection"), 
                       1, GL_FALSE, glm::value_ptr(ctx.proj));
    
    // CRITICAL FIX: Use "viewPos" uniform name to match shader and legacy renderer
    glm::vec3 cameraPos = camera->GetCameraPosition();
    glUniform3fv(glGetUniformLocation(m_shader, "viewPos"), 
                 1, glm::value_ptr(cameraPos));
    
    std::cout << "[TransparentForwardPass] Camera position (viewPos): (" 
              << cameraPos.x << ", " << cameraPos.y << ", " << cameraPos.z << ")" << std::endl;

    // CRITICAL: Bind IBL textures for physically correct reflections and lighting
    if (skybox && skybox->ValidateIBLTextures()) {
        std::cout << "[TransparentForwardPass] Binding valid IBL textures..." << std::endl;
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());

        glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());

        glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
        glBindTexture(GL_TEXTURE_2D, skybox->GetBRDFLUT());

        glUniform1i(glGetUniformLocation(m_shader, "irradianceMap"), TextureUnits::IRRADIANCE_MAP);
        glUniform1i(glGetUniformLocation(m_shader, "prefilteredMap"), TextureUnits::PREFILTERED_ENV_MAP);
        glUniform1i(glGetUniformLocation(m_shader, "brdfLUT"), TextureUnits::BRDF_LUT);
        glUniform1f(glGetUniformLocation(m_shader, "prefilteredMaxLOD"), skybox->GetPrefilteredMaxLOD());
        
        std::cout << "[TransparentForwardPass] IBL textures bound successfully" << std::endl;
    } else {
        std::cerr << "[TransparentForwardPass] WARNING: No valid IBL textures available!" << std::endl;
    }

    // Bind light data for transparent objects
    if (ctx.lightManager && ctx.lightManager->GetActiveLightCount() > 0) {
        ctx.lightManager->UpdateGPUBuffers();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
        glUniform1i(glGetUniformLocation(m_shader, "numLights"), 
                   ctx.lightManager->GetActiveLightCount());
        
        std::cout << "[TransparentForwardPass] Bound " << ctx.lightManager->GetActiveLightCount() 
                  << " active lights" << std::endl;

        // Bind shadow array for transparent shadows
        GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
        if (shadowArray > 0 && glIsTexture(shadowArray)) {
            glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
            glUniform1i(glGetUniformLocation(m_shader, "multiLightShadowArray"), 
                       TextureUnits::SHADOW_MAP_ARRAY);
        }
    } else {
        glUniform1i(glGetUniformLocation(m_shader, "numLights"), 0);
        std::cout << "[TransparentForwardPass] No active lights available" << std::endl;
    }

    // Enhanced glass material uniforms for physically correct transparency
    glUniform1f(glGetUniformLocation(m_shader, "transmissionFactor"), 0.9f); // High transmission for glass
    glUniform1f(glGetUniformLocation(m_shader, "refractionIndex"), 1.5f);    // Typical glass IOR
    glUniform1i(glGetUniformLocation(m_shader, "useScreenSpaceRefraction"), 0); // Environment-based refraction

    // CRITICAL FIX: Collect transparent nodes from entire scene hierarchy based on mesh properties
    std::vector<std::shared_ptr<SceneNode>> transparentNodes;
    if (sceneGraph->GetRoot()) {
        CollectTransparentNodes(sceneGraph->GetRoot(), transparentNodes);
    }
    
    std::cout << "[TransparentForwardPass] Found " << transparentNodes.size() 
              << " nodes with transparent meshes (RequiresAlphaBlending)" << std::endl;

    // DEBUG: Log details about found transparent nodes
    if (transparentNodes.empty()) {
        std::cout << "[TransparentForwardPass] WARNING: No transparent nodes found - checking scene structure..." << std::endl;
        std::cout << "[TransparentForwardPass] Scene root exists: " << (sceneGraph->GetRoot() ? "YES" : "NO") << std::endl;
    } else {
        std::cout << "[TransparentForwardPass] Transparent nodes details:" << std::endl;
        for (size_t i = 0; i < transparentNodes.size(); ++i) {
            auto node = transparentNodes[i];
            std::cout << "  [" << i << "] " << node->GetName() 
                      << " - Model: " << (node->GetModel() ? "YES" : "NO") << std::endl;
            if (node->GetModel()) {
                auto model = node->GetModel();
                int transparentMeshCount = 0;
                for (const auto& mesh : model->meshes) {
                    if (mesh.RequiresAlphaBlending()) {
                        transparentMeshCount++;
                        std::cout << "      Mesh alpha: " << mesh.baseColorFactor.a 
                                  << " mode: " << mesh.alphaMode << std::endl;
                    }
                }
                std::cout << "      Total transparent meshes: " << transparentMeshCount << std::endl;
            }
        }
    }

    // CRITICAL: Sort transparent objects back-to-front for correct alpha blending
    // Objects further from camera render first, closer objects blend over them
    std::sort(transparentNodes.begin(), transparentNodes.end(),
        [&cameraPos](const std::shared_ptr<SceneNode>& a, const std::shared_ptr<SceneNode>& b) {
            // Get world positions from transform matrices
            glm::vec3 posA = glm::vec3(a->GetTransform()[3]);
            glm::vec3 posB = glm::vec3(b->GetTransform()[3]);
            
            // Calculate distances to camera
            float distA = glm::length(cameraPos - posA);
            float distB = glm::length(cameraPos - posB);
            
            // Sort back-to-front (larger distance first)
            return distA > distB;
        });

    // Render transparent objects with proper depth-aware blending
    int renderedCount = 0;
    for (const auto& node : transparentNodes) {
        if (node && node->GetModel()) {
            std::cout << "[TransparentForwardPass] Rendering transparent object: " << node->GetName() << std::endl;
            
            // Upload model matrix
            glm::mat4 modelMatrix = node->GetTransform();
            glUniformMatrix4fv(glGetUniformLocation(m_shader, "model"), 
                             1, GL_FALSE, glm::value_ptr(modelMatrix));
            
            // Calculate and upload normal matrix for correct lighting
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
            glUniformMatrix3fv(glGetUniformLocation(m_shader, "normalMatrix"), 
                             1, GL_FALSE, glm::value_ptr(normalMatrix));
            
            // Render the model
            auto sceneModel = node->GetModel();
            if (sceneModel) {
                sceneModel->Draw();
                renderedCount++;
            }
        }
    }
    
    std::cout << "[TransparentForwardPass] Successfully rendered " << renderedCount 
              << " transparent objects" << std::endl;

    // CRITICAL: Restore depth writes for subsequent passes but keep HDR FBO bound
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE); // Restore culling for opaque objects
    
    // DO NOT unbind the HDR FBO - let the next pass handle that
    // The pipeline expects: Lighting -> Skybox -> Transparent -> (TAA/Bloom) -> PostProcess
    
    std::cout << "[TransparentForwardPass] Execution complete, HDR FBO remains bound" << std::endl;
}
