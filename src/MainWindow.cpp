#include "MainWindow.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <memory>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <GL/glew.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <IMGUI/imgui.h>
#include <IMGUI/imgui_impl_sdl2.h>
#include <IMGUI/imgui_impl_opengl3.h>
#include <IMGUI/ImGuizmo.h>
#include "json.hpp"
using json = nlohmann::json;

#include "Camera.h"
#include "Scene.h"
#include "ShaderLoader.h"
#include "ModelManager.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "AudioNode.h"
#include "GuiNode.h"
#include "SceneLoader.h"
#include "DirectionalLight.h"
#include "LightManager.h"
#include "LightNode.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "StudioLighting.h"
#include "FrustumCulling.h"
#include "ImGuiInterface.h"
#include "ImguiStyle.h"
#include "SimulationConfig.h"
#include "PhysicsEngine.h"
#include "RayCast.h"
#include "BVH.h"
#include "Input/InputIntegration.h"

MainWindow::MainWindow()
	: m_window(nullptr)
	, m_glContext(nullptr)
	, m_windowWidth(1920)
	, m_windowHeight(1080)
	, m_windowTitle("NOX Engine")
	, m_modelPath("model.gltf")
	, m_fontPath("arial.ttf")
	, m_lightPos(1.2f, 1.0f, 2.0f)
	, m_environmentColor(0.3f, 0.3f, 0.3f)
	, m_shaderProgram(0)
	, m_running(false)
	, m_lastTime(0)
	, m_frameCount(0.0f)
	, m_fpsUpdateTime(0.0f)
	, m_fps(0.0f)
	, m_inputIntegration(std::make_unique<InputIntegration>())
{
}

MainWindow::~MainWindow() {
	Cleanup();
}

bool MainWindow::LoadConfiguration() {
	try {
		std::ifstream configFile("config.json");
		if (!configFile || !configFile.is_open()) {
			std::cerr << "Could not open config.json" << std::endl;
			return false;
		}
		configFile >> m_config;

		m_windowWidth = m_config.value("windowWidth", 1920);
		m_windowHeight = m_config.value("windowHeight", 1080);
		m_windowTitle = m_config.value("windowTitle", "NOX Engine");
		m_modelPath = m_config.value("modelPath", "model.gltf");
		m_fontPath = m_config.value("text_font", "arial.ttf");
		m_iconPath = m_config.value("iconPath", "");

		if (m_config.contains("light") && m_config["light"].is_object()) {
			auto& lightConfig = m_config["light"];
			// the light config contains the light position and direction,shadow bias, near and far planes
			m_shadows_enabled = lightConfig.value("shadows_enabled", true);
			m_shadow_bias = std::min(0.001f, lightConfig.value("shadow_bias", 0.005f));
			m_shadow_near = lightConfig.value("shadow_near", 0.1f);
			m_shadow_far = lightConfig.value("shadow_far", 1000.0f);
			m_shadow_size = lightConfig.value("shadow_size", 1024);
			m_split_lambda = lightConfig.value("split_lambda", 0.95f);
		}

		m_vsync = m_config.value("vsync", true);
		if (m_config.contains("environmentColor") &&
			m_config["environmentColor"].is_array() &&
			m_config["environmentColor"].size() == 3)
		{
			m_environmentColor = glm::vec3(
				m_config["environmentColor"][0],
				m_config["environmentColor"][1],
				m_config["environmentColor"][2]
			);
		}
		else {
			std::cerr << "Invalid or missing environmentColor in config.json\n";
			m_environmentColor = glm::vec3(0.3f, 0.3f, 0.3f);
		}
		glm::vec3 lightPosCopy = glm::vec3(
			m_config["light"]["position"][0],
			m_config["light"]["position"][1],
			m_config["light"]["position"][2]
		);
		m_lightPos = lightPosCopy;

		glm::vec3 lightDirCopy = glm::vec3(
			m_config["light"]["direction"][0],
			m_config["light"]["direction"][1],
			m_config["light"]["direction"][2]
		);
		m_lightDir = lightDirCopy;
	}
	catch (const json::type_error& e) {
		std::cerr << "JSON type error: " << e.what() << std::endl;
		return false;
	}
	catch (const std::exception& e) {
		std::cerr << "Exception loading config.json: " << e.what() << std::endl;
		return false;
	}
	return true;
}


bool MainWindow::Initialize() {
	if (!LoadConfiguration())
		return false;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
		return false;
	}
	if (TTF_Init() < 0) {
		std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
		SDL_Quit();
		return false;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	m_window = SDL_CreateWindow(
		m_windowTitle.c_str(),
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		m_windowWidth,
		m_windowHeight,
		SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
	);
	if (!m_window) {
		std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	m_glContext = SDL_GL_CreateContext(m_window);
	if (!m_glContext) {
		std::cerr << "OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
		SDL_DestroyWindow(m_window);
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	if (!m_iconPath.empty()) {
		SDL_Surface* icon = IMG_Load(m_iconPath.c_str());
		if (icon) {
			SDL_SetWindowIcon(m_window, icon);
			SDL_FreeSurface(icon);
		}
		else {
			std::cerr << "Failed to load window icon: " << IMG_GetError() << std::endl;
		}
	}

	SDL_SetRelativeMouseMode(SDL_TRUE);
	SDL_GL_SetSwapInterval(m_vsync ? 1 : 0);

	glewExperimental = GL_TRUE;
	GLenum glewError = glewInit();
	glGetError(); // Clear any spurious errors
	if (glewError != GLEW_OK) {
		std::cerr << "Error initializing GLEW! " << glewGetErrorString(glewError) << std::endl;
		SDL_GL_DeleteContext(m_glContext);
		SDL_DestroyWindow(m_window);
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	// Initialize ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext);
	ImGui_ImplOpenGL3_Init("#version 460 core");
	SetStyle();

	// Initialize the Light Manager
	m_lightManager = std::make_unique<LightManager>();

	// Compile shadow pass shader
	m_shadowShader = CreateShaderProgram("shaders/shadow_vert.glsl", "shaders/shadow_frag.glsl");
	if (!m_shadowShader) {
		std::cerr << "[MainWindow] Failed to create shadow shader.\n";
		return false;
	}
	
	// Initialize ModularRenderer (NEW ARCHITECTURE)
	std::cout << "[MainWindow] Initializing ModularRenderer..." << std::endl;
	m_modularRenderer = std::make_shared<ModularRenderer>();
	if (!m_modularRenderer->Initialize(m_windowWidth, m_windowHeight)) {
		std::cerr << "[MainWindow] Failed to initialize ModularRenderer.\n";
		return false;
	}
	std::cout << "[MainWindow] ModularRenderer initialized successfully!" << std::endl;
	
	// Set up directional light and cascaded shadow mapping
	m_lighting = std::make_unique<DirectionalLight>();
	
	// Use shadow size from config file
	int configShadowSize = m_config["light"].value("shadow_size", 1024);
	if (!m_lighting->InitializeCascades(configShadowSize, 0.5f)) { // Use lambda 0.5 for better distribution
		std::cerr << "Failed to initialize cascaded shadow mapping.\n";
		return false;
	}
	m_lighting->SetLightPosition(m_lightPos);
	m_lighting->SetLightDirection(m_lightDir);
	m_lighting->SetLightColor(glm::vec3(1.0f, 1.0f, 0.5f));
	m_lighting->SetCascadeSplits(m_shadow_near, m_shadow_far, 2.0f); // Reduced overlap
	m_lighting->SetShadowShaderID(m_shadowShader);
	m_lighting->SetShadowSize(configShadowSize); // Ensure shadow size is set from config

	// Register main directional light with light manager
	m_lightManager->RegisterLight(m_lighting, "MainDirectionalLight");

	// Initialize light manager shadow system with config values
	m_lightManager->InitializeShadowSystem(8, configShadowSize);

	// Create the Camera
	m_camera = std::make_shared<Camera>(
		glm::vec3(m_config["camera"]["position"][0],
			m_config["camera"]["position"][1],
			m_config["camera"]["position"][2]),
		glm::vec3(m_config["camera"]["up"][0],
			m_config["camera"]["up"][1],
			m_config["camera"]["up"][2]),
		m_config["camera"].value("yaw", -90.0f),
		m_config["camera"].value("pitch", 0.0f),
		m_config["camera"].value("fov", 45.0f),
		m_config["camera"].value("near", 0.1f),
		m_config["camera"].value("far", 100.0f),
		m_config["camera"].value("movement_speed", 2.5f),
		m_config["camera"].value("mouse_sensitivity", 0.1f)
	);

	// Immediately set perspective projection based on window size
	{
		float aspect = float(m_windowWidth) / float(m_windowHeight);
		m_camera->SetPerspective(
			m_camera->GetCameraFov(),
			aspect,
			m_camera->GetCameraNearPlane(),
			m_camera->GetCameraFarPlane()
		);
		m_camera->SetProjectionType(Camera::ProjectionType::Perspective);
	}

	// Load the initial scene.
	m_scene_to_load = m_config.value("first_scene", "scenes/scene.json");
	m_modelManager = std::make_shared<ModelManager>();
	m_sceneGraph = std::make_shared<SceneGraph>();

	// Initialize ImGui Interface and scene list.
	m_imguiInterface = std::make_unique<ImGuiInterface>();
	if (m_config.contains("scenes") && m_config["scenes"].is_array()) {
		std::vector<std::string> sceneList;
		for (auto& sceneEntry : m_config["scenes"]) {
			std::string path = sceneEntry.value("path", "");
			if (!path.empty()) {
				sceneList.push_back(path);
			}
		}
		if (m_imguiInterface) {
			m_imguiInterface->SetSceneList(sceneList);
		}
	}
	else {
		std::cerr << "Invalid or missing 'scenes' array in config.json" << std::endl;
	}
	// Initiate the physics engine
	SimulationConfig config;
	if (!config.loadConfig("config.json")) {
		std::cerr << "Using default simulation config.\n";
	}
	m_physicsEngine = std::make_shared<PhysicsEngine>(config);


	// Load the Scene using SceneLoader.
	m_sceneLoader = std::make_shared<SceneLoader>(m_modelManager, m_physicsEngine,m_windowWidth, m_windowHeight);
	m_sceneGraph = m_sceneLoader->LoadScene(m_scene_to_load);
	m_currentSceneFilePath = m_scene_to_load; // Store initial scene file path
	m_exposure = m_sceneGraph->m_exposure;
	m_gamma = m_sceneGraph->m_gamma;
	m_scene_name = m_sceneGraph->GetSceneName();
	// Sync physics enabled state from scene JSON
	m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
	if (m_physicsEngine) {
		if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
	}
	if (!m_sceneGraph || !m_sceneGraph->GetRoot()) {
		std::cerr << "Failed to load initial scene: " << m_scene_to_load << std::endl;
		return false;
	}

	// Collect lights from the loaded scene
	m_lightManager->CollectLightsFromScene(m_sceneGraph);
	m_lightManager->PrintLightInfo();

	// Set ImGui callback for scene swapping.
	m_imguiInterface->SetSceneSwapCallback([this](const std::string& newSceneFile) {
		std::cout << "Swapping scene to: " << newSceneFile << std::endl;
		// Shutdown current scene graph first
		if (m_sceneGraph) {
			m_sceneGraph->Shutdown();
		}
		// Shutdown old physics engine before discarding so bodies are cleared
		if (m_physicsEngine) {
			m_physicsEngine->Shutdown();
		}

		// Clear spatial acceleration structures
		m_sceneBVH.reset();
		m_bvhDirty = true;

		// Reset OpenGL state (unchanged)
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glUseProgram(0);
		for (int i = 0; i < 10; i++) {
			glActiveTexture(GL_TEXTURE0 + i);
			if (i == 9)
				glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
			else
				glBindTexture(GL_TEXTURE_2D, 0);
		}
		glActiveTexture(GL_TEXTURE9);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

		// Recreate physics engine and scene loader BEFORE loading new scene so colliders attach to the new engine
		SimulationConfig config;
		if (!config.loadConfig("config.json")) {
			std::cerr << "Using default simulation config.\n";
		}
		m_physicsEngine = std::make_shared<PhysicsEngine>(config);
		m_sceneLoader = std::make_shared<SceneLoader>(m_modelManager, m_physicsEngine, m_windowWidth, m_windowHeight);

		// Load new scene now with fresh physics engine
		auto tempSceneGraph = m_sceneLoader->LoadScene(newSceneFile);

		if (tempSceneGraph && tempSceneGraph->GetRoot() != nullptr) {
			m_sceneGraph = std::move(tempSceneGraph);
			m_scene_name = m_sceneGraph->GetSceneName();
			m_exposure = m_sceneGraph->m_exposure;
			m_gamma = m_sceneGraph->m_gamma;
			// Apply new scene physics flag
			m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
			if (m_physicsEngine) {
				if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
			}

			// Collect lights from the new scene
			if (m_lightManager) {
				m_lightManager->CollectLightsFromScene(m_sceneGraph);
				m_lightManager->PrintLightInfo();
			}

			ComputeSceneBoundingBox();
			m_imguiInterface->SetSceneGraph(m_sceneGraph);

			// Store current scene file path for saving
			m_currentSceneFilePath = newSceneFile;

			std::cout << "Scene swapped successfully to: " << newSceneFile << std::endl;
			SDL_RestoreWindow(m_window);
			SDL_ShowWindow(m_window);
			SDL_RaiseWindow(m_window);
		}
		else {
			std::cerr << "Failed to load scene: " << newSceneFile << ". Retaining the current scene." << std::endl;
		}
		});
	
	// NEW: Set ImGui callback for scene saving
	m_imguiInterface->SetSceneSaveCallback([this]() {
		if (!m_sceneGraph || !m_sceneLoader) {
			std::cerr << "[MainWindow] Cannot save: scene graph or loader is null" << std::endl;
			return;
		}
		
		std::string saveFilePath = m_currentSceneFilePath.empty() ? m_scene_to_load : m_currentSceneFilePath;
		
		std::cout << "[MainWindow] Saving scene to: " << saveFilePath << std::endl;
		
		if (m_sceneLoader->SaveScene(m_sceneGraph, saveFilePath)) {
			std::cout << "[MainWindow] ? Scene saved successfully!" << std::endl;
			
			// Optional: Show a success notification in ImGui
			// You could add a toast notification system here
		} else {
			std::cerr << "[MainWindow] ? Failed to save scene" << std::endl;
		}
	});

	ComputeSceneBoundingBox();

	// Ensure the ImGui interface has the scene graph
	m_imguiInterface->SetSceneGraph(m_sceneGraph);
	
	// CRITICAL: Connect ModularRenderer to UI for real-time rendering control
	if (m_imguiInterface && m_modularRenderer) {
		std::cout << "[MainWindow] Connecting ModularRenderer to ImGui interface for real-time control..." << std::endl;
		m_imguiInterface->SetModularRenderer(m_modularRenderer);
	}

	// Initialize the unified input system
	if (!m_inputIntegration->Initialize(this)) {
		std::cerr << "Failed to initialize input system" << std::endl;
		return false;
	}
	
	// Set up component references for input integration
	m_inputIntegration->SetCamera(m_camera);
	m_inputIntegration->SetSceneGraph(m_sceneGraph);
	m_inputIntegration->SetImGuiInterface(m_imguiInterface.get());
	m_inputIntegration->SetDirectionalLight(m_lighting);
	m_inputIntegration->SetLightManager(m_lightManager.get());
	
	// Set initial mouse lock state to match current SDL state
	m_inputIntegration->SetMouseLocked(true); // Default mouse locked state

	m_running = true;
	m_lastTime = SDL_GetTicks();
	return true;
}

void MainWindow::ProcessEvents() {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		// CRITICAL FIX: Process input system FIRST, before ImGui can capture events
		bool handledByInput = false;
		if (m_inputIntegration) {
			handledByInput = m_inputIntegration->ProcessEvent(event);
		}
		
		// Only let ImGui process events if input system didn't handle them
		// OR if it's a mouse event and mouse is unlocked (for UI interaction)
		bool allowImGui = !handledByInput || 
		                  (m_inputIntegration && !m_inputIntegration->IsMouseLocked());
		
		if (allowImGui) {
			ImGui_ImplSDL2_ProcessEvent(&event);
		}
		
		// Process ImGui keyboard input for window toggles
		if (m_imguiInterface) {
			m_imguiInterface->ProcessKeyboardInput();
		}
		
		// Handle system-level events that the input system doesn't manage
		switch (event.type) {
		case SDL_QUIT:
			m_running = false;
			break;
		case SDL_KEYDOWN:
			// Handle any remaining hardcoded keys that haven't been moved to input system yet
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				m_running = false;
			}
			// Note: Most key handling is now done by the input system
			break;
		case SDL_MOUSEMOTION:
			// Mouse movement is now handled by the input system when mouse is locked
			break;
		case SDL_MOUSEBUTTONDOWN:
			// Only handle mouse picking if not handled by input system and mouse is unlocked
			if (!handledByInput && event.button.button == SDL_BUTTON_LEFT && 
				m_inputIntegration && !m_inputIntegration->IsMouseLocked()) {
				
				// Check if ImGuizmo is currently being used
				bool gizmoBeingUsed = false;
				if (m_imguiInterface && m_imguiInterface->IsGizmoVisible() && m_imguiInterface->GetSelectedNode()) {
					gizmoBeingUsed = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
				}
				
				if (!gizmoBeingUsed) {
					HandleMouseClick(event.button.x, event.button.y);
				} else {
					std::cout << "[MainWindow] Gizmo is active, skipping mouse picking" << std::endl;
				}
			}
			break;
		case SDL_MOUSEWHEEL:
			// Handle camera zoom (still handled directly for fine control)
			m_camera->ProcessMouseScroll(
				static_cast<float>(event.wheel.y),
				m_config["camera"].value("fov", 45.0f));
			break;
		default:
			break;
		}
	}
}

void MainWindow::Update(float deltaTime) {
	// Update input system (this handles all keyboard/mouse/gamepad input)
	if (m_inputIntegration) {
		m_inputIntegration->Update(deltaTime);
	}
	
	// Note: Camera keyboard input is now handled by the input system
	// const Uint8* keystate = SDL_GetKeyboardState(NULL);
	// m_camera->ProcessKeyboard(keystate, deltaTime);

	// Handle deferred physics toggle
	if (m_requestTogglePhysics && m_physicsEngine) {
		m_physicsEnabledForScene = !m_physicsEnabledForScene;
		if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
		m_requestTogglePhysics = false;
	}

	// Update lighting system
	if (m_lightManager) {
		m_lightManager->UpdateLights(deltaTime);
	}

	m_frameCount += 1.0f;
	m_fpsUpdateTime += deltaTime;
	if (m_fpsUpdateTime >= FPS_UPDATE_INTERVAL) {
		m_fps = m_frameCount / m_fpsUpdateTime;
		m_frameCount = 0.0f;
		m_fpsUpdateTime = 0.0f;
	}

	if (m_sceneGraph && m_sceneGraph->IsActive()) {
		m_sceneGraph->GetRoot()->UpdateAnimation(deltaTime);
		if (m_physicsEngine && m_physicsEnabledForScene) {
			m_physicsEngine->Update(deltaTime);
			float alpha = m_physicsEngine->GetAlpha();
			m_physicsEngine->Interpolate(alpha);
		}
		// Audio update
		glm::vec3 listenerPos = m_camera->GetCameraPosition();
		float listenerAngle = m_camera->GetCameraFacingAngle();
		m_sceneGraph->GetRoot()->UpdateAudioNodes(listenerPos, listenerAngle);
		
		// Rebuild BVH if needed (e.g., after animations or physics updates)
		if (m_bvhDirty) {
			RebuildSceneBVH();
		}
	}
}


void MainWindow::Render(float fps)
{
	if (!m_sceneGraph) return;

	// Use ModularRenderer for all rendering
	if (m_modularRenderer) {
		m_modularRenderer->Render(
			m_sceneGraph,
			m_camera,
			m_lighting,
			m_exposure,
			m_gamma,
			m_shadows_enabled,
			m_shadow_bias,
			m_shadow_near,
			m_shadow_far,
			m_environmentColor,
			m_windowWidth,
			m_windowHeight
		);
	} else {
		std::cerr << "[MainWindow] ERROR: ModularRenderer is not initialized!" << std::endl;
		return;
	}

	// Start ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	
	// Set up ImGui interface data
	m_imguiInterface->SetLighting(m_lighting);
	m_imguiInterface->SetSceneGraph(m_sceneGraph);
	m_imguiInterface->SetCamera(m_camera);
	m_imguiInterface->SetFrameData(m_frameTimeData);
	
	// Render ImGui windows
	m_imguiInterface->Render(
		m_windowWidth,
		m_windowHeight,
		m_fps,
		m_scene_name,
		m_lightPos,
		m_lightDir
	);

	// Render gizmo overlay
	m_imguiInterface->RenderGizmoOverlay(m_windowWidth, m_windowHeight);

    // Finally, render ImGui and swap buffers
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(m_window);
}


void MainWindow::ComputeSceneBoundingBox() {
	if (!m_sceneGraph) {
		std::cerr << "[WARN] Scene graph is null, cannot compute bounding box.\n";
		return;
	}

	glm::vec3 minBounds(std::numeric_limits<float>::max());
	glm::vec3 maxBounds(-std::numeric_limits<float>::max());

	std::function<void(std::shared_ptr<SceneNode>)> TraverseNodes =
		[&](std::shared_ptr<SceneNode> node) {
		if (!node) return;
		if (node->GetModel()) {
			auto [nodeMin, nodeMax] = node->GetBoundingBox();
			minBounds = glm::min(minBounds, nodeMin);
			maxBounds = glm::max(maxBounds, nodeMax);
		}
		for (auto& child : node->children) {
			TraverseNodes(child);
		}
		};
	TraverseNodes(m_sceneGraph->GetRoot());

	m_cachedMinBounds = minBounds;
	m_cachedMaxBounds = maxBounds;
	m_cachedSceneCenter = (minBounds + maxBounds) * 0.5f;
	m_cachedSceneRadius = glm::length(maxBounds - minBounds) * 0.5f;
	m_boundingBoxCached = true;

	// Mark BVH as dirty when scene bounds change
	m_bvhDirty = true;

	std::cout << "[INFO] Scene bounding box cached successfully.\n";
}

void MainWindow::RebuildSceneBVH() {
	if (!m_sceneGraph || !m_sceneGraph->GetRoot()) {
		std::cout << "[BVH] Cannot build BVH: Scene graph is null" << std::endl;
		return;
	}

	std::cout << "[BVH] Building BVH for scene acceleration..." << std::endl;

	// Collect all nodes with models from the scene graph
	std::vector<std::shared_ptr<SceneNode>> nodes;
	std::function<void(std::shared_ptr<SceneNode>)> CollectNodes = [&](std::shared_ptr<SceneNode> node) {
		if (!node) return;
		
		// Only add nodes that have models (renderable geometry)
		if (node->GetModel()) {
			nodes.push_back(node);
		}
		
		// Traverse children
		for (auto& child : node->children) {
			CollectNodes(child);
		}
	};

	CollectNodes(m_sceneGraph->GetRoot());

	if (nodes.empty()) {
		std::cout << "[BVH] No nodes with models found, skipping BVH build" << std::endl;
		m_sceneBVH.reset();
		m_bvhDirty = false;
		return;
	}

	// Create BVH
	m_sceneBVH = BVH::CreateSceneNodeBVH(nodes);
	
	// Print statistics
	if (m_sceneBVH) {
		auto stats = m_sceneBVH->GetStatistics();
		std::cout << "[BVH] Built successfully:" << std::endl;
		std::cout << "  - Nodes: " << stats.nodeCount << std::endl;
		std::cout << "  - Leaves: " << stats.leafCount << std::endl;
		std::cout << "  - Objects: " << stats.objectCount << std::endl;
		std::cout << "  - Max Depth: " << stats.maxDepth << std::endl;
		std::cout << "  - Avg Objects/Leaf: " << stats.avgObjectsPerLeaf << std::endl;
	}

	m_bvhDirty = false;
}

void MainWindow::HandleMouseClick(int mouseX, int mouseY) {
	if (!m_sceneGraph || !m_camera) {
		std::cout << "[MousePicking] Error: Scene graph or camera is null" << std::endl;
		return;
	}

	std::cout << "[MousePicking] === LIGHT-PRIORITY MOUSE CLICK ===" << std::endl;
	std::cout << "[MousePicking] Mouse clicked at (" << mouseX << ", " << mouseY << ")" << std::endl;

	// Convert screen coordinates to world ray using new RayCast system
	RayCast::Ray ray = RayCast::ScreenToWorldRay(mouseX, mouseY, m_camera, m_windowWidth, m_windowHeight);

	std::cout << "[MousePicking] Ray origin: (" << ray.origin.x << ", " << ray.origin.y << ", " << ray.origin.z << ")" << std::endl;
	std::cout << "[MousePicking] Ray direction: (" << ray.direction.x << ", " << ray.direction.y << ", " << ray.direction.z << ")" << std::endl;

	// Validate ray direction
	if (glm::length(ray.direction) < 0.1f) {
		std::cout << "[MousePicking] Invalid ray direction, length too small" << std::endl;
		return;
	}

	// Perform ray query using light-priority system
	auto hitNode = PerformRayQuery(ray);
	
	if (hitNode && m_imguiInterface) {
		// Check if this is a light node for special handling
		bool isLightNode = (hitNode->GetNodeType() == SceneNode::LIGHT);
		
		// Don't select the same node twice
		if (hitNode != m_imguiInterface->GetSelectedNode()) {
			m_imguiInterface->SetSelectedNode(hitNode);
			
			if (isLightNode) {
				std::cout << "[MainWindow] *** LIGHT NODE SELECTED *** : " << hitNode->GetName() << std::endl;
				
				// Additional light-specific logging
				if (auto lightNode = std::dynamic_pointer_cast<LightNode>(hitNode)) {
					if (lightNode->GetLight()) {
						auto light = lightNode->GetLight();
						std::cout << "[MainWindow] Light details: "
								  << "Type=" << static_cast<int>(light->GetLightType())
								  << ", Intensity=" << light->GetIntensity()
								  << ", Enabled=" << (light->IsEnabled() ? "Yes" : "No") << std::endl;
					}
				}
			} else {
				std::cout << "[MainWindow] Selected geometry node: " << hitNode->GetName() << std::endl;
			}
		} else {
			std::cout << "[MainWindow] Node already selected: " << hitNode->GetName() << std::endl;
		}
	} else if (m_imguiInterface) {
		// Only deselect if we didn't click in empty space near the current selection
		if (m_imguiInterface->GetSelectedNode()) {
			std::cout << "[MainWindow] Clicked in empty space, keeping current selection" << std::endl;
		} else {
			m_imguiInterface->SetSelectedNode(nullptr);
			std::cout << "[MainWindow] No node selected" << std::endl;
		}
	}
	
	std::cout << "[MousePicking] === SELECTION COMPLETE ===" << std::endl;
}

std::shared_ptr<SceneNode> MainWindow::PerformRayQuery(const RayCast::Ray& ray) {
	// Rebuild BVH if needed
	if (m_bvhDirty) {
		RebuildSceneBVH();
	}

	std::shared_ptr<SceneNode> closestNode = nullptr;
	float closestDistance = std::numeric_limits<float>::max();

	// CRITICAL FIX: Check for light intersections FIRST with proper priority
	if (m_lightManager) {
		// Update light proxies for current frame
		m_lightManager->UpdateLightProxies();
		
		// Check if ray intersects any light proxy - LIGHTS GET SELECTION PRIORITY
		auto lightNode = m_lightManager->FindLightAtRay(ray.origin, ray.direction);
		if (lightNode) {
			std::cout << "[MousePicking] PRIORITY: Selected light node" << std::endl;
			return lightNode; // Lights have absolute priority over geometry
		}
	}

	// ENHANCED: Use advanced GizmoRayCast system for precise selection
	if (m_sceneBVH && !m_sceneBVH->Empty()) {
		std::cout << "[MousePicking] Using enhanced GizmoRayCast with precision selection" << std::endl;
		
		// Configure enhanced selection with small object bias
		GizmoRayCast::GizmoConfig config;
		config.enableBVHAcceleration = true;
		config.prioritizeSelectedNode = true;
		config.maxSelectionDistance = 1000.0f;
		config.preferSmallerObjects = true;
		config.smallObjectBonus = 2.5f; // Strong bias toward smaller objects
		config.precisionTolerance = 0.05f; // Tight precision tolerance
		config.enableExpandedRaySearch = true;
		config.rayExpansionRadius = 0.15f; // Slightly larger expansion for better small object detection
		config.enableMultiPassSelection = true;
		
		// Get current selection for bias
		auto currentSelection = (m_imguiInterface ? m_imguiInterface->GetSelectedNode() : nullptr);
		
		// Use enhanced gizmo-aware ray casting
		auto gizmoResult = GizmoRayCast::QueryForGizmoSelection(ray, m_camera, m_sceneBVH.get(), currentSelection, config);
		
		if (gizmoResult.hit && gizmoResult.node) {
			std::cout << "[MousePicking] Enhanced selection: " << gizmoResult.node->GetName() 
					  << " at distance " << gizmoResult.distanceToCamera << std::endl;
			return gizmoResult.node;
		}
		
		// Fallback to traditional BVH query if enhanced selection failed
		std::cout << "[MousePicking] Enhanced selection failed, falling back to traditional BVH" << std::endl;
		auto candidates = m_sceneBVH->QueryRay(ray);
		std::cout << "[MousePicking] BVH fallback returned " << candidates.size() << " candidates" << std::endl;
		
		// ENHANCED: Apply small object bias to traditional selection too
		struct CandidateInfo {
			std::shared_ptr<SceneNode> node;
			float distance;
			float size;
			float priority;
		};
		
		std::vector<CandidateInfo> validCandidates;
		
		// Test each candidate for actual intersection with size bias
		for (auto& node : candidates) {
			if (!node || !node->GetModel()) continue;
			
			// Calculate world transform for the node
			glm::mat4 worldTransform = node->GetTransform();
			
			// Test intersection using the RayCast system
			RayCast::HitResult result = RayCast::RayIntersectNode(ray, node, worldTransform);
			
			if (result.hit) {
				CandidateInfo info;
				info.node = node;
				info.distance = result.distance;
				
				// Calculate object size for bias
				auto [minBounds, maxBounds] = node->GetBoundingBox();
				glm::vec3 size = maxBounds - minBounds;
				info.size = glm::length(size);
				
				// Calculate priority: closer distance + smaller size = higher priority
				float distancePriority = 1000.0f - result.distance; // Closer = higher
				float sizePriority = 100.0f / (info.size + 1.0f);   // Smaller = higher
				info.priority = distancePriority + sizePriority * config.smallObjectBonus;
				
				validCandidates.push_back(info);
			}
		}
		
		// Sort by priority (higher = better)
		if (!validCandidates.empty()) {
			std::sort(validCandidates.begin(), validCandidates.end(),
				[](const CandidateInfo& a, const CandidateInfo& b) {
					return a.priority > b.priority;
				});
			
			closestNode = validCandidates[0].node;
			std::cout << "[MousePicking] Fallback selected: " << closestNode->GetName() 
					  << " (distance: " << validCandidates[0].distance 
					  << ", size: " << validCandidates[0].size 
					  << ", priority: " << validCandidates[0].priority << ")" << std::endl;
		}
	} else {
		std::cout << "[MousePicking] Falling back to brute force traversal" << std::endl;
		
		// Fallback to brute force scene traversal
		RayCast::HitResult result = RayCast::RayIntersectScene(ray, m_sceneGraph->GetRoot());
		if (result.hit) {
			closestNode = result.node;
		}
	}
	
	if (closestNode) {
		std::cout << "[MousePicking] Final selection: " << closestNode->GetName() << std::endl;
	} else {
		std::cout << "[MousePicking] No intersection found" << std::endl;
	}
	
	return closestNode;
}

// Legacy compatibility functions (keep existing behavior for backward compatibility)
glm::vec3 MainWindow::ScreenToWorldRay(int mouseX, int mouseY) {
	RayCast::Ray ray = RayCast::ScreenToWorldRay(mouseX, mouseY, m_camera, m_windowWidth, m_windowHeight);
	return ray.direction;
}

std::shared_ptr<SceneNode> MainWindow::RayIntersectScene(const glm::vec3& rayOrigin, const glm::vec3& rayDirection) {
	RayCast::Ray ray(rayOrigin, rayDirection);
	RayCast::HitResult result = RayCast::RayIntersectScene(ray, m_sceneGraph->GetRoot());
	return result.node;
}

bool MainWindow::RayIntersectNode(const std::shared_ptr<SceneNode>& node, const glm::vec3& rayOrigin, const glm::vec3& rayDirection, const glm::mat4& worldTransform) {
	RayCast::Ray ray(rayOrigin, rayDirection);
	RayCast::HitResult result = RayCast::RayIntersectNode(ray, node, worldTransform);
	return result.hit;
}

bool MainWindow::RayIntersectAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
	RayCast::Ray ray(rayOrigin, rayDirection);
	RayCast::HitResult result = RayCast::RayIntersectAABB(ray, aabbMin, aabbMax);
	return result.hit;
}

void MainWindow::Run() {
	while (m_running) {
		Uint32 currentTime = SDL_GetTicks();
		float deltaTime = (currentTime - m_lastTime) / 1000.0f;
		m_lastTime = currentTime;
		float fps = (deltaTime > 0.0f) ? 1.0f / deltaTime : 0.0f;
		//frame time in milliseconds
		m_frameTime = deltaTime * 1000.0f;
		if (m_frameTime > 1000.0f) m_frameTime = 1000.0f;
		if (m_frameTime < 0.0f) m_frameTime = 0.0f;
		constexpr size_t maxSamples = 200;
		constexpr size_t smoothWindow = 10;

		m_frameTimeData.push_back(m_frameTime);

		// Apply moving average smoothing
		if (m_frameTimeData.size() >= smoothWindow) {
			float smoothed = 0.0f;
			for (size_t i = m_frameTimeData.size() - smoothWindow; i < m_frameTimeData.size(); ++i) {
				smoothed += m_frameTimeData[i];
			}
			smoothed /= smoothWindow;
			m_frameTimeData.back() = smoothed;
		}

		// Cap data size to avoid memory bloat
		if (m_frameTimeData.size() > maxSamples) {
			m_frameTimeData.erase(m_frameTimeData.begin());
		}

		ProcessEvents();
		Update(deltaTime);
		Render(fps);
	}
}

void MainWindow::Cleanup() {
	// Shutdown input system
	if (m_inputIntegration) {
		m_inputIntegration->Shutdown();
	}
	
	if (ImGui::GetCurrentContext()) {
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();
	}
	if (m_sceneGraph) {
		m_sceneGraph->GetRoot()->children.clear();
	}
	if (m_shaderProgram)  glDeleteProgram(m_shaderProgram);
	if (m_shadowShader)   glDeleteProgram(m_shadowShader);
	if (m_physicsEngine) m_physicsEngine->RemoveAllBodies();
	if (m_glContext)      SDL_GL_DeleteContext(m_glContext);
	if (m_window)         SDL_DestroyWindow(m_window);

	TTF_Quit();
	SDL_Quit();
}

unsigned int MainWindow::CreateShaderProgram(const std::string& vertexPath, const std::string& fragmentPath) {
	return ::CreateShaderProgram(vertexPath.c_str(), fragmentPath.c_str());
}
