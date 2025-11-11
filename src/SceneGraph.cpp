#include "SceneGraph.h"
#include "SceneNode.h"
#include "Scene.h"
#include "LightManager.h"
#include "LightNode.h"
#include <glm/gtc/type_ptr.hpp>
#include <GL/glew.h>
#include <iostream>

SceneGraph::SceneGraph() 
    : m_transformSystem(&m_componentManager)
{
    m_root = std::make_shared<SceneNode>();
    m_root->SetTransform(glm::mat4(1.0f));
    m_active = true;
  
    // Initialize SceneNode static references to use this graph's component system
    SceneNode::SetGlobalComponentManager(&m_componentManager);
  SceneNode::SetGlobalTransformSystem(&m_transformSystem);
}

SceneGraph::~SceneGraph()
{
    Shutdown();
}

std::shared_ptr<SceneNode> SceneGraph::GetRoot() {
    return m_root;
}

std::map<std::string, std::shared_ptr<SceneNode>> SceneGraph::GetSceneHierarchy()
{
    std::map<std::string, std::shared_ptr<SceneNode>> sceneHierarchy;
    sceneHierarchy[m_root->GetName()] = m_root;
 for (auto& child : m_root->children) {
        sceneHierarchy[child->GetName()] = child;
  }
    return sceneHierarchy;
}

// NEW: Update all transforms in one batch using component system
void SceneGraph::UpdateAllTransforms() {
    m_transformSystem.UpdateTransforms();
}

// Multi-light system integration
void SceneGraph::SetLightManager(std::shared_ptr<LightManager> lightManager) {
    m_lightManager = lightManager;
    UpdateLightManager(); // Automatically collect lights when manager is set
}

std::shared_ptr<LightManager> SceneGraph::GetLightManager() const {
    return m_lightManager;
}

void SceneGraph::UpdateLightManager() {
    if (!m_lightManager) {
        std::cout << "[SceneGraph] No LightManager available for light collection" << std::endl;
        return;
    }
    
    // Clear existing lights and collect from scene
    m_lightManager->ClearAllLights();
    
    // CRITICAL DEBUG: Check if scene has any nodes at all
    if (!m_root) {
        std::cerr << "[SceneGraph] ERROR: No root node exists!" << std::endl;
        return;
    }
    
    int totalNodes = static_cast<int>(m_root->children.size());
    std::cout << "[SceneGraph] Scanning " << totalNodes << " root children for lights..." << std::endl;
    
    // Find all LightNode objects in the scene
    auto lightNodes = FindNodesByType(SceneNode::LIGHT);
    
    std::cout << "[SceneGraph] Found " << lightNodes.size() << " light nodes in scene" << std::endl;
    
    // CRITICAL DEBUG: If no lights found, create a default directional light
    if (lightNodes.empty()) {
        std::cout << "[SceneGraph] No lights found in scene - this will result in no shadows!" << std::endl;
        std::cout << "[SceneGraph] Available node types in scene:" << std::endl;
        
        for (auto& child : m_root->children) {
            if (child) {
                std::cout << "  - Node: " << child->GetName() 
                          << " Type: " << static_cast<int>(child->GetNodeType()) << std::endl;
            }
        }
        
        // If this is a loaded scene with models but no lights, we should have lights
        std::cout << "[SceneGraph] WARNING: Scene loaded but contains no lights - shadows will not work!" << std::endl;
    }
    
    for (auto& node : lightNodes) {
        auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
        if (lightNode && lightNode->GetLight()) {
            // Try to derive name from scene or use a generic one
            std::string lightName = "SceneLight_" + std::to_string(m_lightManager->GetLightCount() + 1);
            
            // Register the light node
            m_lightManager->RegisterLightNode(lightNode, lightName);
            
            // CRITICAL DEBUG: Check light properties
            auto light = lightNode->GetLight();
            std::cout << "[SceneGraph] Registered light: " << lightName << std::endl;
            std::cout << "  - Type: " << static_cast<int>(light->GetLightType()) << std::endl;
            std::cout << "  - Intensity: " << light->GetIntensity() << std::endl;
            std::cout << "  - Enabled: " << (light->IsEnabled() ? "Yes" : "No") << std::endl;
            std::cout << "  - Casts Shadows: " << (light->CastsShadows() ? "Yes" : "No") << std::endl;
            std::cout << "  - Position: (" << light->GetPosition().x << ", " << light->GetPosition().y << ", " << light->GetPosition().z << ")" << std::endl;
            std::cout << "  - Direction: (" << light->GetDirection().x << ", " << light->GetDirection().y << ", " << light->GetDirection().z << ")" << std::endl;
        }
    }
    
    // Update GPU buffers after collecting lights
    m_lightManager->UpdateGPUBuffers();
    
    std::cout << "[SceneGraph] Light collection complete. Active lights: " 
              << m_lightManager->GetActiveLightCount() << "/" << m_lightManager->GetLightCount() << std::endl;
    
    // CRITICAL DEBUG: Report shadow casting status
    size_t shadowCastingLights = m_lightManager->GetShadowCastingLightCount();
    std::cout << "[SceneGraph] Shadow-casting lights: " << shadowCastingLights << std::endl;
    
    if (shadowCastingLights == 0) {
        std::cout << "[SceneGraph] WARNING: No lights are set to cast shadows!" << std::endl;
    }
}

//Forward pass draw
void SceneGraph::Draw(
    const glm::mat4& view,
    const glm::mat4& projection,
    GLuint shaderProgram)
{
    // Forward shading pass
    if (m_root) {
        m_root->Draw(glm::mat4(1.0f), view, projection, shaderProgram);
    }
}

void SceneGraph::DrawCascade(
    const glm::mat4& lightSpace,
    GLuint shadowShader)
{
    // CRITICAL DEBUG: Add comprehensive logging to track shadow rendering
    std::cout << "[SceneGraph] DrawCascade called with shader=" << shadowShader << std::endl;
    
    if (!m_root) {
        std::cerr << "[SceneGraph] ERROR: No root node for shadow rendering!" << std::endl;
        return;
    }
    
    // Count how many child nodes we have
    int childCount = static_cast<int>(m_root->children.size());
    std::cout << "[SceneGraph] Root has " << childCount << " children for shadow rendering" << std::endl;
    
    if (childCount == 0) {
        std::cerr << "[SceneGraph] WARNING: No child nodes to render shadows for!" << std::endl;
        return;
    }
    
    // Verify the shader is valid
    if (shadowShader == 0) {
        std::cerr << "[SceneGraph] ERROR: Invalid shadow shader (0)!" << std::endl;
        return;
    }
    
    // Verify OpenGL state
    GLint currentProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    if (currentProgram != shadowShader) {
        std::cerr << "[SceneGraph] WARNING: Shadow shader not active! Expected=" << shadowShader 
                  << " Current=" << currentProgram << std::endl;
    }
    
    // Check if we're rendering to a framebuffer
    GLint currentFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFBO);
    std::cout << "[SceneGraph] Rendering shadows to FBO=" << currentFBO << std::endl;
    
    // CRITICAL: Actually render the scene for shadows
    std::cout << "[SceneGraph] Starting shadow geometry rendering..." << std::endl;
    
    int renderCount = 0;
    
    // Render all child nodes that have geometry
    for (auto& child : m_root->children) {
        if (child) {
            // Check if this node has renderable geometry
            bool hasGeometry = (child->GetModel() != nullptr) || (!child->children.empty());
            
            if (hasGeometry) {
                std::cout << "[SceneGraph] Rendering shadow for node: " << child->GetName() 
                          << " (type=" << static_cast<int>(child->GetNodeType()) << ")" << std::endl;
                
                child->DrawCascade(glm::mat4(1.0f), lightSpace, shadowShader);
                renderCount++;
            }
        }
    }
    
    std::cout << "[SceneGraph] Shadow cascade rendering complete: " << renderCount << " nodes rendered" << std::endl;
    
    // If we didn't render anything, try an alternative approach
    if (renderCount == 0) {
        std::cout << "[SceneGraph] No geometry rendered - trying alternative geometry pass..." << std::endl;
        
        // Use the geometry pass as fallback for shadow rendering
        glUseProgram(shadowShader);
        
        // Set up uniforms for shadow shader
        GLint locLightSpace = glGetUniformLocation(shadowShader, "lightSpaceMatrix");
        if (locLightSpace >= 0) {
            glUniformMatrix4fv(locLightSpace, 1, GL_FALSE, glm::value_ptr(lightSpace));
        }
        
        // Try to render using the geometry pass
        for (auto& child : m_root->children) {
            if (child && child->GetModel()) {
                std::cout << "[SceneGraph] Fallback shadow render for: " << child->GetName() << std::endl;
                child->DrawGeometry(glm::mat4(1.0f), shadowShader);
                renderCount++;
            }
        }
        
        std::cout << "[SceneGraph] Fallback rendering complete: " << renderCount << " nodes rendered" << std::endl;
    }
}

void SceneGraph::CollectRenderableObjects(MDIBatch& batch)
{
    if (m_root) {
        // OPTIMIZATION: Reserve approximate capacity based on tree size
        // This prevents reallocations during traversal
        size_t estimatedObjects = EstimateRenderableObjectCount();
        if (estimatedObjects > 0) {
            // MDIBatch will handle reservation internally if needed
        }
  
        m_root->CollectRenderableObjects(glm::mat4(1.0f), batch);
    }
}

// OPTIMIZATION: Estimate number of renderable objects for batch reservation
size_t SceneGraph::EstimateRenderableObjectCount() const
{
    if (!m_root) return 0;
  
    size_t count = 0;
    // Count nodes with models (simplified estimation)
    std::function<void(const std::shared_ptr<SceneNode>&)> countNodes;
  countNodes = [&](const std::shared_ptr<SceneNode>& node) {
    if (node && node->GetModel()) {
            // Each model may have multiple meshes
   auto model = node->GetModel();
    count += model->meshes.size();
    }
        for (const auto& child : node->children) {
       countNodes(child);
        }
    };
    
    for (const auto& child : m_root->children) {
        countNodes(child);
    }
    
    return count;
}

//Deferred geometry pass
void SceneGraph::DrawGeometry(GLuint geometryShader)
{
    if (m_root) {
        m_root->DrawGeometry(glm::mat4(1.0f), geometryShader);
    }
}

//Motion vector pass for TAA
void SceneGraph::DrawVelocity(GLuint velocityShader)
{
    if (m_root) {
        m_root->DrawVelocity(glm::mat4(1.0f), glm::mat4(1.0f), velocityShader);
    }
}

std::vector<std::shared_ptr<SceneNode>> SceneGraph::FindNodesByType(SceneNode::NODE_TYPE type)
{
    std::vector<std::shared_ptr<SceneNode>> nodes;
	if (m_root) {
		for (auto& child : m_root->children) {
			if (child->GetNodeType() == type) {
				nodes.push_back(child);
			}
		}
	}
	return nodes;
}

// NEW: Find first LPV volume node in scene
std::shared_ptr<SceneNode> SceneGraph::FindLPVVolumeNode()
{
    if (!m_root) return nullptr;
    
    for (auto& child : m_root->children) {
        if (child && child->GetNodeType() == SceneNode::LPV_VOLUME) {
            return child;
        }
    }
    
    return nullptr;
}

// NEW: Save scene to file
bool SceneGraph::SaveToFile(const std::string& filePath)
{
    std::cout << "[SceneGraph] Saving scene '" << m_sceneName << "' to: " << filePath << std::endl;
    
    // We need a SceneLoader instance to do the actual serialization
    // This is a bit of a circular dependency, but it's the cleanest approach
    // We'll pass nullptr for ModelManager and PhysicsEngine as they're not needed for saving
    
    // Note: This requires including SceneLoader.h in SceneGraph.cpp
    // For now, we'll just log and return false - implementation will be in MainWindow
    
    std::cerr << "[SceneGraph] SaveToFile not fully implemented - use SceneLoader::SaveScene instead" << std::endl;
    return false;
}

//set the skybox
void SceneGraph::SetSkybox(const std::shared_ptr<Skybox>& skybox) {
    m_skybox = skybox;

}
std::shared_ptr<Skybox> SceneGraph::GetSkybox() {
	return m_skybox;
}

std::shared_ptr<SceneNode> SceneGraph::FindNodeByModelName(const std::string& modelName) {
    return FindNodeByModelNameRecursive(m_root, modelName);
}

void SceneGraph::SetSceneName(const std::string& sceneName)
{
    m_sceneName = sceneName;
}

std::shared_ptr<SceneNode> SceneGraph::FindNodeByModelNameRecursive(
    const std::shared_ptr<SceneNode>& node,
    const std::string& modelName)
{
    if (!node) {
        return nullptr;
    }
    if (node->GetModel() && node->GetModel()->GetName() == modelName) {
        return node;
    }
    for (auto& child : node->children) {
        auto found = FindNodeByModelNameRecursive(child, modelName);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

void SceneGraph::PrintMembers() {
    std::cout << "SceneGraph: " << m_sceneName << std::endl;
    for (auto& child : m_root->children) {
        std::cout << "Child Name: " << child->GetName() << std::endl;
        std::cout << "Child Index: " << child->nodeIndex << std::endl;
    }
}

void SceneGraph::Shutdown() {
    if (m_root) {
        m_root->Shutdown();
  for (auto& child : m_root->children) {
     child->Shutdown();
        }
  m_root.reset();
   m_active = false;
    }
 
    // Clear component system
    m_componentManager.Clear();
}

// NEW: Flat iteration rendering methods for cache-friendly performance
void SceneGraph::DrawFlat(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram) {
    // Update transforms first
    m_transformSystem.UpdateTransforms();
    
    glUseProgram(shaderProgram);
    
    // Upload view and projection matrices once
    GLint locView = glGetUniformLocation(shaderProgram, "view");
    GLint locProj = glGetUniformLocation(shaderProgram, "projection");
    if (locView != -1) glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
    if (locProj != -1) glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));
    
    // Iterate through all renderable components directly (cache-friendly)
auto& renderablePool = m_componentManager.GetRenderablePool();
    for (auto& entry : renderablePool) {
        EntityID entityID = entry.entity;
        auto& renderable = entry.component;
        
 // Skip if no model
 if (!renderable.model) continue;
        
        // Get world transform from transform system
const glm::mat4& worldTransform = m_transformSystem.GetWorldTransform(entityID);
        
     // Upload model matrix
        GLint locModel = glGetUniformLocation(shaderProgram, "model");
        if (locModel != -1) {
    glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
        }
        
   // Use custom shader if specified
 GLuint useShader = (renderable.shaderID != 0) ? renderable.shaderID : shaderProgram;
        if (useShader != shaderProgram) {
glUseProgram(useShader);
          // Re-upload matrices for custom shader
          if (locView != -1) glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
          if (locProj != -1) glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));
          if (locModel != -1) glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
        }
        
        // Draw the model (Scene::Draw handles mesh iteration)
        renderable.model->Draw();
        
        // Restore default shader if we switched
        if (useShader != shaderProgram) {
 glUseProgram(shaderProgram);
        }
    }
}

void SceneGraph::DrawCascadeFlat(const glm::mat4& lightSpace, GLuint shadowShader) {
    // Update transforms first
    m_transformSystem.UpdateTransforms();
    
    glUseProgram(shadowShader);
    
    // Upload light space matrix once
    GLint locLS = glGetUniformLocation(shadowShader, "lightSpaceMatrix");
    if (locLS != -1) {
        glUniformMatrix4fv(locLS, 1, GL_FALSE, glm::value_ptr(lightSpace));
    }
    
    // Iterate through all renderable components
    auto& renderablePool = m_componentManager.GetRenderablePool();
    for (auto& entry : renderablePool) {
        auto& renderable = entry.component;
        if (!renderable.model) continue;
   
        // Get world transform
  const glm::mat4& worldTransform = m_transformSystem.GetWorldTransform(entry.entity);
      
  // Upload model matrix
        GLint locModel = glGetUniformLocation(shadowShader, "model");
        if (locModel != -1) {
 glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
   }
        
   // Draw model (positions only for shadow pass)
        renderable.model->Draw();
    }
}

void SceneGraph::DrawGeometryFlat(GLuint geometryShader) {
    // Update transforms first
    m_transformSystem.UpdateTransforms();
    
    glUseProgram(geometryShader);
    
    // Iterate through all renderable components
auto& renderablePool = m_componentManager.GetRenderablePool();
    for (auto& entry : renderablePool) {
        auto& renderable = entry.component;
        if (!renderable.model) continue;
        
        // Get world transform
        const glm::mat4& worldTransform = m_transformSystem.GetWorldTransform(entry.entity);
        
        // Upload model matrix
 GLint locModel = glGetUniformLocation(geometryShader, "model");
    if (locModel != -1) {
   glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
        }
        
     // Handle skinning if needed
        if (renderable.isSkinned) {
// TODO: Upload bone matrices from animation component
       GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning");
            if (locUseSkin != -1) {
      glUniform1i(locUseSkin, 1);
    }
        } else {
    GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning");
        if (locUseSkin != -1) {
glUniform1i(locUseSkin, 0);
          }
        }
   
        // Draw model
      renderable.model->Draw();
    }
}

void SceneGraph::CollectRenderableObjectsFlat(MDIBatch& batch) {
    // Update transforms first
    m_transformSystem.UpdateTransforms();
    
    // Iterate through all renderable components
    auto& renderablePool = m_componentManager.GetRenderablePool();
    for (auto& entry : renderablePool) {
        auto& renderable = entry.component;
        if (!renderable.model) continue;
        
        // Get world transform
        const glm::mat4& worldTransform = m_transformSystem.GetWorldTransform(entry.entity);
   
        // Collect each mesh in the model
        for (const auto& mesh : renderable.model->meshes) {
    MDI_RenderableObject obj{};
         obj.count = static_cast<GLuint>(mesh.indexCount);
            obj.firstIndex = 0;
    obj.baseVertex = 0;
  obj.modelMatrix = worldTransform;
            obj.vao = mesh.VAO;
        
            // Use precomputed bounding volume if available
            if (mesh.boundingVolumeValid) {
        obj.boundingSphere = glm::vec4(mesh.boundingCenter, mesh.boundingRadius);
      } else {
       obj.boundingSphere = glm::vec4(0.0f, 0.0f, 0.0f, renderable.boundingRadius);
    }
   
            batch.AddObject(obj);
        }
    }
}

void SceneGraph::SyncSceneNodeToComponents(std::shared_ptr<SceneNode> node, EntityID parentID) {
    // Helper to sync existing SceneNode hierarchy into component system
    // This is called when loading scenes to populate the component pools
    if (!node) return;
    
    // TODO: Implement when SceneNode facade is complete
    // This will copy data from SceneNode into component pools
}
