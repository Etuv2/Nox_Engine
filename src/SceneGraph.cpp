#include "SceneGraph.h"
#include "SceneNode.h"
#include "Scene.h"
#include "LightManager.h"
#include "LightNode.h"
#include <glm/gtc/type_ptr.hpp>
#include <GL/glew.h>
#include <iostream>

SceneGraph::SceneGraph()
	: m_transformSystem(&m_componentManager),
	  m_renderSystem(&m_componentManager, &m_transformSystem),
	  m_animationSystem(&m_componentManager, &m_transformSystem),
	  m_hierarchySystem(&m_componentManager, &m_transformSystem)
{
	m_lightManager = std::make_shared<LightManager>();
	m_root = SceneNode::CreateWithECS(&m_componentManager, &m_transformSystem, "Root", NodeType::NODE);
	if (!m_root) {
		m_root = std::make_shared<SceneNode>(&m_componentManager, &m_transformSystem);
	}
	m_root->SetTransform(glm::mat4(1.0f));
	m_active = true;
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

// Update all transforms in one batch using component system
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

	// Check if scene has any nodes at all
	if (!m_root) {
		std::cerr << "[SceneGraph] ERROR: No root node exists!" << std::endl;
		return;
	}

	int totalNodes = static_cast<int>(m_root->children.size());
	std::cout << "[SceneGraph] Scanning " << totalNodes << " root children for lights..." << std::endl;

	// Find all LightNode objects in the scene
	auto lightNodes = FindNodesByType(SceneNode::LIGHT);

	std::cout << "[SceneGraph] Found " << lightNodes.size() << " light nodes in scene" << std::endl;

	// If no lights found, create a default directional light
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

			// Check light properties
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

	// Report shadow casting status
	size_t shadowCastingLights = m_lightManager->GetShadowCastingLightCount();
	std::cout << "[SceneGraph] Shadow-casting lights: " << shadowCastingLights << std::endl;

	if (shadowCastingLights == 0) {
		std::cout << "[SceneGraph] WARNING: No lights are set to cast shadows!" << std::endl;
	}
}

// Legacy compatibility shim: delegates to ECS RenderSystem (tooling/import use only)
void SceneGraph::Draw(
	const glm::mat4& view,
	const glm::mat4& projection,
	GLuint shaderProgram)
{
	// Delegate to authoritative ECS API
	m_renderSystem.RenderForward(view, projection, shaderProgram);
}

void SceneGraph::DrawCascade(
	const glm::mat4& lightSpace,
	GLuint shadowShader)
{
	// Delegate to authoritative ECS API
	m_renderSystem.RenderShadowCascade(lightSpace, shadowShader);
}

void SceneGraph::CollectRenderableObjects(MDIBatch& batch)
{
	// Delegate to authoritative ECS API
	m_renderSystem.CollectRenderables(batch);
}

// OPTIMIZATION: Estimate number of renderable objects for batch reservation
size_t SceneGraph::EstimateRenderableObjectCount() const
{
	return m_componentManager.GetRenderablePool().Size();
}

//Deferred geometry pass
void SceneGraph::DrawGeometry(GLuint geometryShader)
{
	// Delegate to authoritative ECS API
	m_renderSystem.RenderGeometry(geometryShader);
}

//Motion vector pass for TAA
void SceneGraph::DrawVelocity(GLuint velocityShader)
{
	// Delegate to authoritative ECS API with identity matrices for legacy compatibility
	m_renderSystem.RenderVelocity(glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), velocityShader);
}

std::vector<std::shared_ptr<SceneNode>> SceneGraph::FindNodesByType(SceneNode::NODE_TYPE type)
{
	std::vector<std::shared_ptr<SceneNode>> nodes;
	if (!m_root) return nodes;

	// Recursive lambda to traverse entire scene graph
	std::function<void(const std::shared_ptr<SceneNode>&)> traverse;
	traverse = [&](const std::shared_ptr<SceneNode>& node) {
		if (!node) return;

		// Check if this node matches the type
		if (node->GetNodeType() == type) {
			nodes.push_back(node);
		}

		// Recursively search all children
		for (auto& child : node->children) {
			traverse(child);
		}
	};

	// Start traversal from root's children
	for (auto& child : m_root->children) {
		traverse(child);
	}

	return nodes;
}

// Find first LPV volume node in scene
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

// Save scene to file
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

void SceneGraph::SyncSceneNodeToComponents(std::shared_ptr<SceneNode> node, EntityID parentID) {
	// Helper to sync existing SceneNode hierarchy into component system
	// This is called when loading scenes to populate the component pools
	if (!node) return;

	// Create ECS entity for this node if it doesn't have one
	if (node->GetEntityID() == INVALID_ENTITY) {
		node->CreateECSEntity();
	}
	
	// Update parent relationship
	if (parentID != INVALID_ENTITY) {
		TransformComponent* transform = node->GetTransformComponent();
		if (transform) {
			transform->parentID = parentID;
		}
	}
	
	// Recursively sync children
	EntityID myID = node->GetEntityID();
	for (auto& child : node->children) {
		SyncSceneNodeToComponents(child, myID);
	}
}

// NEW ECS-BASED RENDERING METHODS

void SceneGraph::RenderForward(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram) {
	m_renderSystem.RenderForward(view, projection, shaderProgram);
}

void SceneGraph::RenderShadowCascade(const glm::mat4& lightSpace, GLuint shadowShader) {
	m_renderSystem.RenderShadowCascade(lightSpace, shadowShader);
}

void SceneGraph::RenderGeometry(GLuint geometryShader) {
	m_renderSystem.RenderGeometry(geometryShader);
}

void SceneGraph::RenderVelocity(const glm::mat4& view, const glm::mat4& projection,
                                 const glm::mat4& prevView, const glm::mat4& prevProjection,
                                 GLuint velocityShader) {
	m_renderSystem.RenderVelocity(view, projection, prevView, prevProjection, velocityShader);
}

void SceneGraph::RenderTransparent(const glm::mat4& view, const glm::mat4& projection, GLuint shader) {
	m_renderSystem.RenderTransparent(view, projection, shader);
}

void SceneGraph::CollectRenderables(MDIBatch& batch) {
	m_renderSystem.CollectRenderables(batch);
}

void SceneGraph::SetFrustumPlanes(const glm::mat4& viewProjection) {
	m_renderSystem.SetFrustumPlanes(viewProjection);
}

// ANIMATION METHODS

void SceneGraph::UpdateAnimations(float deltaTime) {
	m_animationSystem.Update(deltaTime);
}

void SceneGraph::PlayAnimation(EntityID entity, int animationIndex, bool loop) {
	m_animationSystem.PlayAnimation(entity, animationIndex, loop);
}

void SceneGraph::StopAnimation(EntityID entity) {
	m_animationSystem.StopAnimation(entity);
}
