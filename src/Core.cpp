#include "Core.h"
#include "automation/AutomationServer.h"
#include "BenchmarkRunner.h"
#include "CameraPath.h"
#include "SceneRenderSettings.h"
#include "render/ActiveCamera.h"
#include "render/ParticleSystem.h"
#include "GameModule.h"
#include "FrameProfiler.h"
#include "PresentTarget.h"
#include "ImGui/EditorWorkspace.h"
#include "ImGui/ConsoleLog.h"
#include "editor/solace/SolaceUI.h"
#include "RenderSystem.h"
#include "TransformSystem.h"
#include "ComponentManager.h"
#include "assets/AssetIO.h"
#include "app/UserSettings.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <IMGUI/imgui.h>
#include <IMGUI/imgui_impl_sdl2.h>
#include <IMGUI/ImGuizmo.h>
#include "rhi/RHI.h"

#include "Camera.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "SceneLoader.h"
#include "ModelManager.h"
#include "DirectionalLight.h"
#include "LightManager.h"
#include "LightNode.h"
#include "scene/Prefab.h"
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
#include "ui/Pointer.h"
#include "PerformanceRecorder.h"
#include "stb_image_write.h"

namespace {
namespace fs = std::filesystem;

std::string GetEnvVarString(const char* name)
{
#if defined(_MSC_VER)
	char* value = nullptr;
	size_t length = 0;
	if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
		return {};
	}
	std::string result(value);
	std::free(value);
	return result;
#else
	const char* value = std::getenv(name);
	return value ? std::string(value) : std::string{};
#endif
}

int GetEnvVarInt(const char* name, int fallback)
{
	const std::string value = GetEnvVarString(name);
	if (value.empty()) {
		return fallback;
	}
	char* end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	return end != value.c_str() ? static_cast<int>(parsed) : fallback;
}

bool EnvFrameListContains(const std::string& frames, int frameIndex)
{
	const char* cursor = frames.c_str();
	while (*cursor != '\0') {
		while (*cursor == ',' || *cursor == ';' || *cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}

		char* end = nullptr;
		const long parsed = std::strtol(cursor, &end, 10);
		if (end == cursor) {
			++cursor;
			continue;
		}
		if (parsed == frameIndex) {
			return true;
		}
		cursor = end;
	}
	return false;
}

bool SaveBackbufferPNG(int width, int height, const fs::path& path)
{
	if (width <= 0 || height <= 0) {
		return false;
	}

	std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);
	rhi::Gpu().BindReadFramebuffer(PresentTarget::Get());
	rhi::Gpu().ReadPixels(PresentTarget::Get(), 0, { 0, 0, width, height }, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, pixels.data(), 1);

	std::vector<unsigned char> flipped(pixels.size(), 0u);
	for (int y = 0; y < height; ++y) {
		const std::size_t src = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
		const std::size_t dst = static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width) * 4u;
		std::copy_n(pixels.data() + src, static_cast<std::size_t>(width) * 4u, flipped.data() + dst);
	}

	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	return stbi_write_png(path.string().c_str(), width, height, 4, flipped.data(), width * 4) != 0;
}

namespace {
// Every ECS entity of a node subtree: the selection outline covers a model and all of its descendants.
void AppendSubtreeEntities(const SceneNode& node, std::vector<uint32_t>& out)
{
	if (node.HasECSEntity()) out.push_back(node.GetEntityID());
	for (const auto& child : node.children) {
		if (child) AppendSubtreeEntities(*child, out);
	}
}
}

void MaybeDumpSceneFramebuffer(int width, int height)
{
	static int frameIndex = 0;
	static int lastDumpedFrame = -1;
	++frameIndex;

	const std::string dumpDir = GetEnvVarString("NOX_FRAMEBUFFER_DUMP_DIR");
	if (dumpDir.empty()) {
		return;
	}

	const std::string explicitFrames = GetEnvVarString("NOX_FRAMEBUFFER_DUMP_FRAMES");
	const int dumpFrame = std::max(GetEnvVarInt("NOX_FRAMEBUFFER_DUMP_FRAME", 120), 1);
	const bool shouldDump = !explicitFrames.empty()
		? EnvFrameListContains(explicitFrames, frameIndex)
		: frameIndex == dumpFrame;
	if (!shouldDump || lastDumpedFrame == frameIndex) {
		return;
	}

	const std::string suffix = !explicitFrames.empty()
		? ("_frame" + std::to_string(frameIndex))
		: std::string{};
	const fs::path path = fs::path(dumpDir) / ("scene_framebuffer" + suffix + ".png");
	const bool ok = SaveBackbufferPNG(width, height, path);
	std::cout << "[FramebufferDump] frame=" << frameIndex
		<< " path=" << path.string()
		<< " ok=" << ok
		<< '\n';
	lastDumpedFrame = frameIndex;
}

std::string TrimScenePathValue(const std::string& value)
{
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	std::string trimmed = value.substr(begin, end - begin);
	if (trimmed.size() >= 2) {
		const char first = trimmed.front();
		const char last = trimmed.back();
		if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
			trimmed = trimmed.substr(1, trimmed.size() - 2);
		}
	}
	return trimmed;
}

bool SceneFileExists(const fs::path& path)
{
	const auto stat = AssetIO::Stat(path.generic_string());
	return stat && !stat->isDirectory;
}

void PushUniqueSceneCandidate(std::vector<fs::path>& candidates, const fs::path& candidate)
{
	const fs::path normalized = candidate.lexically_normal();
	for (const fs::path& existing : candidates) {
		if (existing.lexically_normal() == normalized) {
			return;
		}
	}
	candidates.push_back(normalized);
}

std::string ResolveSceneFilePath(const std::string& requestedPath)
{
	const std::string trimmed = TrimScenePathValue(requestedPath);
	if (trimmed.empty()) {
		return {};
	}

	std::vector<fs::path> candidates;
	const fs::path authoredPath(trimmed);
	PushUniqueSceneCandidate(candidates, authoredPath);
	if (!authoredPath.has_extension()) {
		PushUniqueSceneCandidate(candidates, fs::path(trimmed + ".json"));
	}
	if (!authoredPath.is_absolute()) {
		PushUniqueSceneCandidate(candidates, fs::path("scenes") / authoredPath);
		if (!authoredPath.has_extension()) {
			PushUniqueSceneCandidate(candidates, fs::path("scenes") / fs::path(trimmed + ".json"));
		}
	}

	for (const fs::path& candidate : candidates) {
		if (SceneFileExists(candidate)) {
			const auto logical = AssetIO::Normalize(candidate.generic_string());
			return logical ? *logical : candidate.lexically_normal().generic_string();
		}
	}

	std::cerr << "[Core] Warning: scene path could not be resolved before load: "
		<< trimmed << std::endl;
	return trimmed;
}

bool ReadSceneCameraVec3(const json& object, const char* key, glm::vec3& outValue)
{
	if (!object.contains(key) || !object[key].is_array() || object[key].size() != 3) {
		return false;
	}

	try {
		outValue = glm::vec3(
			object[key][0].get<float>(),
			object[key][1].get<float>(),
			object[key][2].get<float>());
	}
	catch (const std::exception&) {
		return false;
	}

	return std::isfinite(outValue.x) && std::isfinite(outValue.y) && std::isfinite(outValue.z);
}
}
#include "RuntimeStateManager.h"
#include "reflect/Reflection.h"
#include "TimeScale.h"
#include "audio/AudioListener.h"
#include "diagnostics/DebugDraw.h"
#include "physics/PhysicsDebugView.h"
#include "reflect/ReflectServices.h"

Core::Core()
	: m_window(nullptr)
	, m_windowWidth(1920)
	, m_windowHeight(1080)
	, m_modelPath("model.gltf")
	, m_fontPath("arial.ttf")
	, m_vsync(true)
	, m_shaderProgram()
	, m_shadowShader()
	, m_environmentColor(0.3f, 0.3f, 0.3f)
	, m_lightPos(1.2f, 1.0f, 2.0f)
	, m_lightDir(0.0f, -1.0f, 0.0f)
	, m_shadowsEnabled(true)
	, m_shadowBias(0.005f)
	, m_shadowNear(0.1f)
	, m_shadowFar(1000.0f)
	, m_shadowSize(2048)
	, m_splitLambda(0.85f)
	, m_exposure(1.0f)
	, m_gamma(2.2f)
	, m_sceneHasAudioNodes(false)
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
		// Defaults ship as assets; per-user changes live in the writable user directory.
		m_config = UserSettings::LoadJsonWithUserOverride("config/config.json", "settings/config.json");
		if (!m_config.is_object()) {
			std::cerr << "[Core] Could not read config/config.json" << std::endl;
			return false;
		}

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
			m_shadowSize = lightConfig.value("shadow_size", 2048);
			m_splitLambda = glm::clamp(lightConfig.value("split_lambda", 0.85f), 0.0f, 1.0f);
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

bool Core::Initialize(SDL_Window* window, int windowWidth, int windowHeight) {
	m_window = window;
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

	if (m_launch.mode == AppMode::Editor) {
		// The editor starts with a free cursor; the fly camera locks it while the right button is held.
		SetMouseLocked(false);
	}
	else {
		// Game: the pointer layer owns cursor visibility/capture (the game module requests a mode per
		// frame: hidden while driving, visible in menus). ImGui must not reset the OS cursor each frame.
		SetMouseLocked(false);
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		ui::Pointer::Get().SetCursorManaged(true);
		ui::Pointer::Get().SetCursorMode(ui::CursorMode::Hidden);
	}

	std::cout << "[Core] Engine initialization complete!" << std::endl;
	return true;
}

bool Core::InitializeGraphics() {
	std::cout << "[Core] Initializing graphics..." << std::endl;

	// Initialize ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	{
		ImGuiIO& io = ImGui::GetIO();
		if (m_launch.mode == AppMode::Editor) {
			// Docked editor (docs/nfs2015/game.md); layout persists next to the other editor settings.
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			// The layout is user state: ImGui writes it to the user directory; the authored default
			// (config/editor_layout.ini) seeds it until the user has a layout of their own.
			static std::string iniPath;
			if (const auto userIni = UserSettings::UserPath("editor_layout.ini")) {
				iniPath = userIni->string();
				std::error_code ec;
				fs::create_directories(userIni->parent_path(), ec);
				if (!fs::exists(*userIni, ec)) {
					if (const auto defaults = AssetIO::ReadText("config/editor_layout.ini")) {
						ImGui::LoadIniSettingsFromMemory(defaults->data(), defaults->size());
					}
				}
			}
			io.IniFilename = iniPath.empty() ? nullptr : iniPath.c_str();
		}
		else {
			io.IniFilename = nullptr;
		}
		io.ConfigWindowsMoveFromTitleBarOnly = true;
	}
	rhi::Gpu().ImGuiInit();
	SetStyle(); // Solace theme (ImguiStyle.h)
	solace::LoadFonts(15.0f, 1.0f);
	// Optional game layer: the module for the startup scene's project is created before the first
	// frame (its fonts/atlases/audio need the device and ImGui); scene swaps may switch projects.
	m_gameResourcesReady = true;
	SelectGameModule(ReadStartupSceneRoot());

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

bool Core::ApplySceneCameraOverride(const std::string& sceneFilePath) {
	if (!m_camera) {
		return false;
	}

	const auto sceneText = AssetIO::ReadText(sceneFilePath);
	if (!sceneText) {
		std::cerr << "[Core] Scene camera override skipped; failed to open scene file: "
			<< sceneFilePath << std::endl;
		return false;
	}
	std::istringstream sceneFile(*sceneText);

	json sceneJson;
	try {
		sceneFile >> sceneJson;
	}
	catch (const std::exception& e) {
		std::cerr << "[Core] Scene camera override skipped; failed to parse scene file '"
			<< sceneFilePath << "': " << e.what() << std::endl;
		return false;
	}

	if (!sceneJson.is_object() || !sceneJson.contains("camera") || !sceneJson["camera"].is_object()) {
		return false;
	}

	const json& cameraJson = sceneJson["camera"];
	glm::vec3 position(0.0f);
	if (!ReadSceneCameraVec3(cameraJson, "position", position)) {
		std::cerr << "[Core] Scene camera override skipped; camera.position is missing or invalid in "
			<< sceneFilePath << std::endl;
		return false;
	}

	glm::vec3 up(0.0f, 1.0f, 0.0f);
	(void)ReadSceneCameraVec3(cameraJson, "up", up);

	float yaw = cameraJson.value("yaw", m_camera->GetCameraFacingAngle());
	float pitch = cameraJson.value("pitch", 0.0f);
	glm::vec3 lookAt(0.0f);
	if ((!cameraJson.contains("yaw") || !cameraJson.contains("pitch")) &&
		ReadSceneCameraVec3(cameraJson, "look_at", lookAt)) {
		const glm::vec3 direction = glm::normalize(lookAt - position);
		if (std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z)) {
			yaw = glm::degrees(std::atan2(direction.z, direction.x));
			pitch = glm::degrees(std::asin(glm::clamp(direction.y, -1.0f, 1.0f)));
		}
	}

	const float fov = glm::clamp(cameraJson.value("fov", m_camera->GetCameraFov()), 1.0f, 120.0f);
	const float nearPlane = std::max(cameraJson.value("near", m_camera->GetCameraNearPlane()), 0.001f);
	const float farPlane = std::max(cameraJson.value("far", m_camera->GetCameraFarPlane()), nearPlane + 0.001f);
	const float aspect = (m_windowHeight > 0) ?
		static_cast<float>(m_windowWidth) / static_cast<float>(m_windowHeight) :
		16.0f / 9.0f;

	m_camera->SetPose(position, up, yaw, pitch);
	m_camera->SetPerspective(fov, aspect, nearPlane, farPlane);
	m_camera->SetProjectionType(Camera::ProjectionType::Perspective);
	m_camera->m_movementSpeed = std::max(cameraJson.value("movement_speed", m_camera->GetCameraMovementSpeed()), 0.0f);
	m_camera->m_mouseSensitivity = std::max(cameraJson.value("mouse_sensitivity", m_camera->GetCameraMouseSensitivity()), 0.0f);

	std::cout << "[Core] Applied scene camera override from " << sceneFilePath
		<< " position=(" << position.x << ", " << position.y << ", " << position.z
		<< ") yaw=" << yaw << " pitch=" << pitch << " fov=" << fov
		<< " near=" << nearPlane << " far=" << farPlane << std::endl;
	return true;
}

bool Core::InitializePhysics() {
	std::cout << "[Core] Initializing physics..." << std::endl;

	SimulationConfig config;
	if (!config.loadConfigJson(m_config)) {
		std::cerr << "[Core] Using default simulation config.\n";
	}
	m_physicsEngine = std::make_shared<PhysicsEngine>(config);

	return true;
}

bool Core::InitializeScene() {
	std::cout << "[Core] Initializing scene..." << std::endl;

	m_sceneToLoad = ResolveSceneFilePath(m_config.value("first_scene", "scenes/scene.json"));
	if (!m_launch.scene.empty()) {
		m_sceneToLoad = ResolveSceneFilePath(m_launch.scene);
	}
	if (!m_launch.quality.empty() && GetEnvVarString("NOX_QUALITY_TIER").empty()) {
		_putenv_s("NOX_QUALITY_TIER", m_launch.quality.c_str());
	}
	const std::string forcedScene = GetEnvVarString("NOX_FIRST_SCENE");
	if (!forcedScene.empty()) {
		m_sceneToLoad = ResolveSceneFilePath(forcedScene);
		std::cout << "[Core] NOX_FIRST_SCENE override resolved to: " << m_sceneToLoad << std::endl;
	}
	m_modelManager = std::make_shared<ModelManager>();

	// Descriptors and services first: the first scene's "components" blocks are applied while it loads.
	nox::reflect::RegisterBuiltinComponents();
	{
		nox::reflect::EngineServices& services = nox::reflect::Services();
		services.loadModel = [this](const std::string& path) { return m_modelManager ? m_modelManager->LoadModel(path) : nullptr; };
		services.modelPath = [this](const Scene& model) { return m_modelManager ? m_modelManager->GetModelPath(&model) : std::string(); };
		services.physics = m_physicsEngine.get();
		services.sceneGraph = [this] { return m_sceneGraph.get(); };
		services.sceneChanged = [this] { MarkDirty(); InvalidateSceneCaches(); };
		services.isPlaying = [this] { return IsPlaying(); };
		services.requestSceneSwap = [this](const std::string& scenePath) { RequestSceneSwap(scenePath); };
		services.renderContext = [this] { return m_modularRenderer ? &m_modularRenderer->GetContext() : nullptr; };
		services.instantiatePrefab = [this](const std::string& path, const std::shared_ptr<SceneNode>& parent,
			const json& placement, std::string& error) {
			return InstantiatePrefabNode(path, parent, placement, &error);
		};
		services.destroyNode = [this](const std::shared_ptr<SceneNode>& node) { return DestroySceneNodeForPlay(node); };
	}
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
	m_sceneHasAudioNodes = !m_sceneGraph->FindNodesByType(SceneNode::AUDIO).empty();
	ApplySceneCameraOverride(m_sceneToLoad);

	// Sync physics enabled state from scene JSON
	m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
	if (m_physicsEngine) {
		if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
	}

	auto lightManager = m_sceneGraph->GetLightManager();
	if (lightManager) {
		lightManager->GetShadowConfig().directionalSplitLambda = m_splitLambda;
		lightManager->InitializeShadowSystem(8, m_shadowSize);
		lightManager->CollectLightsFromScene(m_sceneGraph);
		if (lightManager->GetEnabledLightCount() == 0 && m_lighting) {
			lightManager->RegisterLight(m_lighting, "MainDirectionalLight");
		}
		lightManager->PrintLightInfo();
	}

	ComputeSceneBoundingBox();

	if (m_modularRenderer) {
		m_sceneHasRenderSettings = SceneRenderSettings::ApplyFromSceneFile(
			m_modularRenderer->GetContext(), m_sceneToLoad, m_sceneHasRenderSettings);
	}
	m_benchmark = BenchmarkRunner::CreateFromEnvironment(m_sceneToLoad);
	if (m_game) {
		GameModuleServices services;
		services.physics = m_physicsEngine.get();
		m_game->SetServices(services);
		m_game->SetCameraTemplate(*m_camera);
	}
	SaveRoundtripCopyIfRequested();
	ConfigureGameFromScene(m_sceneToLoad);
	if (!m_benchmark && m_game && m_game->IsAvailable() && (m_launch.mode == AppMode::Game || m_game->WantsAutoPlay())) {
		RequestPlay();
	}

	return true;
}

// NOX_SAVE_ROUNDTRIP=<out.json>: writes the initial scene to out.json, and the engine quits after that (editor tooling for
// save/load round trips, docs/building.md). The copy is taken right after the scene loads and before the game module is
// configured: a game module poses gameplay nodes (a player car at its spawn point) while it configures, so only this point
// holds the authored state. The source scene is not modified.
void Core::SaveRoundtripCopyIfRequested()
{
	static const std::string roundtripOut = GetEnvVarString("NOX_SAVE_ROUNDTRIP");
	if (roundtripOut.empty() || m_roundtripDone) return;
	m_roundtripDone = true;
	const bool ok = m_sceneGraph && m_sceneLoader &&
		m_sceneLoader->SaveSceneCopy(m_sceneGraph, m_currentSceneFilePath, roundtripOut);
	std::cout << "[Core] NOX_SAVE_ROUNDTRIP " << (ok ? "wrote " : "FAILED ") << roundtripOut << std::endl;
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

	const std::string forcedState = GetEnvVarString("NOX_FIRST_SCENE_STATE");
	if (!forcedState.empty()) {
		std::cout << "[Core] NOX_FIRST_SCENE_STATE override loading: " << forcedState << std::endl;
		if (!LoadSceneState(forcedState)) {
			std::cerr << "[Core] Failed to load NOX_FIRST_SCENE_STATE: " << forcedState << std::endl;
		}
	}

	// Connect ModularRenderer to UI for real-time rendering control
	if (m_modularRenderer) {
		m_imguiInterface->SetModularRenderer(m_modularRenderer);
	}

	if (m_launch.mode == AppMode::Editor && !m_benchmark) {
		m_editor = std::make_unique<EditorWorkspace>(*this, *m_imguiInterface);
	}

	return true;
}

LightManager* Core::GetLightManager() {
	auto lightManager = (m_sceneGraph ? m_sceneGraph->GetLightManager() : nullptr);
	return lightManager ? lightManager.get() : nullptr;
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
	m_inputIntegration->SetLightManager(GetLightManager());

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

	// The game module may hold engine services (physics substep callback): end play and destroy it
	// before the services it was given.
	if (m_game) {
		if (m_playing) m_game->EndPlay();
		m_playing = false;
		m_game.reset();
	}
	if (m_physicsEngine) {
		m_physicsEngine.reset();
	}

	nox::reflect::Services() = {};
	m_sceneGraph.reset();
	m_sceneLoader.reset();
	m_modelManager.reset();
	m_lighting.reset();
	m_camera.reset();
	m_sceneBVH.reset();
	m_performanceRecorder.reset();
	m_stateManager.reset();

	std::cout << "[Core] Shutdown complete" << std::endl;
}

// First current AudioListener in scene order (depth-first). `path` receives the nodes from the root down to it, so
// its world matrix is computed only for that node (the search itself is a flag check per node).
static bool FindCurrentAudioListener(const SceneNode& node, std::vector<const SceneNode*>& path) {
	path.push_back(&node);
	if (node.IsCurrentAudioListener()) return true;
	for (const auto& child : node.children) {
		if (child && FindCurrentAudioListener(*child, path)) return true;
	}
	path.pop_back();
	return false;
}

float Core::GetTimeScale() const {
	return TimeScale::Get();
}

bool Core::SetTimeScale(float scale) {
	return TimeScale::Set(scale);
}

void Core::DrawDebugOverlay(ImDrawList* drawList, float x, float y, float width, float height) {
	if (!drawList || !m_camera) return;
	// Physics debug toggles from the editor Physics window: queued here, for the Scene viewport only, so they are
	// cleared with the other per-frame debug primitives at the next Update.
	if (m_physicsEngine) QueuePhysicsDebugDraw(*m_physicsEngine, m_physicsEngine->DebugView());
	DrawDebugPrimitives(drawList, m_camera, x, y, width, height);
}

void Core::DrawDebugPrimitives(ImDrawList* drawList, const std::shared_ptr<Camera>& camera, float x, float y, float width, float height) {
	if (!drawList || !camera || DebugDraw::Count() == 0) return;
	const glm::mat4 view = camera->GetViewMatrix();
	const float fov = camera->GetCameraFov();
	const ImVec2 origin(x, y);
	const ImVec2 size(width, height);
	DebugDraw::Draw(drawList, [&](const glm::vec3& world, ImVec2& out) {
		return DebugDraw::ProjectView(view, fov, origin, size, world, out);
	});
}

void Core::Update(float deltaTime) {
	// Automation server (src/automation): queued commands run here; a paused engine skips this update.
	if (!automation::BeginFrame(*this, deltaTime)) return;
	m_frameCpuStart = std::chrono::steady_clock::now();
	PerformanceRecorder::ECSMetrics ecsMetrics{};
	// Debug primitives live for real seconds: age them before this frame's scripts queue new ones.
	DebugDraw::Age(deltaTime);

	// NOX_SAVE_ROUNDTRIP: the copy was written during scene initialisation (SaveRoundtripCopyIfRequested); quit
	// before any game or physics step runs.
	if (m_roundtripDone) {
		m_quitRequested = true;
		return;
	}

	// Benchmark runs use a fixed simulation step so camera paths and captures are repeatable.
	if (m_benchmark) {
		deltaTime = m_benchmark->FixedDeltaTime();
	}
	// NOX_FIXED_DT=<seconds>: fixed simulation step so per-frame dumps form a real-time video.
	static const float fixedDt = [] { const std::string v = GetEnvVarString("NOX_FIXED_DT"); return v.empty() ? 0.0f : static_cast<float>(std::atof(v.c_str())); }();
	if (fixedDt > 0.0f) {
		deltaTime = fixedDt;
	}

	// Simulation time: game, animation and physics advance by the time scale. Input and the frame timers keep deltaTime.
	const float simDt = TimeScale::Apply(deltaTime);

	// Update input system
	if (m_inputIntegration) {
		m_inputIntegration->Update(deltaTime);
	}
	FrameProfiler::Mark("input");

	// NOX_SWAP_SCENE=<frame>:<scene.json>: request a scene switch at that frame, like the editor does
	// (test hook for scene-change regressions such as IBL/skybox regeneration).
	{
		static const std::string swapSpec = GetEnvVarString("NOX_SWAP_SCENE");
		static int swapFrame = -1;
		static int frame = 0;
		if (swapFrame < 0 && !swapSpec.empty()) {
			const size_t colon = swapSpec.find(':');
			swapFrame = colon == std::string::npos ? 0 : std::atoi(swapSpec.substr(0, colon).c_str());
		}
		if (!swapSpec.empty() && ++frame == swapFrame) {
			RequestSceneSwap(swapSpec.substr(swapSpec.find(':') + 1));
		}
	}

	// Deferred scene switch requested by the editor.
	if (!m_pendingSceneSwap.empty()) {
		const std::string next = m_pendingSceneSwap;
		m_pendingSceneSwap.clear();
		SwapScene(next);
	}

	// Game layer: car, race and chase camera (posed at spawn while editing).
	if (!m_benchmark && m_game && m_game->IsAvailable()) {
		bool inputEnabled = true;
		float aspect = m_windowHeight > 0 ? static_cast<float>(m_windowWidth) / static_cast<float>(m_windowHeight) : 16.0f / 9.0f;
		if (m_editor) {
			inputEnabled = m_editor->GameInputFocus();
			const EditorWorkspace::Viewport& vp = m_editor->GetViewport(EditorWorkspace::Target::Game);
			if (vp.size.x > 1.0f && vp.size.y > 1.0f) aspect = vp.size.x / vp.size.y;
		}
		if (m_game->Update(simDt, inputEnabled, aspect) == IGameModule::UpdateResult::ExitApplication) {
			m_quitRequested = true;
		}
	}

	FrameProfiler::Mark("game");
	// Particle emitters advance once per frame here (not per render call: the editor renders several views).
	if (m_sceneGraph && m_sceneGraph->GetRoot()) {
		nox::render::UpdateSceneEmitters(*m_sceneGraph->GetRoot(), m_frameTime * 0.001f);
	}
	// Scripted shots pose the camera and animated nodes after input so they always win.
	bool cameraCut = false;
	if (m_benchmark && m_camera && m_sceneGraph) {
		m_benchmark->Update(*m_camera, *m_sceneGraph);
		if (m_benchmark->ConsumeCameraCut()) {
			cameraCut = true;
			if (m_modularRenderer) {
				m_modularRenderer->ResetTAA();
			}
		}
	}

	// NOX_CAMERA_PATH: deterministic camera motion for repeatable temporal tests (CameraPath.h). It poses
	// the camera after game and benchmark posing, so it overrides them while the path is active.
	static CameraPath cameraPath(GetEnvVarString("NOX_CAMERA_PATH"));
	if (cameraPath.IsEnabled() && m_camera) {
		cameraPath.Apply(*m_camera);
	}

	// Handle deferred physics toggle
	if (m_requestTogglePhysics && m_physicsEngine) {
		m_physicsEnabledForScene = !m_physicsEnabledForScene;
		if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
		m_requestTogglePhysics = false;
	}

	// Update FPS counter
	m_frameCount += 1.0f;
	m_fpsUpdateTime += deltaTime;
	if (m_fpsUpdateTime >= FPS_UPDATE_INTERVAL) {
		m_fps = m_frameCount / m_fpsUpdateTime;
		m_frameCount = 0.0f;
		m_fpsUpdateTime = 0.0f;
	}

	// Frame stage contract (attach each runtime feature to one stage only):
	// 1) ECS animation/transform stage: animation + transform systems
	// 2) Physics stage: physics integration + ECS transform refresh
	// 3) Audio stage: listener/source spatial update
	// NOTE: Legacy recursive SceneNode update calls are intentionally excluded
	// from the frame loop when equivalent ECS systems are active.
	if (m_sceneGraph && m_sceneGraph->IsActive()) {
		if (TransformSystem* transformSystem = m_sceneGraph->GetTransformSystem()) {
			transformSystem->BeginFrameDiagnostics();
		}

		FrameProfiler::Mark("benchmark_misc");
		// Stage 1: ECS animation/transform
		auto animationStart = std::chrono::high_resolution_clock::now();
		m_sceneGraph->UpdateAnimations(simDt);
		auto animationEnd = std::chrono::high_resolution_clock::now();
		ecsMetrics.animationSystemMs += std::chrono::duration<float, std::milli>(animationEnd - animationStart).count();

		auto transformSyncStart = std::chrono::high_resolution_clock::now();
		m_sceneGraph->UpdateAllTransforms();
		auto transformSyncEnd = std::chrono::high_resolution_clock::now();
		ecsMetrics.transformSystemMs += std::chrono::duration<float, std::milli>(transformSyncEnd - transformSyncStart).count();
		FrameProfiler::Mark("transforms");
		// A camera cut teleports scripted objects: their motion this frame is not real motion.
		if (m_game && m_game->ConsumeTeleport()) {
			cameraCut = true;
			if (m_modularRenderer) {
				m_modularRenderer->ResetTAA();
			}
		}
		if (cameraCut) {
			if (TransformSystem* transformSystem = m_sceneGraph->GetTransformSystem()) {
				transformSystem->SnapshotPreviousTransforms();
			}
		}

		// Stage 2: physics
		if (m_physicsEngine && m_physicsEnabledForScene) {
			// Physics synchronization is centralized inside PhysicsEngine::Update()
			// (PreStepSync before stepping, PostStepSync after stepping).
			auto physicsStart = std::chrono::high_resolution_clock::now();
			m_physicsEngine->Update(simDt);
			auto physicsEnd = std::chrono::high_resolution_clock::now();
			ecsMetrics.physicsStepMs += std::chrono::duration<float, std::milli>(physicsEnd - physicsStart).count();

			auto postPhysicsTransformSyncStart = std::chrono::high_resolution_clock::now();
			m_sceneGraph->UpdateAllTransforms();
			auto postPhysicsTransformSyncEnd = std::chrono::high_resolution_clock::now();
			ecsMetrics.transformSystemMs += std::chrono::duration<float, std::milli>(postPhysicsTransformSyncEnd - postPhysicsTransformSyncStart).count();
		}

		FrameProfiler::Mark("physics");
		// Stage 3: audio
		// The 3D listener is the current AudioListener node, or the camera when there is none.
		{
			AudioListenerPose listener;
			listener.position = m_camera->GetCameraPosition();
			listener.angleDegrees = m_camera->GetCameraFacingAngle();
			std::vector<const SceneNode*> listenerPath;
			const std::shared_ptr<SceneNode> root = m_sceneGraph->GetRoot();
			if (root && FindCurrentAudioListener(*root, listenerPath)) {
				glm::mat4 listenerWorld(1.0f);
				for (const SceneNode* n : listenerPath) listenerWorld = n->GetGlobalTransform(listenerWorld);
				listener = AudioListenerPoseFromWorld(listenerWorld);
			}
			AudioListenerPublish(listener);
			if (m_sceneHasAudioNodes) {
				m_sceneGraph->GetRoot()->UpdateAudioNodesWithTransform(listener.position, listener.angleDegrees, glm::mat4(1.0f));
			}
		}

		// Rebuild BVH if needed
		if (m_bvhDirty) {
			RebuildSceneBVH();
		}

		if (const TransformSystem* transformSystem = m_sceneGraph->GetTransformSystem()) {
			const TransformSystem::Diagnostics& diagnostics = transformSystem->GetDiagnostics();
			ecsMetrics.transformUpdateMs = diagnostics.updateTransformsMs;
			ecsMetrics.runtimeDirtyEvalMs = diagnostics.runtimeDirtyEvalMs;
			ecsMetrics.transformUpdateCalls = diagnostics.updateCallCount;
			ecsMetrics.pendingDirtyRoots = diagnostics.pendingDirtyRoots;
			ecsMetrics.dirtyRootsProcessed = diagnostics.dirtyRootsProcessed;
			ecsMetrics.transformsRecomputed = diagnostics.transformsRecomputed;
			ecsMetrics.ancestorQueryCalls = diagnostics.findTopDirtyAncestorCalls;
			ecsMetrics.ancestorQuerySteps = diagnostics.findTopDirtyAncestorSteps;
			ecsMetrics.depthQueryCalls = diagnostics.hierarchyDepthQueryCalls;
			ecsMetrics.depthQuerySteps = diagnostics.hierarchyDepthQuerySteps;
			ecsMetrics.runtimeDirtySpanCount = diagnostics.runtimeDirtySpanCount;
			ecsMetrics.runtimeDirtySpanCoverageNodes = diagnostics.runtimeDirtySpanCoverageNodes;
		}

		if (const RenderSystem* renderSystem = m_sceneGraph->GetRenderSystem()) {
			const RenderSystem::Diagnostics& diagnostics = renderSystem->GetDiagnostics();
			ecsMetrics.renderItemCount = diagnostics.renderItemCount;
			ecsMetrics.visibleAllCount = diagnostics.visibleAllCount;
			ecsMetrics.visibleOpaqueCount = diagnostics.visibleOpaqueCount;
			ecsMetrics.visibleTransparentCount = diagnostics.visibleTransparentCount;
			ecsMetrics.frustumCulledCount = diagnostics.frustumCulledCount;
			ecsMetrics.shadowVisibleCount = diagnostics.shadowVisibleCount;
			ecsMetrics.forwardDrawCalls = diagnostics.forwardDrawCalls;
			ecsMetrics.geometryDrawCalls = diagnostics.geometryDrawCalls;
			ecsMetrics.shadowDrawCalls = diagnostics.shadowDrawCalls;
			ecsMetrics.velocityDrawCalls = diagnostics.velocityDrawCalls;
			ecsMetrics.transparentDrawCalls = diagnostics.transparentDrawCalls;
			ecsMetrics.materialUploadCount = diagnostics.materialUploadCount;
			ecsMetrics.materialCacheHitCount = diagnostics.materialCacheHitCount;
			ecsMetrics.textureBindCount = diagnostics.textureBindCount;
			ecsMetrics.transformFullUploadCount = diagnostics.transformFullUploads;
			ecsMetrics.transformPartialUploadCount = diagnostics.transformPartialUploads;
			ecsMetrics.transformUploadBytes = diagnostics.transformUploadBytes;
			ecsMetrics.cameraCacheBuildMs = diagnostics.cameraCacheBuildMs;
			ecsMetrics.shadowCacheBuildMs = diagnostics.shadowCacheBuildMs;
			ecsMetrics.transparentSortMs = diagnostics.transparentSortMs;
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
		std::vector<PerformanceRecorder::PassMetrics> passMetrics;
		float cpuWaitMs = 0.0f;
		if (m_modularRenderer) {
			const RenderContext& context = m_modularRenderer->GetContext();
			std::string renderMode = (context.rendererMode == RenderContext::RendererMode::PATH_TRACED) ?
				"Path-Traced" : "Standard";
			m_performanceRecorder->SetRenderingMode(renderMode);

			for (const auto& metric : m_modularRenderer->GetLastPassMetrics()) {
				passMetrics.push_back({
					metric.name,
					metric.cpuTimeMs,
					metric.gpuTimeMs,
					metric.drawCalls,
					metric.dispatchCount,
					metric.bufferUploadBytes,
					metric.cpuWaitSyncMs
				});
			}
			cpuWaitMs = m_modularRenderer->GetLastCpuWaitSyncMs();
		}
		m_performanceRecorder->SetCurrentFrameMetrics(passMetrics, ecsMetrics, cpuWaitMs, 0);
		m_performanceRecorder->RecordFrame(m_frameTime, m_fps);
	}
}

void Core::RenderSceneTo(rhi::FramebufferHandle framebuffer, int width, int height, const std::shared_ptr<Camera>& camera) {
	if (!m_modularRenderer || !camera || width <= 0 || height <= 0) return;
	const RenderContext& ctx = m_modularRenderer->GetContext();
	if (ctx.width != width || ctx.height != height) {
		m_modularRenderer->Resize(width, height);
		m_modularRenderer->ResetTAA();
	}
	camera->SetPerspective(camera->GetCameraFov(), static_cast<float>(width) / static_cast<float>(height),
		camera->GetCameraNearPlane(), camera->GetCameraFarPlane());
	PresentTarget::Set(framebuffer);
	if (RenderSystem* renderSystem = m_sceneGraph->GetRenderSystem()) {
		renderSystem->BeginFrameDiagnostics();
	}
	FrameProfiler::Mark("update_tail");
	m_modularRenderer->Render(
		m_sceneGraph,
		camera,
		m_lighting,
		m_frameTime * 0.001f,
		m_exposure,
		m_gamma,
		m_shadowsEnabled,
		m_shadowBias,
		m_shadowNear,
		m_shadowFar,
		m_environmentColor,
		width,
		height
	);
	MaybeDumpSceneFramebuffer(width, height);
	PresentTarget::Set(rhi::kBackBuffer);
}

void Core::Render(int windowWidth, int windowHeight) {
	if (!m_sceneGraph) return;

	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;
	if (!m_modularRenderer) {
		std::cerr << "[Core] ERROR: ModularRenderer is not initialized!" << std::endl;
		return;
	}

	// Play mode: a node with Camera.current drives the view (src/render/ActiveCamera.h); the editor camera otherwise.
	if (IsPlaying() && m_camera && m_sceneGraph) {
		nox::render::ApplyActiveCamera(*m_sceneGraph, *m_camera);
	}
	if (m_benchmark) {
		// Benchmarks measure the game view only: the editor UI is not drawn.
		RenderSceneTo(rhi::kBackBuffer, m_windowWidth, m_windowHeight, m_camera);
		std::vector<BenchmarkRunner::PassSample> passes;
		for (const auto& metric : m_modularRenderer->GetLastPassMetrics()) {
			passes.push_back({ metric.name, metric.gpuTimeMs, metric.cpuTimeMs });
		}
		FrameProfiler::Mark("render");
		const float cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_frameCpuStart).count();
		m_benchmark->OnFrameRendered(m_windowWidth, m_windowHeight, passes, cpuMs);
		m_quitRequested = m_benchmark->IsFinished();
		return;
	}

	const float dt = m_frameTime * 0.001f;
	if (!m_editor) {
		// Standalone game: full-window scene with the game's overlay composited on top.
		const std::shared_ptr<Camera> camera = (m_game && m_game->IsAvailable()) ? m_game->GetCamera() : m_camera;
		RenderSceneTo(rhi::kBackBuffer, m_windowWidth, m_windowHeight, camera);
		FrameProfiler::Mark("render_scene");
		if (FrameProfiler::Enabled()) {
			static int diagFrame = 0;
			const RenderSystem* rs = m_sceneGraph->GetRenderSystem();
			if (rs && ++diagFrame % 600 == 0) {
				const RenderSystem::Diagnostics& d = rs->GetDiagnostics();
				std::cout << "[RenderDiag] items=" << d.renderItemCount << " visibleOpaque=" << d.visibleOpaqueCount
					<< " visibleTransparent=" << d.visibleTransparentCount << " culled=" << d.frustumCulledCount
					<< " gbufferDraws=" << d.geometryDrawCalls << " velocityDraws=" << d.velocityDrawCalls
					<< " forwardDraws=" << d.forwardDrawCalls << " materialUploads=" << d.materialUploadCount
					<< " materialCacheHits=" << d.materialCacheHitCount << " textureBinds=" << d.textureBindCount
					<< " cameraCacheMs=" << d.cameraCacheBuildMs << std::endl;
			}
		}
		rhi::Gpu().BindFramebuffer(rhi::kBackBuffer);
		rhi::Gpu().SetViewport({ 0, 0, m_windowWidth, m_windowHeight });
		rhi::Gpu().ImGuiNewFrame();
		ImGui::NewFrame();
		if (m_game) {
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			m_game->DrawOverlay(ImGui::GetForegroundDrawList(), glm::vec2(0.0f), glm::vec2(size.x, size.y), dt);
		}
		DrawDebugPrimitives(ImGui::GetForegroundDrawList(), camera, 0.0f, 0.0f, static_cast<float>(m_windowWidth), static_cast<float>(m_windowHeight));
		FrameProfiler::Mark("overlay");
		ImGui::Render();
		rhi::Gpu().ImGuiRender(ImGui::GetDrawData());
		FrameProfiler::Mark("imgui");
		return;
	}

	// Editor: build the docked UI first (viewport rectangles), then render the scene into the
	// chosen viewport texture, then draw the UI to the window.
	rhi::Gpu().ImGuiNewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();

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
	m_editor->Draw(dt);
	// Legacy panels (rendering, lighting, profiler, animation, ...) dock into the workspace.
	m_imguiInterface->Render(m_windowWidth, m_windowHeight, m_fps, m_sceneName, m_lightPos, m_lightDir);
	if (m_editor->WantsQuit()) {
		m_quitRequested = true;
	}

	static EditorWorkspace::Target lastTarget = EditorWorkspace::Target::None;
	const EditorWorkspace::Target target = m_editor->RenderTarget();
	if (target != EditorWorkspace::Target::None) {
		EditorWorkspace::Viewport& vp = m_editor->GetViewport(target);
		const bool gameView = target == EditorWorkspace::Target::Game && m_game && m_game->IsAvailable();
		if (target != lastTarget) {
			ResetTemporalHistory(); // different camera: history would ghost
		}
		// Selection outline (SelectionOutlinePass): the editor's Scene viewport only, never while playing,
		// so the game viewport and standalone output keep it disabled.
		RenderContext::SelectionOutline& outline = m_modularRenderer->GetContext().selectionOutline;
		outline.enabled = false;
		outline.selectedEntities.clear();
		outline.hoveredEntities.clear();
		if (target == EditorWorkspace::Target::Scene && !m_editor->IsPlaying()) {
			const EditorWorkspace::SelectionOutlineSettings& style = m_editor->OutlineSettings();
			const std::shared_ptr<SceneNode> selected = m_imguiInterface ? m_imguiInterface->GetSelectedNode() : nullptr;
			const std::shared_ptr<SceneNode> hovered = m_editor->HoveredNode();
			if (selected) AppendSubtreeEntities(*selected, outline.selectedEntities);
			if (hovered && hovered != selected) AppendSubtreeEntities(*hovered, outline.hoveredEntities);
			std::sort(outline.selectedEntities.begin(), outline.selectedEntities.end());
			outline.selectedEntities.erase(std::unique(outline.selectedEntities.begin(), outline.selectedEntities.end()), outline.selectedEntities.end());
			std::sort(outline.hoveredEntities.begin(), outline.hoveredEntities.end());
			outline.hoveredEntities.erase(std::unique(outline.hoveredEntities.begin(), outline.hoveredEntities.end()), outline.hoveredEntities.end());
			outline.selectedColor = glm::vec3(style.color[0], style.color[1], style.color[2]);
			outline.selectedWidth = static_cast<float>(style.width);
			outline.occludedAlpha = style.showOccluded ? 0.45f : 0.0f;
			outline.enabled = !outline.selectedEntities.empty() || !outline.hoveredEntities.empty();
		}
		RenderSceneTo(vp.fbo, vp.texWidth, vp.texHeight, gameView ? m_game->GetCamera() : m_camera);
	}
	lastTarget = target;

	rhi::Gpu().BindFramebuffer(rhi::kBackBuffer);
	rhi::Gpu().SetViewport({ 0, 0, m_windowWidth, m_windowHeight });
	rhi::Gpu().SetClearColor(glm::vec4(0.05f, 0.05f, 0.06f, 1.0f));
	rhi::Gpu().Clear(rhi::ClearColor | rhi::ClearDepth);
	ImGui::Render();
	rhi::Gpu().ImGuiRender(ImGui::GetDrawData());
}

bool Core::ProcessInputEvent(const SDL_Event& event) {
	// Focus loss: key/button releases may never arrive; drop held input and end the fly camera.
	if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST && m_inputIntegration) {
		m_inputIntegration->OnFocusLost();
		m_editorCameraActive = false;
	}
	// The game module polls keyboard/pad state itself, gated by viewport focus.
	if (m_launch.mode == AppMode::Game || m_benchmark || !m_inputIntegration) {
		return true;
	}
	// Editor: the legacy fly-camera contexts only see input while the right mouse button is held
	// in the Scene viewport, so typing in an inspector never moves the camera or toggles features.
	const bool keyEvent = event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
	if (m_editorCameraActive) {
		if (event.type == SDL_MOUSEMOTION) {
			m_inputIntegration->ProcessEvent(event);
			return true;
		}
		if (keyEvent) {
			const SDL_Keycode k = event.key.keysym.sym;
			if (k == SDLK_w || k == SDLK_a || k == SDLK_s || k == SDLK_d || k == SDLK_q || k == SDLK_e) {
				m_inputIntegration->ProcessEvent(event);
				return true;
			}
		}
		return false;
	}
	if (event.type == SDL_KEYUP) {
		m_inputIntegration->ProcessEvent(event); // release keys held when flight ended
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
	rhi::Gpu().SetViewport({ 0, 0, m_windowWidth, m_windowHeight });

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
	if (m_playing && m_game) {
		m_game->EndPlay();
		m_playing = false;
	}

	const std::string resolvedSceneFile = ResolveSceneFilePath(newSceneFile);
	m_sceneToLoad = resolvedSceneFile;
	CleanupCurrentScene();

	if (m_sceneLoader) {
		auto newGraph = m_sceneLoader->LoadScene(resolvedSceneFile);
		if (newGraph) {
			m_sceneGraph = newGraph;
			m_currentSceneFilePath = resolvedSceneFile;

			m_exposure = m_sceneGraph->m_exposure;
			m_gamma = m_sceneGraph->m_gamma;
			m_sceneName = m_sceneGraph->GetSceneName();
			m_sceneHasAudioNodes = !m_sceneGraph->FindNodesByType(SceneNode::AUDIO).empty();
			ApplySceneCameraOverride(resolvedSceneFile);

			m_physicsEnabledForScene = m_sceneGraph->IsPhysicsEnabled();
			if (m_physicsEngine) {
				if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
			}

			// Update state manager with new scene file path
			if (m_stateManager) {
				m_stateManager->SetCurrentSceneFilePath(resolvedSceneFile);
			}

			auto lightManager = m_sceneGraph->GetLightManager();
			if (lightManager) {
				lightManager->GetShadowConfig().directionalSplitLambda = m_splitLambda;
				lightManager->InitializeShadowSystem(8, m_shadowSize);
				lightManager->CollectLightsFromScene(m_sceneGraph);
				if (lightManager->GetEnabledLightCount() == 0 && m_lighting) {
					lightManager->RegisterLight(m_lighting, "MainDirectionalLight");
				}
				lightManager->PrintLightInfo();
			}

			if (m_modularRenderer) {
				m_sceneHasRenderSettings = SceneRenderSettings::ApplyFromSceneFile(
					m_modularRenderer->GetContext(), resolvedSceneFile, m_sceneHasRenderSettings);
			}
			ConfigureGameFromScene(resolvedSceneFile);
			m_dirty = false;
			if (m_editor) {
				m_editor->OnSceneLoaded();
			}

			if (m_imguiInterface) {
				m_imguiInterface->SetSceneGraph(m_sceneGraph);
				// Re-read renderer settings into the editor so it does not push stale values back.
				m_imguiInterface->SetModularRenderer(m_modularRenderer);
			}

			if (m_inputIntegration) {
				m_inputIntegration->SetSceneGraph(m_sceneGraph);
				m_inputIntegration->SetLightManager(GetLightManager());
			}

			ComputeSceneBoundingBox();

			std::cout << "[Core] Scene swap completed successfully: " << resolvedSceneFile << std::endl;
		}
		else {
			std::cerr << "[Core] Failed to load scene: " << resolvedSceneFile << std::endl;
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

	if (m_playing) {
		std::cerr << "[Core] Stop play mode before saving (runtime state is not authored data)" << std::endl;
		return;
	}
	const std::string logicalScene = m_currentSceneFilePath.empty() ? m_sceneToLoad : m_currentSceneFilePath;
	// Authoring saves write the loose source project explicitly; packs are read-only.
	const auto sourceFile = AssetIO::SourcePath(logicalScene);
	if (!sourceFile) {
		std::cerr << "[Core] Cannot save '" << logicalScene << "': scenes are read-only in a packaged build" << std::endl;
		return;
	}
	const std::string saveFilePath = sourceFile->string();
	std::cout << "[Core] Saving scene to: " << saveFilePath << std::endl;

	if (m_sceneLoader->SaveScene(m_sceneGraph, saveFilePath)) {
		// Authoring data of the game layer (spawn, handling, camera, roads, events, HUD path).
		if (m_game && m_game->IsAvailable()) {
			m_authoredGame = m_game->SaveGameJson();
			std::ifstream in(saveFilePath);
			json root = in ? json::parse(in, nullptr, false) : json();
			in.close();
			if (root.is_object()) {
				root["game"] = m_authoredGame;
				std::ofstream out(saveFilePath);
				out << root.dump(2);
				m_sceneRootJson = root;
			}
		}
		m_dirty = false;
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

	// Stop all animations before cleanup
	if (m_sceneGraph && m_sceneGraph->GetAnimationSystem()) {
		std::cout << "[Core] Stopping all animations" << std::endl;
		m_sceneGraph->GetAnimationSystem()->StopAllAnimations();
	}

	// Clean up all physics bodies to prevent memory leaks and duplicate registrations
	if (m_physicsEngine) {
		std::cout << "[Core] Removing all physics bodies from engine" << std::endl;
		m_physicsEngine->RemoveAllBodies();
	}

	// Clear spatial acceleration structures
	if (m_sceneBVH) {
		m_sceneBVH->Clear();
	}

	// Clear UI selection and notify of scene change
	if (m_imguiInterface) {
		m_imguiInterface->SetSelectedNode(nullptr);
	}

	// Invalidate cached bounds
	m_boundingBoxCached = false;
	m_bvhDirty = true;
	m_sceneHasAudioNodes = false;

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
	auto lightManager = GetLightManager();
	if (lightManager) {
		lightManager->UpdateLightProxies();
		auto lightNode = lightManager->FindLightAtRay(ray.origin, ray.direction);
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
		const std::string resolvedSceneFile = ResolveSceneFilePath(sceneFile);
		auto newGraph = m_sceneLoader->LoadScene(resolvedSceneFile);
		if (!newGraph || !newGraph->GetRoot()) {
			std::cerr << "[Core] ERROR: Failed to load scene: " << resolvedSceneFile << std::endl;
			return nullptr;
		}

		// Update m_sceneGraph immediately so that state restoration
		// operates on the correct scene graph instance
		m_sceneGraph = newGraph;
		m_currentSceneFilePath = resolvedSceneFile;

		// Initialize scene systems (lights, physics, etc.)
		m_exposure = newGraph->m_exposure;
		m_gamma = newGraph->m_gamma;
		m_sceneName = newGraph->GetSceneName();
		m_sceneHasAudioNodes = !newGraph->FindNodesByType(SceneNode::AUDIO).empty();

		m_physicsEnabledForScene = newGraph->IsPhysicsEnabled();
		if (m_physicsEngine) {
			if (m_physicsEnabledForScene) m_physicsEngine->Resume(); else m_physicsEngine->Pause();
		}

		auto lightManager = newGraph->GetLightManager();
		if (lightManager) {
			lightManager->GetShadowConfig().directionalSplitLambda = m_splitLambda;
			lightManager->InitializeShadowSystem(8, m_shadowSize);
			lightManager->CollectLightsFromScene(newGraph);
			if (lightManager->GetEnabledLightCount() == 0 && m_lighting) {
				lightManager->RegisterLight(m_lighting, "MainDirectionalLight");
			}
			lightManager->PrintLightInfo();
		}

		// Update UI reference immediately so state restoration can access it
		if (m_imguiInterface) {
			m_imguiInterface->SetSceneGraph(newGraph);
		}

		if (m_inputIntegration) {
			m_inputIntegration->SetSceneGraph(newGraph);
			m_inputIntegration->SetLightManager(GetLightManager());
		}

		ComputeSceneBoundingBox();
		m_bvhDirty = true;
		m_boundingBoxCached = false;

		if (m_modularRenderer) {
			m_modularRenderer->ResetTAA();
		}

		std::cout << "[Core] Base scene loaded and initialized: " << resolvedSceneFile << std::endl;
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

	// m_sceneGraph was already updated in the callback, but update state manager path
	m_currentSceneFilePath = m_stateManager->GetCurrentSceneFilePath();

	// CRITICAL: Force immediate transform propagation through the hierarchy
	// After state restoration, all transforms are marked as dirty but world transforms
	// have not been computed yet. This ensures all node world transforms are correct
	// immediately, rather than waiting for the next update cycle.
	if (m_sceneGraph) {
		m_sceneGraph->UpdateAllTransforms();
		std::cout << "[Core] Transform hierarchy updated after state restoration" << std::endl;
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

// ---------------------------------------------------------------------------------------------
// Editor / game integration (docs/nfs2015/game.md)
// ---------------------------------------------------------------------------------------------
std::vector<std::string> Core::GetSceneList() const
{
	std::vector<std::string> scenes;
	auto add = [&](std::string p) {
		std::replace(p.begin(), p.end(), '\\', '/');
		if (std::find(scenes.begin(), scenes.end(), p) == scenes.end()) scenes.push_back(p);
	};
	AssetIO::ListOptions options;
	options.extension = ".json";
	for (const std::string& path : AssetIO::List("scenes", options)) add(path);
	std::sort(scenes.begin(), scenes.end());
	return scenes;
}

void Core::ConfigureGameFromScene(const std::string& scenePath)
{
	const auto text = AssetIO::ReadText(scenePath);
	m_sceneRootJson = text ? json::parse(*text, nullptr, false) : json();
	if (!m_sceneRootJson.is_object()) m_sceneRootJson = json::object();
	m_authoredGame = m_sceneRootJson.value("game", json());
	m_playing = false;
	SelectGameModule(m_sceneRootJson);
	if (m_game && m_sceneGraph) {
		m_game->Configure(m_authoredGame, m_sceneRootJson, *m_sceneGraph);
	}
}

void Core::ApplyGameJson(const json& game)
{
	if (!m_game || !m_sceneGraph || m_playing) return;
	m_authoredGame = game;
	m_game->Reconfigure(game, m_sceneRootJson, *m_sceneGraph);
}

json Core::GetAuthoredGameJson() const
{
	if (m_playing) return m_prePlayGame;
	return m_game ? m_game->SaveGameJson() : m_authoredGame;
}

std::string Core::ProjectForScene(const json& sceneRoot) const
{
	if (!m_launch.project.empty()) return m_launch.project;
	if (sceneRoot.is_object() && sceneRoot.contains("project") && sceneRoot["project"].is_string()) {
		return sceneRoot["project"].get<std::string>();
	}
	// The bootstrap "descriptor" only records which project a distribution packages; scenes (or the
	// launch options) select the module, so packaged and development builds behave the same.
	return {};
}

json Core::ReadStartupSceneRoot() const
{
	std::string scene = m_launch.scene.empty() ? m_config.value("first_scene", std::string()) : m_launch.scene;
	scene = ResolveSceneFilePath(scene);
	const auto text = scene.empty() ? std::nullopt : AssetIO::ReadText(scene);
	json root = text ? json::parse(*text, nullptr, false) : json();
	return root.is_object() ? root : json::object();
}

// Project selection (docs/engine_boundary.md): a scene or launch option naming a project descriptor
// gets that project's runtime module; otherwise the application's registered native module, if any.
void Core::SelectGameModule(const json& sceneRoot)
{
	const std::string project = ProjectForScene(sceneRoot);
	if (m_game && project == m_gameProject) return;
	if (!m_game && project.empty() && m_gameProjectResolved) return;
	if (m_game) {
		if (m_playing) m_game->EndPlay();
		m_playing = false;
		m_game.reset();
	}
	m_gameProject = project;
	m_gameProjectResolved = true;
	if (project.empty()) {
		m_game = GameModules::Create();
	}
	else {
		std::string error;
		m_game = GameModules::CreateForProject(project, error);
		if (!m_game) std::cerr << "[Core] Project '" << project << "' has no runnable module: " << error << std::endl;
	}
	if (!m_game) return;
	std::cout << "[Core] Game module: " << (project.empty() ? std::string("(application native)") : project) << std::endl;
	m_game->SetStandalone(m_launch.mode == AppMode::Game);
	GameModuleServices services;
	services.physics = m_physicsEngine.get();
	m_game->SetServices(services);
	if (m_camera) m_game->SetCameraTemplate(*m_camera);
	if (m_gameResourcesReady) {
		m_game->InitializeResources();
		// Fonts added by the module invalidate the atlas; re-upload it (also safe before frame one).
		if (!ImGui::GetIO().Fonts->IsBuilt()) rhi::Gpu().ImGuiRebuildFonts();
	}
}

void Core::RequestPlay()
{
	if (m_playing || !m_game || !m_game->IsAvailable() || !m_sceneGraph) return;
	m_prePlayGame = m_game->SaveGameJson();
	m_prePlayTransforms.clear();
	std::function<void(const std::shared_ptr<SceneNode>&)> snap = [&](const std::shared_ptr<SceneNode>& n) {
		if (!n) return;
		const glm::vec3 p = n->GetPosition();
		const glm::quat q = n->GetOrientation();
		const glm::vec3 sc = n->GetScale();
		m_prePlayTransforms.push_back({ n, { p.x, p.y, p.z, q.w, q.x, q.y, q.z, sc.x, sc.y, sc.z } });
		for (const auto& c : n->children) snap(c);
	};
	for (const auto& c : m_sceneGraph->GetRoot()->children) snap(c);
	m_playWorld.Capture(*m_sceneGraph, *m_sceneLoader);
	m_playing = true;
	m_game->BeginPlay();
	ResetTemporalHistory();
	std::cout << "[Core] Play" << std::endl;
}

void Core::RequestStop()
{
	if (!m_playing || !m_game) return;
	m_game->EndPlay();
	if (m_sceneGraph && m_sceneLoader) {
		const PlayWorldSnapshot::Report report = m_playWorld.Restore(*m_sceneGraph, m_physicsEngine.get(), *m_sceneLoader,
			m_currentSceneFilePath);
		if (report.removedNodes > 0 || report.recreatedNodes > 0) {
			if (m_sceneGraph->GetLightManager()) m_sceneGraph->UpdateLightManager();
			m_bvhDirty = true;
			m_boundingBoxCached = false;
			m_dirty = true;
		}
		std::cout << "[Core] Play world restored: nodes removed " << report.removedNodes << ", re-created "
			<< report.recreatedNodes << ", entities removed " << report.destroyedEntities << ", transforms "
			<< report.restoredTransforms << ", bodies " << report.restoredBodies << ", component values "
			<< report.restoredComponents << std::endl;
	}
	for (const auto& [weak, t] : m_prePlayTransforms) {
		if (auto n = weak.lock()) {
			n->SetLocalTRS(glm::vec3(t[0], t[1], t[2]), glm::quat(t[3], t[4], t[5], t[6]), glm::vec3(t[7], t[8], t[9]));
		}
	}
	m_prePlayTransforms.clear();
	m_playing = false;
	// Physics tuning made during play (scripts and the Physics window) is dropped: runtime = authored again.
	if (m_physicsEngine) m_physicsEngine->SetRuntimeConfig(m_physicsEngine->GetAuthoredConfig());
	if (m_sceneGraph) {
		m_sceneGraph->UpdateAllTransforms();
		m_game->Configure(m_prePlayGame, m_sceneRootJson, *m_sceneGraph);
	}
	ResetTemporalHistory();
	std::cout << "[Core] Stop (pre-play state restored)" << std::endl;
}

void Core::TogglePause()
{
	if (m_playing && m_game) m_game->SetPaused(!m_game->IsPaused());
}

bool Core::IsPaused() const
{
	return m_playing && m_game && m_game->IsPaused();
}

void Core::ApplyRuntimeChangesToAuthored()
{
	if (!m_playing || !m_game) return;
	m_prePlayGame = m_game->SaveGameJson();
	m_authoredGame = m_prePlayGame;
	m_dirty = true;
}

void Core::ResetTemporalHistory()
{
	if (m_modularRenderer) m_modularRenderer->ResetTAA();
	if (m_sceneGraph) {
		if (TransformSystem* ts = m_sceneGraph->GetTransformSystem()) ts->SnapshotPreviousTransforms();
	}
}

void Core::SetEditorCameraActive(bool active)
{
	if (m_editorCameraActive == active) return;
	m_editorCameraActive = active;
	SetMouseLocked(active);
}

std::shared_ptr<SceneNode> Core::PickNode(int x, int y, int width, int height)
{
	if (!m_camera || !m_sceneGraph || width <= 0 || height <= 0) return nullptr;
	m_camera->SetPerspective(m_camera->GetCameraFov(), static_cast<float>(width) / static_cast<float>(height),
		m_camera->GetCameraNearPlane(), m_camera->GetCameraFarPlane());
	RayCast::Ray ray = RayCast::ScreenToWorldRay(x, y, m_camera, width, height);
	if (glm::length(ray.direction) < 0.1f) return nullptr;
	if (m_bvhDirty) RebuildSceneBVH();
	return PerformRayQuery(ray);
}

namespace {
	std::shared_ptr<SceneNode> FindParentNode(const std::shared_ptr<SceneNode>& root, const SceneNode* child) {
		if (!root) return nullptr;
		for (const auto& c : root->children) {
			if (c.get() == child) return root;
			if (auto p = FindParentNode(c, child)) return p;
		}
		return nullptr;
	}
}

std::shared_ptr<SceneNode> Core::DuplicateNode(const std::shared_ptr<SceneNode>& node)
{
	if (!node || !m_sceneLoader || !m_sceneGraph || m_playing) return nullptr;
	json j = m_sceneLoader->SerializeNodeForEditor(node);
	if (!j.is_object()) return nullptr;
	j["name"] = node->GetName() + "_copy";
	if (j.contains("position") && j["position"].is_array() && j["position"].size() == 3) {
		j["position"][0] = j["position"][0].get<float>() + 2.0f;
	}
	auto parent = FindParentNode(m_sceneGraph->GetRoot(), node.get());
	return InstantiateNodeFromJson(j, parent);
}

nlohmann::json Core::SerializeNodeForEditor(const std::shared_ptr<SceneNode>& node)
{
	if (!m_sceneLoader || !node) return json();
	return m_sceneLoader->SerializeNodeForEditor(node);
}

std::shared_ptr<SceneNode> Core::InstantiateNodeFromJson(const json& nodeJson, const std::shared_ptr<SceneNode>& parent)
{
	if (!m_sceneLoader || !m_sceneGraph || m_playing) return nullptr;
	auto target = parent ? parent : m_sceneGraph->GetRoot();
	auto node = m_sceneLoader->InstantiateNode(*m_sceneGraph, nodeJson, target, m_currentSceneFilePath);
	if (node) {
		if (auto lm = m_sceneGraph->GetLightManager()) {
			std::function<void(const std::shared_ptr<SceneNode>&)> reg = [&](const std::shared_ptr<SceneNode>& n) {
				if (auto ln = std::dynamic_pointer_cast<LightNode>(n)) lm->RegisterLightNode(ln);
				for (const auto& c : n->children) reg(c);
			};
			reg(node);
		}
		m_bvhDirty = true;
		m_boundingBoxCached = false;
		m_dirty = true;
	}
	return node;
}

std::shared_ptr<SceneNode> Core::InstantiatePrefabNode(const std::string& logicalPath, const std::shared_ptr<SceneNode>& parent,
	const json& placement, std::string* error)
{
	if (!m_sceneLoader || !m_sceneGraph) {
		if (error) *error = "no scene is loaded";
		return nullptr;
	}
	auto target = parent ? parent : m_sceneGraph->GetRoot();
	auto node = nox::prefab::InstantiatePrefab(*m_sceneLoader, *m_sceneGraph, logicalPath, target, placement, error);
	if (node) {
		if (auto lm = m_sceneGraph->GetLightManager()) {
			std::function<void(const std::shared_ptr<SceneNode>&)> reg = [&](const std::shared_ptr<SceneNode>& n) {
				if (auto ln = std::dynamic_pointer_cast<LightNode>(n)) lm->RegisterLightNode(ln);
				for (const auto& c : n->children) reg(c);
			};
			reg(node);
		}
		m_bvhDirty = true;
		m_boundingBoxCached = false;
		m_dirty = true;
	}
	return node;
}

bool Core::SavePrefabFromNode(const std::shared_ptr<SceneNode>& node, const std::string& logicalPath, std::string* error)
{
	if (!m_sceneLoader) {
		if (error) *error = "no scene loader";
		return false;
	}
	return nox::prefab::SavePrefab(*m_sceneLoader, node, logicalPath, error);
}

bool Core::DestroySceneNodeForPlay(const std::shared_ptr<SceneNode>& node)
{
	if (!node || !m_sceneGraph || node == m_sceneGraph->GetRoot()) return false;
	if (!PlayWorldSnapshot::RemoveSubtree(*m_sceneGraph, node, m_physicsEngine.get())) return false;
	m_bvhDirty = true;
	m_boundingBoxCached = false;
	m_dirty = true;
	return true;
}

bool Core::ReparentNode(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& newParent, int index, bool keepWorld)
{
	if (!node || !newParent || !m_sceneGraph || m_playing || node == m_sceneGraph->GetRoot()) return false;
	for (auto p = newParent; p; p = p->parentNode.lock()) {
		if (p == node) return false;   // would create a cycle
	}
	const glm::mat4 world = node->GetWorldPosition4x4();
	const auto oldParent = node->parentNode.lock();
	if (oldParent == newParent) {
		// AddChild only detaches from a different parent; drop the existing entry first.
		auto& siblings = newParent->children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
	}
	newParent->AddChild(node);
	if (index >= 0) {
		auto& siblings = newParent->children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
		siblings.insert(siblings.begin() + std::min(static_cast<size_t>(index), siblings.size()), node);
	}
	if (keepWorld) node->SetWorldTransform(world);
	m_sceneGraph->UpdateAllTransforms();
	m_bvhDirty = true;
	m_boundingBoxCached = false;
	m_dirty = true;
	return true;
}

std::shared_ptr<SceneNode> Core::AddModelNode(const std::string& modelPath, const glm::vec3& position,
	const std::shared_ptr<SceneNode>& parent)
{
	if (!m_sceneLoader || !m_sceneGraph || m_playing) return nullptr;
	json j = { {"type", "model"}, {"name", fs::path(modelPath).stem().string()}, {"path", modelPath},
		{"position", { position.x, position.y, position.z }}, {"rotation", { 0.0f, 0.0f, 0.0f }}, {"scale", { 1.0f, 1.0f, 1.0f }} };
	return InstantiateNodeFromJson(j, parent);
}

bool Core::DeleteNode(const std::shared_ptr<SceneNode>& node)
{
	if (!node || !m_sceneGraph || m_playing) return false;
	auto parent = FindParentNode(m_sceneGraph->GetRoot(), node.get());
	if (!parent) return false;
	auto lm = m_sceneGraph->GetLightManager();
	ComponentManager* cm = m_sceneGraph->GetComponentManager();
	std::function<void(const std::shared_ptr<SceneNode>&)> destroy = [&](const std::shared_ptr<SceneNode>& n) {
		for (const auto& c : n->children) destroy(c);
		if (auto ln = std::dynamic_pointer_cast<LightNode>(n)) {
			if (lm && ln->GetLight()) lm->UnregisterLight(ln->GetLight());
		}
		if (cm && n->GetEntityID() != INVALID_ENTITY) cm->DestroyEntity(n->GetEntityID());
	};
	destroy(node);
	auto& siblings = parent->children;
	siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
	m_sceneGraph->UpdateAllTransforms();
	m_bvhDirty = true;
	m_boundingBoxCached = false;
	m_dirty = true;
	return true;
}
