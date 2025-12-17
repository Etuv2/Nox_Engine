#include "Core.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <limits>

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

#include "Camera.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "SceneLoader.h"
#include "ModelManager.h"
#include "DirectionalLight.h"
#include "LightManager.h"
#include "LightNode.h"
#include "ModularRenderer.h"
#include "ImGuiInterface.h"
#include "ImguiStyle.h"
#include "SimulationConfig.h"
#include "PhysicsEngine.h"
#include "RayCast.h"
#include "BVH.h"
#include "GizmoRayCast.h"
#include "ShaderLoader.h"
#include "Input/InputIntegration.h"
#include "PerformanceRecorder.h"
#include "RuntimeStateManager.h"

Core::Core()
	: m_window(nullptr)
	, m_glContext(nullptr)
	, m_windowWidth(1920)
	, m_windowHeight(1080)
	, m_modelPath("model.gltf")
	, m_fontPath("arial.ttf")
	, m_vsync(true)
	, m_shaderProgram(0)
	, m_shadowShader(0)
	, m_environmentColor(0.3f, 0.3f, 0.3f)
	, m_lightPos(1.2f, 1.0f, 2.0f)
	, m_lightDir(0.0f, -1.0f, 0.0f)
	, m_shadowsEnabled(true)
	, m_shadowBias(0.005f)
	, m_shadowNear(0.1f)
	, m_shadowFar(1000.0f)
	, m_shadowSize(1024)
	, m_splitLambda(0.95f)
	, m_exposure(1.0f)
	, m_gamma(2.2f)
	, m_physicsEnabledForScene(false)
	, m_requestTogglePhysics(false)
	, m_frameCount(0.0f)
	, m_fpsUpdateTime(0.0f)
	, m_fps(0.0f)
	, m_frameTime(0.0f)
	, m_frameTimeIndex(0)
	, m_frameTimeCount(0)
	, m_boundingBoxCached(false)
	, m_bvhDirty(true)
{
	m_frameTimeBuffer.fill(0.0f);
}

Core::~Core() {
	Shutdown();
}

bool Core::LoadConfiguration() {
	try {
		std::ifstream configFile(".\\config\\config.json");
		if (!configFile || !configFile.is_open()) {
			std::cerr << "[Core] Could not open config.json" << std::endl;
			return false;
		}
		configFile >> m_config;

		m_modelPath = m_config.value("modelPath", "model.gltf");
		m_fontPath = m_config.value("text_font", "arial.ttf");
		m_iconPath = m_config.value("iconPath", "");
		m_vsync = m_config.value("vsync", true);

		if (m_config.contains("light") && m_config["light"].is_object()) {
			auto& lightConfig = m_config["light"];
			m_shadowsEnabled = lightConfig.value("shadows_enabled", true);
			m_shadowBias = std::min(0.001f, lightConfig.value("shadow_bias", 0.005f));
			m_shadowNear = lightConfig.value("shadow_near", 0.1f);
			m_shadowFar = lightConfig.value("shadow_far", 1000.0f);
			m_shadowSize = lightConfig.value("shadow_size", 1024);
			m_splitLambda = lightConfig.value("split_lambda", 0.95f);
		}

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
			std::cerr << "[Core] Invalid or missing environmentColor in config.json\n";
			m_environmentColor = glm::vec3(0.3f, 0.3f, 0.3f);
		}

		m_lightPos = glm::vec3(
			m_config["light"]["position"][0],
			m_config["light"]["position"][1],
			m_config["light"]["position"][2]
		);

		m_lightDir = glm::vec3(
			m_config["light"]["direction"][0],
			m_config["light"]["direction"][1],
			m_config["light"]["direction"][2]
		);
	}
	catch (const json::type_error& e) {
		std::cerr << "[Core] JSON type error: " << e.what() << std::endl;
		return false;
	}
	catch (const std::exception& e) {
		std::cerr << "[Core] Exception loading config.json: " << e.what() << std::endl;
		return false;
	}
	return true;
}

bool Core::Initialize(SDL_Window* window, SDL_GLContext glContext, int windowWidth, int windowHeight) {
	m_window = window;
	m_glContext = glContext;
	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;

	std::cout << "[Core] Initializing engine subsystems..." << std::endl;

	if (!LoadConfiguration()) {
		std::cerr << "[Core] Failed to load configuration" << std::endl;
		return false;
	}

	if (!InitializeGraphics()) {
		std::cerr << "[Core] Failed to initialize graphics" << std::endl;
		return false;
	}

	if (!InitializeLighting()) {
		std::cerr << "[Core] Failed to initialize lighting" << std::endl;
		return false;
	}

	if (!InitializeCamera()) {
		std::cerr << "[Core] Failed to initialize camera" << std::endl;
		return false;
	}

	if (!InitializePhysics()) {
		std::cerr << "[Core] Failed to initialize physics" << std::endl;
		return false;
	}

	if (!InitializeScene()) {
		std::cerr << "[Core] Failed to initialize scene" << std::endl;
		return false;
	}

	if (!InitializeUI()) {
		std::cerr << "[Core] Failed to initialize UI" << std::endl;
		return false;
	}

	if (!InitializeInput()) {
		std::cerr << "[Core] Failed to initialize input" << std::endl;
		return false;
	}

	std::cout << "[Core] Engine initialization complete!" << std::endl;
	return true;
}

bool Core::InitializeGraphics() {
	std::cout << "[Core] Initializing graphics..." << std::endl;

	// Initialize ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext);
	ImGui_ImplOpenGL3_Init("#version 460 core");
	SetStyle();

	// Compile shadow pass shader
	m_shadowShader = CreateShaderProgram("shaders/shadow_vert.glsl", "shaders/shadow_frag.glsl");
	if (!m_shadowShader) {
		std::cerr << "[Core] Failed to create shadow shader.\n";
		return false;
	}

	// Initialize ModularRenderer
	m_modularRenderer = std::make_shared<ModularRenderer>();
	if (!m_modularRenderer->Initialize(m_windowWidth, m_windowHeight)) {
		std::cerr << "[Core] Failed to initialize ModularRenderer.\n";
		return false;
	}
	std::cout << "[Core] ModularRenderer initialized successfully!" << std::endl;

	return true;
}

bool Core::InitializeLighting() {
	std::cout << "[Core] Initializing lighting..." << std::endl;

	// Initialize the Light Manager
	m_lightManager = std::make_unique<LightManager>();

	// Set up directional light and cascaded shadow mapping
	m_lighting = std::make_shared<DirectionalLight>();

	int configShadowSize = m_config["light"].value("shadow_size", 1024);
	if (!m_lighting->InitializeCascades(configShadowSize, 0.5f)) {
		std::cerr << "[Core] Failed to initialize cascaded shadow mapping.\n";
		return false;
	}
	m_lighting->SetLightPosition(m_lightPos);
	m_lighting->SetLightDirection(m_lightDir);
	m_lighting->SetLightColor(glm::vec3(1.0f, 1.0f, 0.5f));
	m_lighting->SetCascadeSplits(m_shadowNear, m_shadowFar, 2.0f);
	m_lighting->SetShadowShaderID(m_shadowShader);
	m_lighting->SetShadowSize(configShadowSize);

	// Register main directional light with light manager
	m_lightManager->RegisterLight(m_lighting, "MainDirectionalLight");

	// Initialize light manager shadow system with config values
	m_lightManager->InitializeShadowSystem(8, configShadowSize);

	return true;
}

bool Core::InitializeCamera() {
	std::cout << "[Core] Initializing camera..." << std::endl;

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

	// Set perspective projection based on window size
	float aspect = float(m_windowWidth) / float(m_windowHeight);
	m_camera->SetPerspective(
		m_camera->GetCameraFov(),
		aspect,
		m_camera->GetCameraNearPlane(),
		m_camera->GetCameraFarPlane()
	);
	m_camera->SetProjectionType(Camera::ProjectionType::Perspective);

	return true;
}

bool Core::InitializePhysics() {
	std::cout << "[Core] Initializing physics..." << std::endl;

	SimulationConfig config;
	if (!config.loadConfig(".\\config\\config.json")) {
		std::cerr << "[Core] Using default simulation config.\n";
	}
	m_physicsEngine = std::make_shared<PhysicsEngine>(config);

	return true;
}

bool Core::InitializeScene() {
	std::cout << "[Core] Initializing scene..." << std::endl;

	m_sceneToLoad = m_config.value("first_scene", "scenes/scene.json");
	m_modelManager = std::make_shared<ModelManager>();

	// Load the Scene using SceneLoader
	m_sceneLoader = std::make_shared<SceneLoader>(m_modelManager, m_physicsEngine, m_windowWidth, m_windowHeight);
	m_sceneGraph = m_sceneLoader->LoadScene(m_sceneToLoad);
	m_currentSceneFilePath = m_sceneToLoad;

	if (!m_sceneGraph || !m_sceneGraph->GetRoot()) {
		std::cerr << "[Core] Failed to load initial scene: " << m_sceneToLoad << std::endl;
		return false;
	}

	m_exposure = m_sceneGraph->m_exposure;
	m_gamma = m_sceneGraph->m_gamma;
	m_sceneName = m_sceneGraph->GetSceneName();

	// Sync physics enabled state from scene JSON
	m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
	if (m_physicsEngine) {
		if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
	}

	// Collect lights from the loaded scene
	m_lightManager->CollectLightsFromScene(m_sceneGraph);
	m_lightManager->PrintLightInfo();

	ComputeSceneBoundingBox();

	return true;
}

bool Core::InitializeUI() {
	std::cout << "[Core] Initializing UI..." << std::endl;

	m_imguiInterface = std::make_unique<ImGuiInterface>();

	// Setup scene list
	if (m_config.contains("scenes") && m_config["scenes"].is_array()) {
		std::vector<std::string> sceneList;
		for (auto& sceneEntry : m_config["scenes"]) {
			std::string path = sceneEntry.value("path", "");
			if (!path.empty()) {
				sceneList.push_back(path);
			}
		}
		m_imguiInterface->SetSceneList(sceneList);
	}
	else {
		std::cerr << "[Core] Invalid or missing 'scenes' array in config.json" << std::endl;
	}

	// Set ImGui callback for scene swapping
	m_imguiInterface->SetSceneSwapCallback([this](const std::string& newSceneFile) {
		SwapScene(newSceneFile);
		});

	// Set ImGui callback for scene saving
	m_imguiInterface->SetSceneSaveCallback([this]() {
		SaveCurrentScene();
		});

	// Initialize PerformanceRecorder
	m_performanceRecorder = std::make_unique<PerformanceRecorder>();
	std::cout << "[Core] PerformanceRecorder initialized" << std::endl;

	// Initialize RuntimeStateManager
	m_stateManager = std::make_unique<RuntimeStateManager>();
	std::cout << "[Core] RuntimeStateManager initialized" << std::endl;
	
	// Set current scene file path for proper state tracking
	if (m_stateManager) {
		m_stateManager->SetCurrentSceneFilePath(m_sceneToLoad);
	}
	
	// Set physics engine for gizmo interaction
	if (m_physicsEngine) {
		m_imguiInterface->SetPhysicsEngine(m_physicsEngine);
	}

	// Set up state export and performance recording callbacks
	m_imguiInterface->SetStateExportCallbacks(
		[this](const std::string& filepath) { return SaveSceneState(filepath); },
		[this](const std::string& filepath) { return LoadSceneState(filepath); },
		[this]() { return QuickSave(); },
		[this](const std::string& filepath) { return ExportPerformanceCSV(filepath); },
		[this](const std::string& filepath) { return ExportPerformanceJSON(filepath); },
		[this]() { StartPerformanceRecording(); },
		[this]() { StopPerformanceRecording(); }
	);
	std::cout << "[Core] State export callbacks configured" << std::endl;

	// Set scene graph reference
	m_imguiInterface->SetSceneGraph(m_sceneGraph);

	// Connect ModularRenderer to UI for real-time rendering control
	if (m_modularRenderer) {
		m_imguiInterface->SetModularRenderer(m_modularRenderer);
	}

	return true;
}

bool Core::InitializeInput() {
	std::cout << "[Core] Initializing input system..." << std::endl;

	m_inputIntegration = std::make_unique<InputIntegration>();

	if (!m_inputIntegration->Initialize(this)) {
		std::cerr << "[Core] Failed to initialize input system" << std::endl;
		return false;
	}

	// Set up component references for input integration
	m_inputIntegration->SetCamera(m_camera);
	m_inputIntegration->SetSceneGraph(m_sceneGraph);
	m_inputIntegration->SetImGuiInterface(m_imguiInterface.get());
	m_inputIntegration->SetDirectionalLight(m_lighting);
	m_inputIntegration->SetLightManager(m_lightManager.get());

	// Set initial mouse lock state
	m_inputIntegration->SetMouseLocked(true);

	return true;
}

void Core::Shutdown() {
	std::cout << "[Core] Shutting down..." << std::endl;

	// Cleanup in reverse order of initialization
	if (m_inputIntegration) {
		m_inputIntegration.reset();
	}

	if (m_imguiInterface) {
		m_imguiInterface.reset();
	}

	if (m_modularRenderer) {
		m_modularRenderer.reset();
	}

	if (m_physicsEngine) {
		m_physicsEngine.reset();
	}

	m_sceneGraph.reset();
	m_sceneLoader.reset();
	m_modelManager.reset();
	m_lighting.reset();
	m_camera.reset();
	m_lightManager.reset();
	m_sceneBVH.reset();
	m_performanceRecorder.reset();
	m_stateManager.reset();

	std::cout << "[Core] Shutdown complete" << std::endl;
}

void Core::Update(float deltaTime) {
	// Update input system
	if (m_inputIntegration) {
		m_inputIntegration->Update(deltaTime);
	}

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

	// Update FPS counter
	m_frameCount += 1.0f;
	m_fpsUpdateTime += deltaTime;
	if (m_fpsUpdateTime >= FPS_UPDATE_INTERVAL) {
		m_fps = m_frameCount / m_fpsUpdateTime;
		m_frameCount = 0.0f;
		m_fpsUpdateTime = 0.0f;
	}

	// Update scene
	if (m_sceneGraph && m_sceneGraph->IsActive()) {
		// Use new transform-aware animation update
		m_sceneGraph->GetRoot()->UpdateAnimationWithTransform(deltaTime, glm::mat4(1.0f));

		if (m_physicsEngine && m_physicsEnabledForScene) {
			// Physics synchronization is now centralized inside PhysicsEngine::Update()
			// PreStepSync runs before stepping to push kinematic bodies
			// PostStepSync runs after stepping to pull dynamic bodies
			// Step physics simulation
			m_physicsEngine->Update(deltaTime);
			
			// Update all transforms in the scene graph after physics changes
			// This ensures the ECS transform system processes the physics updates
			m_sceneGraph->UpdateAllTransforms();
		}

		// Use new transform-aware audio update
		glm::vec3 listenerPos = m_camera->GetCameraPosition();
		float listenerAngle = m_camera->GetCameraFacingAngle();
		m_sceneGraph->GetRoot()->UpdateAudioNodesWithTransform(listenerPos, listenerAngle, glm::mat4(1.0f));

		// Rebuild BVH if needed
		if (m_bvhDirty) {
			RebuildSceneBVH();
		}
	}

	// Store frame time for profiling using circular buffer
	m_frameTime = deltaTime * 1000.0f;
	m_frameTimeBuffer[m_frameTimeIndex] = m_frameTime;
	m_frameTimeIndex = (m_frameTimeIndex + 1) % FRAME_TIME_BUFFER_SIZE;
	if (m_frameTimeCount < FRAME_TIME_BUFFER_SIZE) {
		m_frameTimeCount++;
	}

	// Record performance data if recording is active
	if (m_performanceRecorder && m_performanceRecorder->IsRecording()) {
		// Update rendering mode in recorder
		if (m_modularRenderer) {
			const RenderContext& context = m_modularRenderer->GetContext();
			std::string renderMode = (context.rendererMode == RenderContext::RendererMode::PATH_TRACED) ? 
				"Path-Traced" : "Standard";
			m_performanceRecorder->SetRenderingMode(renderMode);
		}
		m_performanceRecorder->RecordFrame(m_frameTime, m_fps);
	}
}

void Core::Render(int windowWidth, int windowHeight) {
	if (!m_sceneGraph) return;

	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;

	// Use ModularRenderer for all rendering
	if (m_modularRenderer) {
		m_modularRenderer->Render(
			m_sceneGraph,
			m_camera,
			m_lighting,
			m_exposure,
			m_gamma,
			m_shadowsEnabled,
			m_shadowBias,
			m_shadowNear,
			m_shadowFar,
			m_environmentColor,
			m_windowWidth,
			m_windowHeight
		);
	}
	else {
		std::cerr << "[Core] ERROR: ModularRenderer is not initialized!" << std::endl;
		return;
	}

	// Start ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	// Set up ImGui interface data only if pointers have changed
	if (m_cachedImGuiLighting != m_lighting) {
		m_imguiInterface->SetLighting(m_lighting);
		m_cachedImGuiLighting = m_lighting;
	}
	if (m_cachedImGuiSceneGraph != m_sceneGraph) {
		m_imguiInterface->SetSceneGraph(m_sceneGraph);
		m_cachedImGuiSceneGraph = m_sceneGraph;
	}
	if (m_cachedImGuiCamera != m_camera) {
		m_imguiInterface->SetCamera(m_camera);
		m_cachedImGuiCamera = m_camera;
	}
	m_imguiInterface->SetFrameData(m_frameTimeBuffer.data(), m_frameTimeCount);

	// Render ImGui windows
	m_imguiInterface->Render(
		m_windowWidth,
		m_windowHeight,
		m_fps,
		m_sceneName,
		m_lightPos,
		m_lightDir
	);

	// Render gizmo overlay
	m_imguiInterface->RenderGizmoOverlay(m_windowWidth, m_windowHeight);

	// Finally, render ImGui
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool Core::ProcessInputEvent(const SDL_Event& event) {
	if (m_inputIntegration) {
		return m_inputIntegration->ProcessEvent(event);
	}
	return false;
}

void Core::HandleMouseClick(int mouseX, int mouseY, int windowWidth, int windowHeight) {
	if (!m_sceneGraph || !m_camera) {
		std::cout << "[Core] Error: Scene graph or camera is null" << std::endl;
		return;
	}

	// Check if ImGuizmo is currently being used
	bool gizmoBeingUsed = false;
	if (m_imguiInterface && m_imguiInterface->IsGizmoVisible() && m_imguiInterface->GetSelectedNode()) {
		gizmoBeingUsed = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
	}

	if (gizmoBeingUsed) {
		std::cout << "[Core] Gizmo is active, skipping mouse picking" << std::endl;
		return;
	}

	std::cout << "[Core] === LIGHT-PRIORITY MOUSE CLICK ===" << std::endl;
	std::cout << "[Core] Mouse clicked at (" << mouseX << ", " << mouseY << ")" << std::endl;

	// Convert screen coordinates to world ray using RayCast system
	RayCast::Ray ray = RayCast::ScreenToWorldRay(mouseX, mouseY, m_camera, windowWidth, windowHeight);

	// Validate ray direction
	if (glm::length(ray.direction) < 0.1f) {
		std::cout << "[Core] Invalid ray direction, length too small" << std::endl;
		return;
	}

	// Perform ray query using light-priority system
	auto hitNode = PerformRayQuery(ray);

	if (hitNode && m_imguiInterface) {
		bool isLightNode = (hitNode->GetNodeType() == SceneNode::LIGHT);

		if (hitNode != m_imguiInterface->GetSelectedNode()) {
			m_imguiInterface->SetSelectedNode(hitNode);

			if (isLightNode) {
				std::cout << "[Core] *** LIGHT NODE SELECTED *** : " << hitNode->GetName() << std::endl;
				if (auto lightNode = std::dynamic_pointer_cast<LightNode>(hitNode)) {
					if (lightNode->GetLight()) {
						auto light = lightNode->GetLight();
						std::cout << "[Core] Light details: "
							<< "Type=" << static_cast<int>(light->GetLightType())
							<< ", Intensity=" << light->GetIntensity()
							<< ", Enabled=" << (light->IsEnabled() ? "Yes" : "No") << std::endl;
					}
				}
			}
			else {
				std::cout << "[Core] Selected geometry node: " << hitNode->GetName() << std::endl;
			}
		}
	}
	else if (m_imguiInterface) {
		if (m_imguiInterface->GetSelectedNode()) {
			std::cout << "[Core] Clicked in empty space, keeping current selection" << std::endl;
		}
		else {
			m_imguiInterface->SetSelectedNode(nullptr);
			std::cout << "[Core] No node selected" << std::endl;
		}
	}
}

void Core::HandleMouseScroll(float scrollY) {
	if (m_camera) {
		m_camera->ProcessMouseScroll(scrollY, m_config["camera"].value("fov", 45.0f));
	}
}

void Core::OnWindowResize(int newWidth, int newHeight) {
	if (newWidth <= 0 || newHeight <= 0) return;
	if (newWidth == m_windowWidth && newHeight == m_windowHeight) return;

	m_windowWidth = newWidth;
	m_windowHeight = newHeight;

	// Update OpenGL viewport
	glViewport(0, 0, m_windowWidth, m_windowHeight);

	// Update camera aspect ratio
	if (m_camera) {
		float aspect = static_cast<float>(m_windowWidth) / static_cast<float>(m_windowHeight);
		m_camera->SetPerspective(
			m_camera->GetCameraFov(),
			aspect,
			m_camera->GetCameraNearPlane(),
			m_camera->GetCameraFarPlane()
		);
	}

	// Resize modular renderer
	if (m_modularRenderer) {
		m_modularRenderer->Resize(m_windowWidth, m_windowHeight);
	}

	std::cout << "[Core] Window resized to " << m_windowWidth << "x" << m_windowHeight << std::endl;
}

void Core::SwapScene(const std::string& newSceneFile) {
	std::cout << "[Core] Swapping to scene: " << newSceneFile << std::endl;

	m_sceneToLoad = newSceneFile;
	CleanupCurrentScene();

	if (m_sceneLoader) {
		auto newGraph = m_sceneLoader->LoadScene(newSceneFile);
		if (newGraph) {
			m_sceneGraph = newGraph;
			m_currentSceneFilePath = newSceneFile;

			m_exposure = m_sceneGraph->m_exposure;
			m_gamma = m_sceneGraph->m_gamma;
			m_sceneName = m_sceneGraph->GetSceneName();

			m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
			if (m_physicsEngine) {
				if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
			}
			
			// Update state manager with new scene file path
			if (m_stateManager) {
				m_stateManager->SetCurrentSceneFilePath(newSceneFile);
			}

			if (m_lightManager) {
				m_lightManager->CollectLightsFromScene(m_sceneGraph);
				m_lightManager->PrintLightInfo();
			}

			if (m_imguiInterface) {
				m_imguiInterface->SetSceneGraph(m_sceneGraph);
			}

			if (m_inputIntegration) {
				m_inputIntegration->SetSceneGraph(m_sceneGraph);
			}

			ComputeSceneBoundingBox();

			std::cout << "[Core] Scene swap completed successfully: " << newSceneFile << std::endl;
		}
		else {
			std::cerr << "[Core] Failed to load scene: " << newSceneFile << std::endl;
		}

		m_bvhDirty = true;
		m_boundingBoxCached = false;

		if (m_modularRenderer) {
			m_modularRenderer->ResetTAA();
		}
	}
}

void Core::SaveCurrentScene() {
	if (!m_sceneGraph || !m_sceneLoader) {
		std::cerr << "[Core] Cannot save: scene graph or loader is null" << std::endl;
		return;
	}

	std::string saveFilePath = m_currentSceneFilePath.empty() ? m_sceneToLoad : m_currentSceneFilePath;
	std::cout << "[Core] Saving scene to: " << saveFilePath << std::endl;

	if (m_sceneLoader->SaveScene(m_sceneGraph, saveFilePath)) {
		std::cout << "[Core] Scene saved successfully!" << std::endl;
	}
	else {
		std::cerr << "[Core] Failed to save scene" << std::endl;
	}
}

bool Core::IsMouseLocked() const {
	if (m_inputIntegration) {
		return m_inputIntegration->IsMouseLocked();
	}
	return false;
}

void Core::SetMouseLocked(bool locked) {
	if (m_inputIntegration) {
		m_inputIntegration->SetMouseLocked(locked);
	}
}

void Core::CleanupCurrentScene() {
	std::cout << "[Core] Cleaning up current scene" << std::endl;

	// Clean up all physics bodies to prevent memory leaks and duplicate registrations
	if (m_physicsEngine) {
		std::cout << "[Core] Removing all physics bodies from engine" << std::endl;
		m_physicsEngine->RemoveAllBodies();
	}

	// Clear spatial acceleration structures
	if (m_sceneBVH) {
		m_sceneBVH->Clear();
	}

	// Clear UI selection to prevent dangling pointers
	if (m_imguiInterface) {
		m_imguiInterface->SetSelectedNode(nullptr);
	}
	
	// Invalidate cached bounds
	m_boundingBoxCached = false;
	m_bvhDirty = true;
	
	std::cout << "[Core] Scene cleanup complete" << std::endl;
}

void Core::ComputeSceneBoundingBox() {
	if (!m_sceneGraph) {
		std::cerr << "[Core] Scene graph is null, cannot compute bounding box.\n";
		return;
	}

	glm::vec3 minBounds(std::numeric_limits<float>::max());
	glm::vec3 maxBounds(-std::numeric_limits<float>::max());

	std::function<void(std::shared_ptr<SceneNode>)> TraverseNodes =
		[&](std::shared_ptr<SceneNode> node) {
		if (!node) return;
		if (node->GetModel()) {
			auto nodeBounds = node->GetBoundingBox();
			minBounds = glm::min(minBounds, nodeBounds.first);
			maxBounds = glm::max(maxBounds, nodeBounds.second);
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

	m_bvhDirty = true;

	std::cout << "[Core] Scene bounding box cached successfully.\n";
}

void Core::RebuildSceneBVH() {
	if (!m_sceneGraph || !m_sceneGraph->GetRoot()) {
		std::cout << "[Core] Cannot build BVH: Scene graph is null" << std::endl;
		return;
	}

	std::cout << "[Core] Building BVH for scene acceleration..." << std::endl;

	std::vector<std::shared_ptr<SceneNode>> nodes;
	std::function<void(std::shared_ptr<SceneNode>)> CollectNodes = [&](std::shared_ptr<SceneNode> node) {
		if (!node) return;
		if (node->GetModel()) {
			nodes.push_back(node);
		}
		for (auto& child : node->children) {
			CollectNodes(child);
		}
		};
	CollectNodes(m_sceneGraph->GetRoot());

	if (nodes.empty()) {
		std::cout << "[Core] No nodes with models found, skipping BVH build" << std::endl;
		m_sceneBVH.reset();
		m_bvhDirty = false;
		return;
	}

	m_sceneBVH = BVH::CreateSceneNodeBVH(nodes);

	if (m_sceneBVH) {
		auto stats = m_sceneBVH->GetStatistics();
		std::cout << "[Core] BVH built successfully:" << std::endl;
		std::cout << "  - Nodes: " << stats.nodeCount << std::endl;
		std::cout << "  - Leaves: " << stats.leafCount << std::endl;
		std::cout << "  - Objects: " << stats.objectCount << std::endl;
		std::cout << "  - Max Depth: " << stats.maxDepth << std::endl;
	}

	m_bvhDirty = false;
}


std::shared_ptr<SceneNode> Core::PerformRayQuery(const RayCast::Ray& ray) {
	if (m_bvhDirty) {
		RebuildSceneBVH();
	}

	// Check for light intersections first (lights get priority)
	if (m_lightManager) {
		m_lightManager->UpdateLightProxies();
		auto lightNode = m_lightManager->FindLightAtRay(ray.origin, ray.direction);
		if (lightNode) {
			std::cout << "[Core] PRIORITY: Selected light node" << std::endl;
			return lightNode;
		}
	}

	// Use enhanced GizmoRayCast system
	if (m_sceneBVH && !m_sceneBVH->Empty()) {
		GizmoRayCast::GizmoConfig config;
		config.enableBVHAcceleration = true;
		config.prioritizeSelectedNode = true;
		config.maxSelectionDistance = 1000.0f;
		config.preferSmallerObjects = true;
		config.smallObjectBonus = 2.5f;
		config.precisionTolerance = 0.05f;
		config.enableExpandedRaySearch = true;
		config.rayExpansionRadius = 0.15f;
		config.enableMultiPassSelection = true;

		auto currentSelection = (m_imguiInterface ? m_imguiInterface->GetSelectedNode() : nullptr);

		auto gizmoResult = GizmoRayCast::QueryForGizmoSelection(ray, m_camera, m_sceneBVH.get(), currentSelection, config);

		if (gizmoResult.hit && gizmoResult.node) {
			std::cout << "[Core] Enhanced selection: " << gizmoResult.node->GetName()
				<< " at distance " << gizmoResult.distanceToCamera << std::endl;
			return gizmoResult.node;
		}

		// Fallback to traditional BVH query
		auto candidates = m_sceneBVH->QueryRay(ray);

		struct CandidateInfo {
			std::shared_ptr<SceneNode> node;
			float distance;
			float size;
			float priority;
		};

		std::vector<CandidateInfo> validCandidates;

		for (const auto& candidateNode : candidates) {
			if (!candidateNode || !candidateNode->GetModel()) continue;

			glm::mat4 worldTransform = candidateNode->GetTransform();
			RayCast::HitResult result = RayCast::RayIntersectNode(ray, candidateNode, worldTransform);

			if (result.hit) {
				CandidateInfo info;
				info.node = candidateNode;
				info.distance = result.distance;

				auto nodeBounds = candidateNode->GetBoundingBox();
				glm::vec3 size = nodeBounds.second - nodeBounds.first;
				info.size = glm::length(size);

				float distancePriority = 1000.0f - result.distance;
				float sizePriority = 100.0f / (info.size + 0.1f);
				info.priority = distancePriority + sizePriority * config.smallObjectBonus;

				validCandidates.push_back(info);
			}
		}

		std::sort(validCandidates.begin(), validCandidates.end(),
			[](const CandidateInfo& a, const CandidateInfo& b) {
				return a.priority > b.priority;
			});

		if (!validCandidates.empty()) {
			return validCandidates[0].node;
		}
	}

	// Fallback: Use legacy ray-scene intersection
	return RayIntersectScene(ray.origin, ray.direction);
}

glm::vec3 Core::ScreenToWorldRay(int mouseX, int mouseY, int windowWidth, int windowHeight) {
	float x = (2.0f * mouseX) / windowWidth - 1.0f;
	float y = 1.0f - (2.0f * mouseY) / windowHeight;

	glm::mat4 proj = m_camera->GetProjectionMatrix();
	glm::mat4 view = m_camera->GetViewMatrix();

	glm::vec4 rayClip(x, y, -1.0f, 1.0f);
	glm::vec4 rayEye = glm::inverse(proj) * rayClip;
	rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);

	glm::vec3 rayWorld = glm::vec3(glm::inverse(view) * rayEye);
	rayWorld = glm::normalize(rayWorld);

	return rayWorld;
}

std::shared_ptr<SceneNode> Core::RayIntersectScene(const glm::vec3& rayOrigin, const glm::vec3& rayDirection) {
	if (!m_sceneGraph || !m_sceneGraph->GetRoot()) {
		return nullptr;
	}

	std::shared_ptr<SceneNode> closestNode = nullptr;
	float closestDistance = std::numeric_limits<float>::max();

	std::function<void(const std::shared_ptr<SceneNode>&, const glm::mat4&)> traverse;
	traverse = [&](const std::shared_ptr<SceneNode>& node, const glm::mat4& parentTransform) {
		if (!node) return;

		glm::mat4 worldTransform = parentTransform * node->GetTransform();

		if (node->GetModel()) {
			auto nodeBounds = node->GetBoundingBox();
			glm::vec3 minBounds = nodeBounds.first;
			glm::vec3 maxBounds = nodeBounds.second;

			glm::vec3 worldMin = glm::vec3(worldTransform * glm::vec4(minBounds, 1.0f));
			glm::vec3 worldMax = glm::vec3(worldTransform * glm::vec4(maxBounds, 1.0f));

			glm::vec3 actualMin = glm::min(worldMin, worldMax);
			glm::vec3 actualMax = glm::max(worldMin, worldMax);

			if (RayIntersectAABB(rayOrigin, rayDirection, actualMin, actualMax) &&
				glm::distance(rayOrigin, (actualMin + actualMax) * 0.5f) < closestDistance) {
				closestDistance = glm::distance(rayOrigin, (actualMin + actualMax) * 0.5f);
				closestNode = node;
			}
		}

		for (const auto& child : node->children) {
			traverse(child, worldTransform);
		}
		};

	traverse(m_sceneGraph->GetRoot(), glm::mat4(1.0f));

	return closestNode;
}

bool Core::RayIntersectNode(const std::shared_ptr<SceneNode>& node, const glm::vec3& rayOrigin,
	const glm::vec3& rayDirection, const glm::mat4& worldTransform) {
	if (!node || !node->GetModel()) return false;

	auto nodeBounds = node->GetBoundingBox();
	glm::vec3 minBounds = nodeBounds.first;
	glm::vec3 maxBounds = nodeBounds.second;

	glm::vec3 worldMin = glm::vec3(worldTransform * glm::vec4(minBounds, 1.0f));
	glm::vec3 worldMax = glm::vec3(worldTransform * glm::vec4(maxBounds, 1.0f));

	return RayIntersectAABB(rayOrigin, rayDirection, glm::min(worldMin, worldMax), glm::max(worldMin, worldMax));
}

bool Core::RayIntersectAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
	const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
	glm::vec3 invDir = 1.0f / rayDirection;

	glm::vec3 t0 = (aabbMin - rayOrigin) * invDir;
	glm::vec3 t1 = (aabbMax - rayOrigin) * invDir;

	glm::vec3 tmin = glm::min(t0, t1);
	glm::vec3 tmax = glm::max(t0, t1);

	float tNear = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
	float tFar = glm::min(glm::min(tmax.x, tmax.y), tmax.z);

	return tNear <= tFar && tFar >= 0.0f;
}
// State Export and Performance Recording


bool Core::SaveSceneState(const std::string& filepath) {
	if (!m_sceneGraph || !m_stateManager || !m_camera) {
		std::cerr << "[Core] Cannot save state: missing components" << std::endl;
		return false;
	}
	return m_stateManager->SaveState(m_sceneGraph, m_camera, filepath);
}

bool Core::LoadSceneState(const std::string& filepath) {
	if (!m_stateManager || !m_camera) {
		std::cerr << "[Core] Cannot load state: missing components" << std::endl;
		return false;
	}

	// Use LoadStateWithSceneValidation to ensure correct base scene is loaded first
	std::cout << "[Core] Loading scene state with automatic scene validation..." << std::endl;

	// Create scene loader callback that Core will use to load the base scene
	auto sceneLoaderCallback = [this](const std::string& sceneFile) -> std::shared_ptr<SceneGraph> {
		std::cout << "[Core] Scene loader callback invoked for: " << sceneFile << std::endl;

		// Use existing scene loader to load the scene
		if (!m_sceneLoader) {
			std::cerr << "[Core] ERROR: SceneLoader not available!" << std::endl;
			return nullptr;
		}

		// Clean up current scene before loading new one
		CleanupCurrentScene();

		// Load the scene
		auto newGraph = m_sceneLoader->LoadScene(sceneFile);
		if (!newGraph || !newGraph->GetRoot()) {
			std::cerr << "[Core] ERROR: Failed to load scene: " << sceneFile << std::endl;
			return nullptr;
		}

		// Initialize scene systems (lights, physics, etc.)
		m_exposure = newGraph->m_exposure;
		m_gamma = newGraph->m_gamma;
		m_sceneName = newGraph->GetSceneName();

		m_physicsEnabledForScene = newGraph->IsPhysicsEnabled();
		if (m_physicsEngine) {
			if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
		}

		if (m_lightManager) {
			m_lightManager->CollectLightsFromScene(newGraph);
			m_lightManager->PrintLightInfo();
		}

		if (m_inputIntegration) {
			m_inputIntegration->SetSceneGraph(newGraph);
		}

		ComputeSceneBoundingBox();
		m_bvhDirty = true;
		m_boundingBoxCached = false;

		if (m_modularRenderer) {
			m_modularRenderer->ResetTAA();
		}

		std::cout << "[Core] Base scene loaded and initialized: " << sceneFile << std::endl;
		return newGraph;
		};

	// Load state with automatic scene loading
	auto loadedSceneGraph = m_stateManager->LoadStateWithSceneValidation(
		m_camera,
		filepath,
		sceneLoaderCallback
	);

	if (!loadedSceneGraph) {
		std::cerr << "[Core] Failed to load scene state: " << filepath << std::endl;
		return false;
	}

	// Update Core's scene graph reference
	m_sceneGraph = loadedSceneGraph;
	m_currentSceneFilePath = m_stateManager->GetCurrentSceneFilePath();

	// Update UI references
	if (m_imguiInterface) {
		m_imguiInterface->SetSceneGraph(m_sceneGraph);
	}

	std::cout << "[Core] Scene state loaded successfully with proper base scene" << std::endl;
	return true;
}

bool Core::QuickSave() {
	if (!m_sceneGraph || !m_stateManager || !m_camera) {
		std::cerr << "[Core] Cannot quick save: missing components" << std::endl;
		return false;
	}
	return m_stateManager->QuickSave(m_sceneGraph, m_camera);
}

bool Core::ExportPerformanceCSV(const std::string& filepath) {
	if (!m_performanceRecorder) {
		std::cerr << "[Core] Performance recorder not initialized" << std::endl;
		return false;
	}
	if (m_performanceRecorder->GetFrameCount() == 0) {
		std::cerr << "[Core] No performance data to export" << std::endl;
		return false;
	}
	return m_performanceRecorder->ExportToCSV(filepath);
}

bool Core::ExportPerformanceJSON(const std::string& filepath) {
	if (!m_performanceRecorder) {
		std::cerr << "[Core] Performance recorder not initialized" << std::endl;
		return false;
	}
	if (m_performanceRecorder->GetFrameCount() == 0) {
		std::cerr << "[Core] No performance data to export" << std::endl;
		return false;
	}
	return m_performanceRecorder->ExportToJSON(filepath);
}

void Core::StartPerformanceRecording() {
	if (!m_performanceRecorder) {
		std::cerr << "[Core] Performance recorder not initialized" << std::endl;
		return;
	}

	m_performanceRecorder->StartRecording();

	if (m_imguiInterface) {
		m_imguiInterface->SetRecordingState(true);
	}

	std::cout << "[Core] Started performance recording" << std::endl;
}

void Core::StopPerformanceRecording() {
	if (!m_performanceRecorder) {
		std::cerr << "[Core] Performance recorder not initialized" << std::endl;
		return;
	}

	m_performanceRecorder->StopRecording();

	if (m_imguiInterface) {
		m_imguiInterface->SetRecordingState(false);
	}

	std::cout << "[Core] Stopped performance recording. Captured "
		<< m_performanceRecorder->GetFrameCount() << " frames" << std::endl;
}
