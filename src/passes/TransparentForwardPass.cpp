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

/**
 * @brief Helper function to recursively collect all transparent nodes from scene graph
 * 
 * Traverses the scene hierarchy and identifies nodes containing meshes that require
 * alpha blending based on their material properties.
 * 
 * @param node The current node to check
 * @param transparentNodes Output vector to store transparent nodes
 */
static void CollectTransparentNodes(const std::shared_ptr<SceneNode>& node, 
    std::vector<std::shared_ptr<SceneNode>>& transparentNodes)
{
    if (!node) {
        return;
    }
    
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

TransparentForwardPass::~TransparentForwardPass()
{
    if (m_shader) {
      glDeleteProgram(m_shader);
    m_shader = 0;
    }
}

bool TransparentForwardPass::Initialize(RenderContext& context)
{
    m_shader = CreateShaderProgram("shaders/forward_transparent_vert.glsl", 
 "shaders/forward_transparent_frag.glsl");
  if (!m_shader) {
      std::cerr << "[TransparentForwardPass] Failed to create shader.\n";
        return false;
    }

  std::cout << "[TransparentForwardPass] Initialized successfully.\n";
    return true;
}

void TransparentForwardPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
    // No internal buffers - renders directly to HDR FBO
}

void TransparentForwardPass::Execute(RenderContext& ctx,
      const std::shared_ptr<SceneGraph>& sceneGraph,
       const std::shared_ptr<Camera>& camera,
  const std::shared_ptr<DirectionalLight>& dirLight,
       const std::shared_ptr<Skybox>& skybox)
{
    if (!sceneGraph || !camera) {
        std::cerr << "[TransparentForwardPass] Missing sceneGraph or camera!" << std::endl;
     return;
    }

    std::cout << "[TransparentForwardPass] Starting execution..." << std::endl;

    //Collect transparent nodes FIRST to check if we have any work to do
    std::vector<std::shared_ptr<SceneNode>> transparentNodes;
    if (sceneGraph->GetRoot()) {
  CollectTransparentNodes(sceneGraph->GetRoot(), transparentNodes);
 }
    
    std::cout << "[TransparentForwardPass] Found " << transparentNodes.size() 
   << " nodes with transparent meshes" << std::endl;

    // Early exit if no transparent objects to render
    if (transparentNodes.empty()) {
      std::cout << "[TransparentForwardPass] No transparent objects found - skipping pass" << std::endl;
        return;
    }

    //HDR FBO should already be bound from skybox rendering
    // We DO NOT rebind or unbind - just verify it's correct
    if (!ctx.hdrFBO) {
        std::cerr << "[TransparentForwardPass] ERROR: HDR FBO is null!" << std::endl;
        return;
    }

    std::cout << "[TransparentForwardPass] Rendering to HDR FBO (ID: " << ctx.hdrFBO->GetFBO() << ")" << std::endl;

    //Set up transparent rendering state WITHOUT clearing or rebinding
    // The depth buffer already contains opaque geometry + skybox at max depth
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);        // Test against existing depth
    glDepthMask(GL_FALSE);       // Don't write to depth buffer (transparency layering)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha blending
    glEnable(GL_CULL_FACE);      // Enable culling for proper transparent rendering
    glCullFace(GL_BACK);         // Cull back faces

    std::cout << "[TransparentForwardPass] State: Depth test=ENABLED(LESS), Depth writes=DISABLED, "
        << "Blending=ENABLED(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)" << std::endl;

    glUseProgram(m_shader);

    // Upload camera and matrices
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "view"), 
        1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "projection"), 
        1, GL_FALSE, glm::value_ptr(ctx.proj));
    
    glm::vec3 cameraPos = camera->GetCameraPosition();
    glUniform3fv(glGetUniformLocation(m_shader, "viewPos"), 
        1, glm::value_ptr(cameraPos));

    // Upload screen size for depth comparison in fragment shader
    glUniform2f(glGetUniformLocation(m_shader, "screenSize"), 
        static_cast<float>(ctx.width), static_cast<float>(ctx.height));
    
    std::cout << "[TransparentForwardPass] Camera position: (" 
        << cameraPos.x << ", " << cameraPos.y << ", " << cameraPos.z << ")" << std::endl;

    // Bind depth buffer from G-buffer for depth comparisons
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_DEPTH);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_shader, "gDepth"), TextureUnits::GBUFFER_DEPTH);

    // Bind IBL textures for physically correct reflections and lighting
    if (skybox && skybox->ValidateIBLTextures()) {
        std::cout << "[TransparentForwardPass] Binding IBL textures..." << std::endl;
        
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
    } else {
        std::cout << "[TransparentForwardPass] WARNING: No valid IBL textures available" << std::endl;
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

    // NOTE: Material-specific transmission and IOR uniforms are now set per-mesh
    // in SceneNode::Draw() rather than as pass-wide defaults here.
    // This allows each transparent material to use its own KHR_materials_transmission
    // and KHR_materials_ior extension values.

    //Sort transparent objects back-to-front for correct alpha blending
    std::sort(transparentNodes.begin(), transparentNodes.end(),
        [&cameraPos](const std::shared_ptr<SceneNode>& a, const std::shared_ptr<SceneNode>& b) {
            glm::vec3 posA = glm::vec3(a->GetTransform()[3]);
            glm::vec3 posB = glm::vec3(b->GetTransform()[3]);
            float distA = glm::length(cameraPos - posA);
            float distB = glm::length(cameraPos - posB);
            return distA > distB; // Back-to-front ordering
        });

    // Render transparent objects with proper depth-aware blending
    int renderedCount = 0;
    for (const auto& node : transparentNodes) {
        if (node && node->GetModel()) {
            std::cout << "[TransparentForwardPass] Rendering: " << node->GetName() << std::endl;
     
            // Upload model matrix
            glm::mat4 modelMatrix = node->GetTransform();
            glUniformMatrix4fv(glGetUniformLocation(m_shader, "model"), 
                1, GL_FALSE, glm::value_ptr(modelMatrix));
       
            // Calculate and upload normal matrix for correct lighting
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
            glUniformMatrix3fv(glGetUniformLocation(m_shader, "normalMatrix"), 
                1, GL_FALSE, glm::value_ptr(normalMatrix));
    
            // Render the model - material uniforms including transmission and IOR
            // are set per-mesh in Scene::Draw() via SceneNode
            auto sceneModel = node->GetModel();
            if (sceneModel) {
                sceneModel->Draw();
                renderedCount++;
            }
        }
    }
    
    std::cout << "[TransparentForwardPass] Successfully rendered " << renderedCount 
        << " transparent objects" << std::endl;

    //Restore render state for subsequent passes
    glDepthMask(GL_TRUE);      // Re-enable depth writes
    glDisable(GL_BLEND);       // Disable blending
    glDepthFunc(GL_LESS);      // Reset depth function to default
    
    // DO NOT unbind the HDR FBO - let the pipeline coordinator handle that
    
    std::cout << "[TransparentForwardPass] Execution complete (HDR FBO remains bound)" << std::endl;
}
