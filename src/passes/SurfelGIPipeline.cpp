#include "SurfelGIPipeline.h"

#include "../ComputeShader.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
constexpr uint32_t kRadialDepthTexelsPerSurfel = 16u;
constexpr uint32_t kGuideCellsPerSurfel = 36u;

void DeleteBuffer(GLuint& buffer)
{
	if (buffer != 0u) {
		glDeleteBuffers(1, &buffer);
		buffer = 0u;
	}
}

void DeleteTexture(GLuint& texture)
{
	if (texture != 0u) {
		glDeleteTextures(1, &texture);
		texture = 0u;
	}
}

bool AllocateBuffer(GLuint& buffer, GLsizeiptr sizeBytes, const char* label)
{
	if (sizeBytes <= 0) {
		return false;
	}

	if (buffer == 0u) {
		glGenBuffers(1, &buffer);
	}
	if (buffer == 0u) {
		return false;
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeBytes, nullptr, GL_DYNAMIC_DRAW);
	const GLenum error = glGetError();
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	if (error != GL_NO_ERROR) {
		std::cerr << "[SurfelGIPipeline] Failed to allocate " << label
			<< " (" << sizeBytes << " bytes, GL error 0x"
			<< std::hex << error << std::dec << ").\n";
		DeleteBuffer(buffer);
		return false;
	}

	if (GLEW_KHR_debug) {
		glObjectLabel(GL_BUFFER, buffer, -1, label);
	}
	return true;
}

bool AllocateIndirectTexture(GLuint& texture, uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u) {
		return false;
	}

	if (texture == 0u) {
		glGenTextures(1, &texture);
	}
	if (texture == 0u) {
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA16F,
		static_cast<GLsizei>(width),
		static_cast<GLsizei>(height),
		0,
		GL_RGBA,
		GL_FLOAT,
		nullptr);
	const GLenum error = glGetError();
	glBindTexture(GL_TEXTURE_2D, 0);

	if (error != GL_NO_ERROR) {
		std::cerr << "[SurfelGIPipeline] Failed to allocate indirect texture ("
			<< width << "x" << height << ", GL error 0x"
			<< std::hex << error << std::dec << ").\n";
		DeleteTexture(texture);
		return false;
	}

	if (GLEW_KHR_debug) {
		glObjectLabel(GL_TEXTURE, texture, -1, "SurfelGI.IndirectDiffuse");
	}
	return true;
}

bool CheckedBytes(uint64_t elementCount, uint64_t elementSize, GLsizeiptr& outBytes)
{
	if (elementCount == 0u || elementSize == 0u) {
		return false;
	}
	const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max());
	if (elementCount > maxSize / elementSize) {
		return false;
	}
	outBytes = static_cast<GLsizeiptr>(elementCount * elementSize);
	return true;
}

uint32_t ScaledExtent(uint32_t extent, float scale)
{
	const float safeScale = std::clamp(scale, 0.25f, 1.0f);
	return std::max(1u, static_cast<uint32_t>(std::ceil(static_cast<float>(std::max(extent, 1u)) * safeScale)));
}

bool CreateCompute(std::unique_ptr<ComputeShader>& shader, const char* path)
{
	shader = std::make_unique<ComputeShader>();
	if (!shader->CreateFromFile(path)) {
		std::cerr << "[SurfelGIPipeline] Failed to compile " << path << ".\n";
		shader.reset();
		return false;
	}
	return true;
}

bool IsSurfelFullscreenDebugView(SurfelGIDebugView view)
{
	return view == SurfelGIDebugView::SurfelGridCells ||
		view == SurfelGIDebugView::GatherWeights ||
		view == SurfelGIDebugView::GBufferWorldPosition ||
		view == SurfelGIDebugView::GBufferNormal ||
		view == SurfelGIDebugView::GBufferTransformID ||
		view == SurfelGIDebugView::GBufferMaterialID ||
		view == SurfelGIDebugView::SpawnCandidates;
}

glm::vec3 CameraPositionFromView(const glm::mat4& view)
{
	const glm::mat4 invView = glm::inverse(view);
	return glm::vec3(invView[3]);
}

void ComputeCameraCenteredGridBounds(const SurfelGridSettings& gridSettings,
	const glm::vec3& cameraPosition,
	glm::vec3& gridMin,
	glm::vec3& gridMax)
{
	const glm::vec3 halfExtent = gridSettings.worldExtent * 0.5f;
	gridMin = cameraPosition - halfExtent;
	gridMax = cameraPosition + halfExtent;
}

void SetUniform1ui(GLuint program, const char* name, GLuint value)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform1ui(loc, value);
	}
}

void SetUniform2ui(GLuint program, const char* name, GLuint x, GLuint y)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform2ui(loc, x, y);
	}
}

void SetUniform3ui(GLuint program, const char* name, GLuint x, GLuint y, GLuint z)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform3ui(loc, x, y, z);
	}
}

void SetUniform1i(GLuint program, const char* name, GLint value)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform1i(loc, value);
	}
}

void SetUniform1f(GLuint program, const char* name, GLfloat value)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform1f(loc, value);
	}
}

void SetUniform2f(GLuint program, const char* name, GLfloat x, GLfloat y)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform2f(loc, x, y);
	}
}

void SetUniform3fv(GLuint program, const char* name, const glm::vec3& value)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniform3fv(loc, 1, glm::value_ptr(value));
	}
}

void SetUniformMat4(GLuint program, const char* name, const glm::mat4& value)
{
	const GLint loc = glGetUniformLocation(program, name);
	if (loc >= 0) {
		glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(value));
	}
}

GLuint DivRoundUp(GLuint value, GLuint divisor)
{
	return (value + divisor - 1u) / divisor;
}

float MatrixMaxAbsDelta(const glm::mat4& a, const glm::mat4& b)
{
	float maxDelta = 0.0f;
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			maxDelta = std::max(maxDelta, std::abs(a[column][row] - b[column][row]));
		}
	}
	return maxDelta;
}

uint32_t ComputeSpawnPassCount(const SurfelGISettings& settings,
	uint32_t frameIndex,
	uint32_t stationaryFrameCount)
{
	const uint32_t steadyPasses = std::clamp(settings.spawnPasses, 1u, 16u);
	const uint32_t fastPasses = std::clamp(settings.fastFillSpawnPasses, steadyPasses, 16u);
	const bool initialFill = frameIndex < settings.fastFillFrameCount;
	const bool stationaryFill = stationaryFrameCount > 0u &&
		stationaryFrameCount <= settings.stationaryFastFillFrames;
	return initialFill || stationaryFill ? fastPasses : steadyPasses;
}
}

SurfelGIPipeline::~SurfelGIPipeline()
{
	Shutdown();
}

bool SurfelGIPipeline::Init(RenderContext& context, const SurfelGISettings& settings)
{
	m_settings = settings;
	m_width = static_cast<uint32_t>(std::max(context.width, 1));
	m_height = static_cast<uint32_t>(std::max(context.height, 1));
	m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
	m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
	m_resources = {};
	m_stats = {};
	m_stats.configuredEnabled = m_settings.enabled;
	m_stats.ready = false;
	m_frameIndex = 0;
	m_stationaryFrameCount = 0;
	m_hasLastCameraState = false;
	m_initialized = true;

	if (!m_settings.enabled) {
		return true;
	}

	SurfelGridSettings gridSettings{};
	gridSettings.maxSurfelsPerCell = m_settings.maxSurfelsPerCell;
	gridSettings.useNonLinearGrid = m_settings.useNonLinearGrid;

	const bool ok =
		m_pool.Create(m_settings.maxSurfels) &&
		m_grid.Create(gridSettings) &&
		m_rayQueue.Create(m_settings.maxRayBudget, m_settings.maxSurfels) &&
		CreateAuxiliaryResources() &&
		CreateIndirectTexture() &&
		LoadShaders();

	if (!ok) {
		Shutdown();
		return false;
	}

	m_resources.surfelBuffer = m_pool.GetSurfelBuffer();
	m_resources.freeListBuffer = m_pool.GetFreeListBuffer();
	m_resources.countersBuffer = m_pool.GetCountersBuffer();
	m_resources.gridHeaderBuffer = m_grid.GetCellHeaderBuffer();
	m_resources.gridEntryBuffer = m_grid.GetCellEntryBuffer();
	m_resources.gridAverageBuffer = m_grid.GetCellAverageBuffer();
	m_resources.gridCountersBuffer = m_grid.GetCountersBuffer();
	m_resources.rayRequestBuffer = m_rayQueue.GetRequestBuffer();
	m_resources.rayBuffer = m_rayQueue.GetRayBuffer();
	m_resources.sortedRayBuffer = m_rayQueue.GetSortedRayBuffer();
	m_resources.rayHitBuffer = m_rayQueue.GetRayHitBuffer();
	m_resources.rayBinBuffer = m_rayQueue.GetRayBinBuffer();
	m_resources.rayCountersBuffer = m_rayQueue.GetCountersBuffer();
	m_stats.ready = IsReady();
	return true;
}

void SurfelGIPipeline::Resize(RenderContext&, uint32_t width, uint32_t height)
{
	m_width = std::max(width, 1u);
	m_height = std::max(height, 1u);
	m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
	m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
	if (m_settings.enabled && m_initialized) {
		ReleaseIndirectTexture();
		CreateIndirectTexture();
	}
}

void SurfelGIPipeline::Shutdown()
{
	ReleaseShaders();
	ReleaseIndirectTexture();
	ReleaseAuxiliaryResources();
	m_rayQueue.Destroy();
	m_grid.Destroy();
	m_pool.Destroy();
	m_resources = {};
	m_stats = {};
	m_width = 0;
	m_height = 0;
	m_indirectWidth = 0;
	m_indirectHeight = 0;
	m_frameIndex = 0;
	m_stationaryFrameCount = 0;
	m_hasLastCameraState = false;
	m_initialized = false;
}

void SurfelGIPipeline::BeginFrame(RenderContext&,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&)
{
	m_stats.configuredEnabled = m_settings.enabled;
	m_stats.ready = IsReady();
}

void SurfelGIPipeline::Execute(RenderContext& context,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>&)
{
	if (!IsReady() || !context.gbufferFBO) {
		m_stats.ready = false;
		return;
	}

	const uint32_t maxSurfels = m_settings.maxSurfels;
	const GLuint groupsSurfels = DivRoundUp(maxSurfels, 64u);
	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = camera ? camera->GetCameraPosition() : CameraPositionFromView(context.view);
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);

	GLuint transformBuffer = 0u;
	uint32_t transformCount = 0u;
	if (sceneGraph) {
		if (RenderSystem* renderSystem = sceneGraph->GetRenderSystem()) {
			transformBuffer = renderSystem->GetTransformBufferID();
			transformCount = static_cast<uint32_t>(std::min<size_t>(
				renderSystem->GetTransformRecordCount(),
				std::numeric_limits<uint32_t>::max()));
		}
	}
	m_lastTransformCount = transformCount;

	BindCoreResources(transformBuffer);

	if (m_hasLastCameraState) {
		const float positionDelta = glm::length(cameraPosition - m_lastCameraPosition);
		const float viewDelta = MatrixMaxAbsDelta(context.view, m_lastView);
		if (positionDelta <= m_settings.stationaryCameraEpsilon &&
			viewDelta <= m_settings.stationaryCameraEpsilon) {
			m_stationaryFrameCount = std::min(m_stationaryFrameCount + 1u, 1024u);
		} else {
			m_stationaryFrameCount = 0u;
		}
	}
	m_lastCameraPosition = cameraPosition;
	m_lastView = context.view;
	m_hasLastCameraState = true;

	auto buildCellAverages = [&]() {
		if (m_cellAverageShader && m_cellAverageShader->IsValid()) {
			const GLuint program = m_cellAverageShader->GetProgramID();
			glUseProgram(program);
			SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
			SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			m_cellAverageShader->Dispatch(DivRoundUp(m_grid.GetCellCount(), 64u), 1u, 1u);
			m_cellAverageShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
		}
	};

	auto rebuildGridAndAverages = [&]() {
		if (m_clearGridShader && m_clearGridShader->IsValid()) {
			const GLuint program = m_clearGridShader->GetProgramID();
			glUseProgram(program);
			SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
			SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
			m_clearGridShader->Dispatch(DivRoundUp(m_grid.GetCellCount(), 64u), 1u, 1u);
			m_clearGridShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
		}

		if (m_buildGridShader && m_buildGridShader->IsValid()) {
			const GLuint program = m_buildGridShader->GetProgramID();
			glUseProgram(program);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
			SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
			SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
			SetUniform3fv(program, "uGridMin", gridMin);
			SetUniform3fv(program, "uGridMax", gridMax);
			m_buildGridShader->Dispatch(groupsSurfels, 1u, 1u);
			m_buildGridShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
		}

		buildCellAverages();
	};

	if (m_beginFrameShader && m_beginFrameShader->IsValid()) {
		glUseProgram(m_beginFrameShader->GetProgramID());
		m_beginFrameShader->Dispatch(1u, 1u, 1u);
		m_beginFrameShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (m_updateShader && m_updateShader->IsValid() && transformBuffer != 0u && transformCount > 0u) {
		const GLuint program = m_updateShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uTransformCount", transformCount);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform2f(program, "uViewportSize", static_cast<float>(m_width), static_cast<float>(m_height));
		SetUniformMat4(program, "uView", context.view);
		SetUniformMat4(program, "uProjection", context.proj);
		SetUniform1f(program, "uTargetSurfelScreenRadiusPx", m_settings.targetSurfelScreenRadiusPx);
		SetUniform1f(program, "uMinSurfelRadius", m_settings.minSurfelRadius);
		SetUniform1f(program, "uMaxSurfelRadius", m_settings.maxSurfelRadius);
		m_updateShader->Dispatch(groupsSurfels, 1u, 1u);
		m_updateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (m_recycleShader && m_recycleShader->IsValid()) {
		const GLuint program = m_recycleShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform3fv(program, "uCameraPosition", cameraPosition);
		SetUniform3fv(program, "uGridMin", gridMin);
		SetUniform3fv(program, "uGridMax", gridMax);
		SetUniform1f(program, "uRecyclePressureStart", m_settings.recyclePressureStart);
		SetUniform1f(program, "uMaxDistance", glm::length(gridSettings.worldExtent));
		m_recycleShader->Dispatch(groupsSurfels, 1u, 1u);
		m_recycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	rebuildGridAndAverages();

	if (m_spawnShader && m_spawnShader->IsValid()) {
		const GLuint tileSize = std::max(m_settings.spawnTileSize, 1u);
		const GLuint tilesX = DivRoundUp(m_width, tileSize);
		const GLuint tilesY = DivRoundUp(m_height, tileSize);
		const GLuint spawnPassCount = ComputeSpawnPassCount(m_settings, m_frameIndex, m_stationaryFrameCount);
		const GLuint tileCount = tilesX * tilesY;
		const glm::vec3 lightDirection = dirLight ? dirLight->GetLightDirection() : glm::vec3(-0.35f, -1.0f, -0.25f);
		const glm::vec3 lightRadiance = dirLight ? dirLight->GetLightColor() * dirLight->GetIntensity() : glm::vec3(1.0f);
		const GLuint program = m_spawnShader->GetProgramID();
		glUseProgram(program);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(0));
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(1));
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(3));
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(4));
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(5));
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetDepthTexture());

		SetUniform1i(program, "uPackedNormalRMTex", 0);
		SetUniform1i(program, "uAlbedoAOTex", 1);
		SetUniform1i(program, "uMaterialIDTex", 2);
		SetUniform1i(program, "uEmissiveTex", 3);
		SetUniform1i(program, "uTransformIDTex", 4);
		SetUniform1i(program, "uDepthTex", 5);
		SetUniform1ui(program, "uMaxSpawnCandidates", tileCount);
		SetUniform2ui(program, "uTileCount", tilesX, tilesY);
		SetUniform1ui(program, "uTileSize", tileSize);
		SetUniform1ui(program, "uSpawnPassCount", spawnPassCount);
		SetUniform2ui(program, "uResolution", m_width, m_height);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uTransformCount", transformCount);
		SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
		SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
		SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
		SetUniform3fv(program, "uGridMin", gridMin);
		SetUniform3fv(program, "uGridMax", gridMax);
		SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
		SetUniformMat4(program, "uInvView", glm::inverse(context.view));
		SetUniform1f(program, "uSpawnRadius", m_settings.minSurfelRadius);
		SetUniform1f(program, "uCoverageThreshold", m_settings.spawnCoverageThreshold);
		SetUniform1f(program, "uNormalRejectCos", m_settings.normalRejectCos);
		SetUniform3fv(program, "uDirectionalLightDirection", lightDirection);
		SetUniform3fv(program, "uDirectionalLightRadiance", lightRadiance);
		SetUniform3fv(program, "uSkyRadiance", context.envColor * m_settings.skyMissRadianceMultiplier);

		// Spawned surfels must be visible to same-frame ray tracing and final
		// gather; otherwise the composited output can remain one frame behind
		// or black while the pool is filling. Rebuild between bounded spawn
		// passes so later visible-tile samples observe fresh coverage.
		for (GLuint spawnPass = 0u; spawnPass < spawnPassCount; ++spawnPass) {
			SetUniform1ui(program, "uSpawnPassIndex", spawnPass);
			m_spawnShader->Dispatch(DivRoundUp(tileCount, 64u), 1u, 1u);
			m_spawnShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
			rebuildGridAndAverages();
			glUseProgram(program);
		}
	}

	const uint32_t rayUpdateInterval = std::max(m_settings.rayUpdateInterval, 1u);
	const bool updateRaysThisFrame = m_frameIndex < 8u || (m_frameIndex % rayUpdateInterval) == 0u;
	if (updateRaysThisFrame && m_requestRaysShader && m_requestRaysShader->IsValid()) {
		const GLuint program = m_requestRaysShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		m_requestRaysShader->Dispatch(groupsSurfels, 1u, 1u);
		m_requestRaysShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (updateRaysThisFrame && m_allocateRaysShader && m_allocateRaysShader->IsValid()) {
		const GLuint program = m_allocateRaysShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRayBudget", m_settings.maxRayBudget);
		m_allocateRaysShader->Dispatch(groupsSurfels, 1u, 1u);
		m_allocateRaysShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (updateRaysThisFrame && m_generateRaysShader && m_generateRaysShader->IsValid()) {
		const GLuint program = m_generateRaysShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1i(program, "uUseRayGuiding", m_settings.useRayGuiding ? 1 : 0);
		SetUniform1f(program, "uRayTMin", 0.01f);
		SetUniform1f(program, "uRayTMax", std::max(m_settings.maxSurfelRadius * 16.0f, 4.0f));
		m_generateRaysShader->Dispatch(groupsSurfels, 1u, 1u);
		m_generateRaysShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (updateRaysThisFrame && m_traceRaysShader && m_traceRaysShader->IsValid()) {
		const GLuint rayBudget = std::max(m_settings.maxRayBudget, 1u);
		const glm::vec3 lightDirection = dirLight ? dirLight->GetLightDirection() : glm::vec3(-0.35f, -1.0f, -0.25f);
		const glm::vec3 lightRadiance = dirLight ? dirLight->GetLightColor() * dirLight->GetIntensity() : glm::vec3(1.0f);
		const GLuint program = m_traceRaysShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uActiveRayCount", rayBudget);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
		SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
		SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
		SetUniform3fv(program, "uGridMin", gridMin);
		SetUniform3fv(program, "uGridMax", gridMax);
		SetUniform1f(program, "uNormalRejectCos", m_settings.normalRejectCos);
		SetUniform3fv(program, "uDirectionalLightDirection", lightDirection);
		SetUniform3fv(program, "uDirectionalLightRadiance", lightRadiance);
		SetUniform3fv(program, "uSkyRadiance", context.envColor * m_settings.skyMissRadianceMultiplier);
		m_traceRaysShader->Dispatch(DivRoundUp(rayBudget, 64u), 1u, 1u);
		m_traceRaysShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (updateRaysThisFrame && m_integrateShader && m_integrateShader->IsValid()) {
		const GLuint program = m_integrateShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1i(program, "uUseRayGuiding", m_settings.useRayGuiding ? 1 : 0);
		m_integrateShader->Dispatch(groupsSurfels, 1u, 1u);
		m_integrateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	const uint32_t radialUpdateInterval = std::max(m_settings.radialDepthUpdateInterval, 1u);
	if (updateRaysThisFrame && m_settings.useRadialDepth &&
		(m_frameIndex % radialUpdateInterval) == 0u &&
		m_radialDepthShader && m_radialDepthShader->IsValid()) {
		const GLuint program = m_radialDepthShader->GetProgramID();
		glUseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		m_radialDepthShader->Dispatch(groupsSurfels, 1u, 1u);
		m_radialDepthShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
	}

	if (updateRaysThisFrame) {
		buildCellAverages();
	}

	glUseProgram(0);
	++m_frameIndex;
	m_stats.ready = IsReady();
}

void SurfelGIPipeline::ApplyIndirect(RenderContext& context, FrameBuffer&)
{
	if (!IsReady() || !m_applyIndirectShader || !m_applyIndirectShader->IsValid() ||
		m_resources.indirectTexture == 0u || m_resources.rawIndirectTexture == 0u || !context.gbufferFBO) {
		m_stats.ready = IsReady();
		return;
	}

	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = CameraPositionFromView(context.view);
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);
	const GLuint applyDebugView = IsSurfelFullscreenDebugView(m_settings.debugView)
		? static_cast<GLuint>(m_settings.debugView)
		: 0u;
	const bool fullscreenDebug = applyDebugView != 0u;
	BindCoreResources(0u);

	const GLuint program = m_applyIndirectShader->GetProgramID();
	glUseProgram(program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(0));
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetDepthTexture());
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(3));
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(5));
	glBindImageTexture(0,
		fullscreenDebug ? m_resources.indirectTexture : m_resources.rawIndirectTexture,
		0,
		GL_FALSE,
		0,
		GL_WRITE_ONLY,
		GL_RGBA16F);

	SetUniform1i(program, "uPackedNormalRMTex", 0);
	SetUniform1i(program, "uDepthTex", 1);
	SetUniform1i(program, "uMaterialIDTex", 2);
	SetUniform1i(program, "uTransformIDTex", 3);
	SetUniform1i(program, "uUseMaterialIDReject", 1);
	SetUniform2ui(program, "uResolution", m_indirectWidth, m_indirectHeight);
	SetUniform2ui(program, "uInputResolution", m_width, m_height);
	SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
	SetUniformMat4(program, "uInvView", glm::inverse(context.view));
	SetUniform1ui(program, "uMaxSurfels", m_settings.maxSurfels);
	SetUniform1ui(program, "uTransformCount", m_lastTransformCount);
	SetUniform1ui(program, "uFrameIndex", m_frameIndex);
	SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
	SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
	SetUniform1ui(program, "uMaxGatherSurfelsPerPixel", m_settings.maxGatherSurfelsPerPixel);
	SetUniform1ui(program, "uGatherNeighborRadius", m_settings.gatherNeighborRadius);
	SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
	SetUniform3fv(program, "uGridMin", gridMin);
	SetUniform3fv(program, "uGridMax", gridMax);
	SetUniform1f(program, "uNormalRejectCos", m_settings.finalGatherNormalCos);
	SetUniform1i(program, "uUseRadialDepth", m_settings.useRadialDepth ? 1 : 0);
	SetUniform1f(program, "uRadialDepthMinVariance", std::max(m_settings.radialDepthSigmaScale, 1.0e-6f));
	SetUniform1f(program, "uFallbackStrength", 1.0f);
	SetUniform1ui(program, "uDebugView", applyDebugView);

	m_applyIndirectShader->Dispatch(DivRoundUp(m_indirectWidth, 8u), DivRoundUp(m_indirectHeight, 8u), 1u);
	m_applyIndirectShader->WaitForCompletion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	if (!fullscreenDebug && m_spatialFilterShader && m_spatialFilterShader->IsValid()) {
		const GLuint filterProgram = m_spatialFilterShader->GetProgramID();
		glUseProgram(filterProgram);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_resources.rawIndirectTexture);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetColorAttachment(0));
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, context.gbufferFBO->GetDepthTexture());
		glBindImageTexture(0, m_resources.indirectTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		SetUniform1i(filterProgram, "uRawIndirectTex", 0);
		SetUniform1i(filterProgram, "uPackedNormalRMTex", 1);
		SetUniform1i(filterProgram, "uDepthTex", 2);
		SetUniform2ui(filterProgram, "uResolution", m_indirectWidth, m_indirectHeight);
		SetUniform2ui(filterProgram, "uInputResolution", m_width, m_height);
		m_spatialFilterShader->Dispatch(DivRoundUp(m_indirectWidth, 8u), DivRoundUp(m_indirectHeight, 8u), 1u);
		m_spatialFilterShader->WaitForCompletion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
		glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	}

	glUseProgram(0);
	m_stats.ready = IsReady();
}

void SurfelGIPipeline::RenderDebug(RenderContext& context) const
{
	if (!IsReady() || m_debugProgram == 0u || m_debugVAO == 0u ||
		m_settings.debugView == SurfelGIDebugView::Off) {
		return;
	}

	BindCoreResources(0u);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, context.width, context.height);
	glEnable(GL_PROGRAM_POINT_SIZE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);

	glUseProgram(m_debugProgram);
	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = CameraPositionFromView(context.view);
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);
	const bool hasSceneDepth = context.gbufferFBO && context.gbufferFBO->GetDepthTexture() != 0u;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, hasSceneDepth ? context.gbufferFBO->GetDepthTexture() : 0u);
	SetUniformMat4(m_debugProgram, "uView", context.view);
	SetUniformMat4(m_debugProgram, "uProjection", context.proj);
	SetUniform2f(m_debugProgram, "uViewportSize", static_cast<GLfloat>(std::max(context.width, 1)), static_cast<GLfloat>(std::max(context.height, 1)));
	SetUniform1ui(m_debugProgram, "uMaxSurfels", m_settings.maxSurfels);
	SetUniform1ui(m_debugProgram, "uDebugView", static_cast<GLuint>(m_settings.debugView));
	SetUniform1ui(m_debugProgram, "uMaxRays", m_settings.maxRayBudget);
	SetUniform1i(m_debugProgram, "uSceneDepthTex", 0);
	SetUniform1i(m_debugProgram, "uUseDepthReject", hasSceneDepth ? 1 : 0);
	SetUniform1f(m_debugProgram, "uDepthRejectBias", 0.0015f);
	SetUniform3ui(m_debugProgram, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
	SetUniform3fv(m_debugProgram, "uGridMin", gridMin);
	SetUniform3fv(m_debugProgram, "uGridMax", gridMax);

	glBindVertexArray(m_debugVAO);
	glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_settings.maxSurfels));
	glBindVertexArray(0);
	glUseProgram(0);

	glDepthMask(GL_TRUE);
}

bool SurfelGIPipeline::ReloadShaders()
{
	ReleaseShaders();
	return !m_settings.enabled || LoadShaders();
}

void SurfelGIPipeline::SetSettings(const SurfelGISettings& settings)
{
	const uint32_t oldIndirectWidth = m_indirectWidth;
	const uint32_t oldIndirectHeight = m_indirectHeight;
	m_settings = settings;
	m_stats.configuredEnabled = settings.enabled;
	if (m_initialized && m_settings.enabled && m_width > 0u && m_height > 0u) {
		m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
		m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
		if (m_resources.indirectTexture != 0u &&
			(m_indirectWidth != oldIndirectWidth || m_indirectHeight != oldIndirectHeight)) {
			ReleaseIndirectTexture();
			CreateIndirectTexture();
		}
	}
}

bool SurfelGIPipeline::IsReady() const
{
	return m_initialized &&
		m_resources.surfelBuffer != 0u &&
		m_resources.freeListBuffer != 0u &&
		m_resources.countersBuffer != 0u &&
		m_resources.gridHeaderBuffer != 0u &&
		m_resources.gridEntryBuffer != 0u &&
		m_resources.gridAverageBuffer != 0u &&
		m_resources.rayRequestBuffer != 0u &&
		m_resources.rayBuffer != 0u &&
		m_resources.rayHitBuffer != 0u &&
		m_resources.rawIndirectTexture != 0u &&
		m_resources.indirectTexture != 0u;
}

bool SurfelGIPipeline::CreateAuxiliaryResources()
{
	GLsizeiptr radialBytes = 0;
	GLsizeiptr guideMapBytes = 0;
	GLsizeiptr guideScaleBytes = 0;
	if (!CheckedBytes(
			static_cast<uint64_t>(m_settings.maxSurfels) * kRadialDepthTexelsPerSurfel,
			sizeof(float) * 2u,
			radialBytes) ||
		!CheckedBytes(
			static_cast<uint64_t>(m_settings.maxSurfels) * kGuideCellsPerSurfel,
			sizeof(uint32_t),
			guideMapBytes) ||
		!CheckedBytes(m_settings.maxSurfels, sizeof(glm::vec4), guideScaleBytes)) {
		std::cerr << "[SurfelGIPipeline] Auxiliary buffer size overflow.\n";
		return false;
	}

	const bool ok =
		AllocateBuffer(m_resources.radialDepthBuffer, radialBytes, "SurfelGI.RadialDepth") &&
		AllocateBuffer(m_resources.guideMapBuffer, guideMapBytes, "SurfelGI.GuideMap") &&
		AllocateBuffer(m_resources.guideScaleBuffer, guideScaleBytes, "SurfelGI.GuideScale");

	if (!ok) {
		ReleaseAuxiliaryResources();
	}
	return ok;
}

void SurfelGIPipeline::ReleaseAuxiliaryResources()
{
	DeleteBuffer(m_resources.radialDepthBuffer);
	DeleteBuffer(m_resources.guideMapBuffer);
	DeleteBuffer(m_resources.guideScaleBuffer);
}

bool SurfelGIPipeline::LoadShaders()
{
	const bool computeOk = CreateCompute(m_beginFrameShader, "shaders/surfel_gi/begin_frame.comp") &&
		CreateCompute(m_updateShader, "shaders/surfel_gi/update_surfels.comp") &&
		CreateCompute(m_recycleShader, "shaders/surfel_gi/recycle_surfels.comp") &&
		CreateCompute(m_coverageShader, "shaders/surfel_gi/coverage.comp") &&
		CreateCompute(m_spawnShader, "shaders/surfel_gi/spawn.comp") &&
		CreateCompute(m_clearGridShader, "shaders/surfel_gi/clear_grid.comp") &&
		CreateCompute(m_buildGridShader, "shaders/surfel_gi/build_grid.comp") &&
		CreateCompute(m_cellAverageShader, "shaders/surfel_gi/cell_average.comp") &&
		CreateCompute(m_requestRaysShader, "shaders/surfel_gi/request_rays.comp") &&
		CreateCompute(m_allocateRaysShader, "shaders/surfel_gi/allocate_rays.comp") &&
		CreateCompute(m_generateRaysShader, "shaders/surfel_gi/generate_rays.comp") &&
		CreateCompute(m_traceRaysShader, "shaders/surfel_gi/trace_rays.comp") &&
		CreateCompute(m_integrateShader, "shaders/surfel_gi/integrate.comp") &&
		CreateCompute(m_radialDepthShader, "shaders/surfel_gi/radial_depth_update.comp") &&
		CreateCompute(m_applyIndirectShader, "shaders/surfel_gi/apply_indirect.comp") &&
		CreateCompute(m_spatialFilterShader, "shaders/surfel_gi/spatial_filter.comp");

	if (!computeOk) {
		return false;
	}

	m_debugProgram = CreateShaderProgram(
		"shaders/surfel_gi/debug_surfels.vert",
		"shaders/surfel_gi/debug_surfels.frag");
	if (m_debugProgram == 0u) {
		std::cerr << "[SurfelGIPipeline] Failed to compile surfel debug shader.\n";
		return false;
	}

	if (m_debugVAO == 0u) {
		glGenVertexArrays(1, &m_debugVAO);
	}
	return m_debugVAO != 0u;
}

void SurfelGIPipeline::ReleaseShaders()
{
	m_beginFrameShader.reset();
	m_updateShader.reset();
	m_recycleShader.reset();
	m_coverageShader.reset();
	m_spawnShader.reset();
	m_clearGridShader.reset();
	m_buildGridShader.reset();
	m_cellAverageShader.reset();
	m_requestRaysShader.reset();
	m_allocateRaysShader.reset();
	m_generateRaysShader.reset();
	m_traceRaysShader.reset();
	m_integrateShader.reset();
	m_radialDepthShader.reset();
	m_applyIndirectShader.reset();
	m_spatialFilterShader.reset();
	if (m_debugProgram != 0u) {
		glDeleteProgram(m_debugProgram);
		m_debugProgram = 0u;
	}
	if (m_debugVAO != 0u) {
		glDeleteVertexArrays(1, &m_debugVAO);
		m_debugVAO = 0u;
	}
}

bool SurfelGIPipeline::CreateIndirectTexture()
{
	m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
	m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
	return AllocateIndirectTexture(m_resources.rawIndirectTexture, m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.indirectTexture, m_indirectWidth, m_indirectHeight);
}

void SurfelGIPipeline::ReleaseIndirectTexture()
{
	DeleteTexture(m_resources.rawIndirectTexture);
	DeleteTexture(m_resources.indirectTexture);
}

void SurfelGIPipeline::BindCoreResources(GLuint transformBuffer) const
{
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::Surfels), m_resources.surfelBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::SurfelFreeList), m_resources.freeListBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::SurfelCounters), m_resources.countersBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GridHeaders), m_resources.gridHeaderBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GridEntries), m_resources.gridEntryBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GridAverages), m_resources.gridAverageBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::RayRequests), m_resources.rayRequestBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::Rays), m_resources.rayBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::SortedRays), m_resources.sortedRayBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::RayHits), m_resources.rayHitBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::RayBins), m_resources.rayBinBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::RayCounters), m_resources.rayCountersBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GridCounters), m_resources.gridCountersBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::RadialDepth), m_resources.radialDepthBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GuideMap), m_resources.guideMapBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::GuideScale), m_resources.guideScaleBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ToGLuint(SurfelGIBinding::Transforms), transformBuffer);
}
