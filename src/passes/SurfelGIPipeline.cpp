#include "SurfelGIPipeline.h"
#include "../PresentTarget.h"

#include "../ComputeShader.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../LightManager.h"
#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../RTSceneResources.h"
#include "../SceneGraph.h"
#include "../ScreenQuad.h"
#include "../ShaderLoader.h"
#include "../ShadowMapper.h"
#include "../Skybox.h"
#include "../TextureUnits.h"
#include "../stb_image_write.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
// Uniform sky radiance for rays that leave the scene when no environment map is bound. The
// deferred pass shades without any sky IBL when there is no skybox, so the surfel cache must
// not invent one either (envColor is the clear color, not a light source).
glm::vec3 SurfelSkyRadiance(const RenderContext& context, const std::shared_ptr<Skybox>& skybox, const SurfelGISettings& settings) {
	if (!skybox || !skybox->IsReady()) {
		return glm::vec3(0.0f);
	}
	return context.envColor * settings.skyMissRadianceMultiplier;
}

constexpr uint32_t kRadialDepthTexelsPerSurfel = 16u;
constexpr uint32_t kGuideCellsPerSurfel = 36u;
constexpr uint32_t kGpuTimingReadbackFrameDelay = 3u;
constexpr std::size_t kMaxPendingGpuTimingQueries = 256u;
constexpr uint32_t kSurfelStatsReadbackInterval = 30u;
constexpr uint32_t kSurfelGIGBufferNormalRMUnit = 0u;
constexpr uint32_t kSurfelGIGBufferAlbedoAOUnit = 1u;
constexpr uint32_t kSurfelGIGBufferMaterialIDUnit = 2u;
constexpr uint32_t kSurfelGIGBufferEmissiveUnit = 3u;
constexpr uint32_t kSurfelGIGBufferTransformIDUnit = 4u;
constexpr uint32_t kSurfelGIGBufferDepthUnit = 5u;
constexpr uint32_t kSurfelGIShadowArrayUnit = static_cast<uint32_t>(TextureUnits::SHADOW_MAP_ARRAY);
constexpr uint32_t kSurfelGISkyIrradianceUnit = static_cast<uint32_t>(TextureUnits::IRRADIANCE_MAP);
constexpr uint32_t kSurfelGISkyEnvironmentUnit = static_cast<uint32_t>(TextureUnits::ENVIRONMENT_MAP);
// Gather coverage read by the spawn pass (no surfel pass binds this unit otherwise).
constexpr uint32_t kSurfelGICoverageUnit = 6u;
constexpr float kSurfelGISkyDiffuseMaxScale = 0.25f;

bool EnvFlagEnabled(const char* name, bool fallback)
{
#if defined(_MSC_VER)
	char* rawValue = nullptr;
	std::size_t length = 0;
	if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr || length == 0) {
		return fallback;
	}
	const std::string value(rawValue);
	std::free(rawValue);
#else
	const char* rawValue = std::getenv(name);
	if (rawValue == nullptr || rawValue[0] == '\0') {
		return fallback;
	}
	const std::string value(rawValue);
#endif

	if (value[0] == '0' ||
		value[0] == 'f' || value[0] == 'F' ||
		value[0] == 'n' || value[0] == 'N') {
		return false;
	}
	return true;
}

uint32_t EnvUInt(const char* name, uint32_t fallback)
{
#if defined(_MSC_VER)
	char* rawValue = nullptr;
	std::size_t length = 0;
	if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr || length == 0) {
		return fallback;
	}
	const std::string value(rawValue);
	std::free(rawValue);
#else
	const char* rawValue = std::getenv(name);
	if (rawValue == nullptr || rawValue[0] == '\0') {
		return fallback;
	}
	const std::string value(rawValue);
#endif

	char* end = nullptr;
	const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
	if (end == value.c_str()) {
		return fallback;
	}
	return static_cast<uint32_t>(std::max<unsigned long>(parsed, 1ul));
}

// Saves the raster, depth and blend state that the debug draws change and restores the same fields on
// exit (the old GL guard did not restore framebuffer, viewport or program, and neither does this one).
class SurfelRenderStateGuard {
public:
	SurfelRenderStateGuard() : m_state(rhi::Gpu().CaptureState()) {}
	~SurfelRenderStateGuard()
	{
		rhi::Device& gpu = rhi::Gpu();
		gpu.SetDepthTest(m_state.depthTest);
		gpu.SetCullEnabled(m_state.cullEnabled);
		gpu.SetBlendEnabled(m_state.blend.enabled);
		gpu.SetScissorEnabled(m_state.scissorTest);
		gpu.SetDepthFunc(m_state.depthFunc);
		gpu.SetDepthWrite(m_state.depthWrite);
		gpu.SetCullFace(m_state.cullFace);
		gpu.SetFrontFace(m_state.frontFace);
		gpu.SetBlendFuncSeparate(m_state.blend.srcColor, m_state.blend.dstColor, m_state.blend.srcAlpha, m_state.blend.dstAlpha);
		gpu.SetBlendEquationSeparate(m_state.blend.colorOp, m_state.blend.alphaOp);
	}

	SurfelRenderStateGuard(const SurfelRenderStateGuard&) = delete;
	SurfelRenderStateGuard& operator=(const SurfelRenderStateGuard&) = delete;

private:
	rhi::StateSnapshot m_state;
};


void DeleteBuffer(rhi::BufferHandle& buffer)
{
	if (buffer.IsValid()) {
		rhi::Gpu().DestroyBuffer(buffer);
		buffer = {};
	}
}

void DeleteTexture(rhi::TextureHandle& texture)
{
	if (texture.IsValid()) {
		rhi::Gpu().DestroyTexture(texture);
		texture = {};
	}
}

bool AllocateBuffer(rhi::BufferHandle& buffer, std::ptrdiff_t sizeBytes, const char* label)
{
	if (sizeBytes <= 0) {
		return false;
	}

	rhi::Device& gpu = rhi::Gpu();
	if (!buffer.IsValid()) {
		rhi::BufferDesc desc{};
		desc.size = static_cast<size_t>(sizeBytes);
		desc.usage = rhi::BufferUsage::DynamicDraw;
		buffer = gpu.CreateBuffer(desc);
		if (!buffer.IsValid()) {
			return false;
		}
	}
	else {
		gpu.ReallocateBuffer(buffer, static_cast<size_t>(sizeBytes), nullptr, rhi::BufferUsage::DynamicDraw);
	}
	const char* error = gpu.PollError();

	if (error != nullptr) {
		std::cerr << "[SurfelGIPipeline] Failed to allocate " << label
			<< " (" << sizeBytes << " bytes, " << error << ").\n";
		DeleteBuffer(buffer);
		return false;
	}

	gpu.SetDebugName(buffer, label);
	return true;
}

void ClearBufferUInt(rhi::BufferHandle buffer, uint32_t value)
{
	if (!buffer.IsValid()) {
		return;
	}

	rhi::Gpu().ClearBuffer(buffer, rhi::Format::R32UI, rhi::PixelFormat::RInt, rhi::PixelType::UInt, &value);
}

void ClearBufferVec4(rhi::BufferHandle buffer, const glm::vec4& value)
{
	if (!buffer.IsValid()) {
		return;
	}

	rhi::Gpu().ClearBuffer(buffer, rhi::Format::RGBA32F, rhi::PixelFormat::RGBA, rhi::PixelType::Float, glm::value_ptr(value));
}

void StoreSurfelCounters(const SurfelCounters& counters, SurfelGIFrameStats& stats);

// Synchronous read: stalls on every pending write to the counters buffer. Used only by the
// placement-validation tool, which needs this frame's counters before it reports.
void ReadSurfelCounters(rhi::BufferHandle countersBuffer, SurfelGIFrameStats& stats)
{
	if (!countersBuffer.IsValid()) {
		return;
	}

	SurfelCounters counters{};
	rhi::Gpu().ReadBuffer(countersBuffer, 0, sizeof(SurfelCounters), &counters);
	StoreSurfelCounters(counters, stats);
}

// Largest spawn budget a frame can use, as spawn.comp computes it: the floor, or visible tiles / divisor bounded by
// the ceiling. Visible tiles are at most the full tile count, so the full count gives an upper bound. The surfel-wide
// dispatch slack is this value, so every spawn of the frame is inside the bound and gets a grid entry this frame.
uint32_t SpawnBudgetCeiling(uint32_t tileCount, const SurfelGISettings& settings)
{
	const float divisor = std::max(settings.spawnBudgetTileDivisor, 1.0f);
	const float scaled = std::min(static_cast<float>(tileCount) / divisor,
		static_cast<float>(settings.maxSpawnBudgetPerFrame));
	return std::max(settings.maxSpawnsPerFrame, static_cast<uint32_t>(scaled));
}

// Surfel-wide dispatch sized by the indirect args written by dispatch_args.comp; falls back to the
// full range when the args pass is unavailable.
void DispatchSurfelWide(ComputeShader& shader, rhi::BufferHandle argsBuffer, uint32_t fullGroups)
{
	if (argsBuffer.IsValid()) {
		shader.DispatchIndirect(argsBuffer, 0u);
	}
	else {
		shader.Dispatch(fullGroups, 1u, 1u);
	}
}

void StoreSurfelCounters(const SurfelCounters& counters, SurfelGIFrameStats& stats)
{
	stats.liveSurfels = counters.liveCount;
	stats.spawnedThisFrame = counters.spawnedThisFrame;
	stats.recycledThisFrame = counters.recycledThisFrame;
	stats.requestedRays = counters.requestedRays;
	stats.allocatedRays = counters.allocatedRays;
	stats.overflowSurfels = counters.overflowSurfels;
	stats.overflowGridEntries = counters.overflowGridEntries;
	stats.rejectedInvalidDepth = counters.rejectedInvalidDepth;
	stats.rejectedInvalidTransform = counters.rejectedInvalidTransform;
	stats.rejectedInvalidMaterial = counters.rejectedInvalidMaterial;
	stats.rejectedOutsideGrid = counters.rejectedOutsideGrid;
	stats.rejectedInvalidWorldPos = counters.rejectedInvalidWorldPos;
	stats.rejectedInvalidNormal = counters.rejectedInvalidNormal;
	stats.rejectedInvalidRadius = counters.rejectedInvalidRadius;
	stats.rejectedPoolFull = counters.rejectedPoolFull;
	stats.overCoverageRecycled = counters.overCoverageRecycled;
	stats.staleRecycled = counters.staleRecycled;
	stats.pressureRecycled = counters.pressureRecycled;
	stats.underCoveredTileCount = counters.underCoveredTileCount;
	stats.highPriorityTileCount = counters.highPriorityTileCount;
	stats.coverageSpawnedTileCount = counters.coverageSpawnedTileCount;
	stats.coverageVisibleTileCount = counters.coverageVisibleTileCount;
	stats.coverageInvalidTileCount = counters.coverageInvalidTileCount;
	stats.projectedSurfels = counters.projectedSurfels;
	stats.coverageDepthRejected = counters.coverageDepthRejected;
	stats.coverageNormalRejected = counters.coverageNormalRejected;
	stats.coverageMaterialRejected = counters.coverageMaterialRejected;
	stats.coverageRadiusRejected = counters.coverageRadiusRejected;
	stats.rayHitGrid = counters.rayHitGrid;
	stats.rayHitScreen = counters.rayHitScreen;
	stats.rayHitBVH = counters.rayHitBVH;
	stats.rayHitEnvironment = counters.rayHitEnvironment;
	stats.rayHitMiss = counters.rayHitMiss;
	stats.acceptedRaySamples = counters.acceptedRaySamples;
	stats.temporalClampEvents = counters.temporalClampEvents;
	stats.sharingAppliedSurfels = counters.sharingAppliedSurfels;
	stats.sharingRejectedDarkLift = counters.sharingRejectedDarkLift;
	stats.finalGatherDirectPixels = counters.finalGatherDirectPixels;
	stats.finalGatherFallbackPixels = counters.finalGatherFallbackPixels;
	stats.finalGatherBleedPixels = counters.finalGatherBleedPixels;
	stats.finalGatherValidPixels = counters.finalGatherValidPixels;
	stats.finalGatherEmptyPixels = counters.finalGatherEmptyPixels;
	stats.youngSurfels = counters.youngSurfelCount;
	stats.staleRaySlots = counters.staleRaySlotCount;
	stats.lookupsSharing = counters.lookupsSharing;
	stats.entriesSharing = counters.entriesSharing;
	stats.lookupsGather = counters.lookupsGather;
	stats.entriesGather = counters.entriesGather;
	stats.lookupsTrace = counters.lookupsTrace;
	stats.entriesTrace = counters.entriesTrace;
	stats.lookupsSpawn = counters.lookupsSpawn;
	stats.entriesSpawn = counters.entriesSpawn;
	stats.recycleOverTarget = counters.recycleOverTarget;
	stats.recycleStale = counters.recycleStale;
	stats.recycleFar = counters.recycleFar;
	stats.recycleForced = counters.recycleForced;
	stats.spawnDecisions = counters.spawnDecisions;
	stats.spawnBudgetDenied = counters.spawnBudgetDenied;
	stats.radiusCapped = counters.radiusCapped;
	stats.recycledYoung = counters.recycledYoung;
	// Rays asked for by request_rays but not granted by allocate_rays (both counters are per frame).
	stats.rayShortfall = counters.requestedRays > counters.allocatedRays
		? counters.requestedRays - counters.allocatedRays
		: 0u;
}

// Reference mode accumulates at quarter of the output resolution.
uint32_t ReferenceExtent(uint32_t fullExtent)
{
	return std::max((fullExtent + 3u) / 4u, 1u);
}

bool AllocateReferenceTextures(rhi::TextureHandle& mean, rhi::TextureHandle& count, uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u) {
		return false;
	}

	rhi::Device& gpu = rhi::Gpu();
	rhi::TextureDesc desc{};
	desc.width = static_cast<int>(width);
	desc.height = static_cast<int>(height);
	if (!mean.IsValid()) {
		desc.format = rhi::Format::RGBA32F;
		mean = gpu.CreateTexture(desc);
		gpu.SetTextureMinFilter(mean, rhi::Filter::Linear);
		gpu.SetTextureMagFilter(mean, rhi::Filter::Linear);
	}
	if (!count.IsValid()) {
		desc.format = rhi::Format::R32F;
		count = gpu.CreateTexture(desc);
	}
	return mean.IsValid() && count.IsValid();
}

bool AllocateIndirectTexture(rhi::TextureHandle& texture, uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u) {
		return false;
	}

	rhi::Device& gpu = rhi::Gpu();
	const bool existed = texture.IsValid();
	if (!existed) {
		rhi::TextureDesc desc{};
		desc.format = rhi::Format::RGBA16F;
		desc.width = static_cast<int>(width);
		desc.height = static_cast<int>(height);
		texture = gpu.CreateTexture(desc);
	}
	if (!texture.IsValid()) {
		return false;
	}

	gpu.SetTextureMinFilter(texture, rhi::Filter::Linear);
	gpu.SetTextureMagFilter(texture, rhi::Filter::Linear);
	gpu.SetTextureWrapAxis(texture, 0, rhi::Wrap::ClampToEdge);
	gpu.SetTextureWrapAxis(texture, 1, rhi::Wrap::ClampToEdge);
	if (existed) {
		gpu.ResizeTexture(texture, static_cast<int>(width), static_cast<int>(height), 1, 1);
	}
	const char* error = gpu.PollError();

	if (error != nullptr) {
		std::cerr << "[SurfelGIPipeline] Failed to allocate indirect texture ("
			<< width << "x" << height << ", " << error << ").\n";
		DeleteTexture(texture);
		return false;
	}

	gpu.SetDebugName(texture, "SurfelGI.IndirectDiffuse");
	return true;
}

bool CheckedBytes(uint64_t elementCount, uint64_t elementSize, std::ptrdiff_t& outBytes)
{
	if (elementCount == 0u || elementSize == 0u) {
		return false;
	}
	const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
	if (elementCount > maxSize / elementSize) {
		return false;
	}
	outBytes = static_cast<std::ptrdiff_t>(elementCount * elementSize);
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
		view == SurfelGIDebugView::FinalGatherFallback ||
		view == SurfelGIDebugView::FinalGatherBleed ||
		view == SurfelGIDebugView::FinalGatherComposite ||
		view == SurfelGIDebugView::GBufferDepth ||
		view == SurfelGIDebugView::GBufferWorldPosition ||
		view == SurfelGIDebugView::GBufferNormal ||
		view == SurfelGIDebugView::GBufferTransformID ||
		view == SurfelGIDebugView::GBufferMaterialID ||
		view == SurfelGIDebugView::SpawnCandidates ||
		view == SurfelGIDebugView::RawIndirectIrradiance;
}

glm::vec3 CameraPositionFromView(const glm::mat4& view)
{
	const glm::mat4 invView = glm::inverse(view);
	return glm::vec3(invView[3]);
}

glm::vec3 NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback)
{
	const float lenSq = glm::dot(value, value);
	if (lenSq <= 1.0e-8f) {
		return fallback;
	}
	return value * (1.0f / std::sqrt(lenSq));
}

glm::vec3 LogCompressedRadiance(const glm::vec3& radiance)
{
	const glm::vec3 safe(
		std::max(radiance.x, 0.0f),
		std::max(radiance.y, 0.0f),
		std::max(radiance.z, 0.0f));
	return glm::vec3(
		std::log1p(safe.x),
		std::log1p(safe.y),
		std::log1p(safe.z));
}

float SmoothStepFloat(float edge0, float edge1, float value)
{
	const float t = std::clamp((value - edge0) / std::max(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void AppendLightingSignature(std::vector<glm::vec4>& signature, const std::shared_ptr<BaseLight>& light)
{
	if (!light || !light->IsEnabled()) {
		return;
	}

	const glm::vec3 position = light->GetPosition() * 0.16f;
	const glm::vec3 direction = NormalizeOrFallback(light->GetDirection(), glm::vec3(0.0f, -1.0f, 0.0f));
	const glm::vec3 radiance = LogCompressedRadiance(light->GetColor() * std::max(light->GetIntensity(), 0.0f));
	const glm::vec3 attenuation = light->GetAttenuation() * 0.15f;
	signature.emplace_back(position, static_cast<float>(light->GetLightType()));
	signature.emplace_back(direction, light->GetRange() * 0.02f);
	signature.emplace_back(radiance, light->CastsShadows() ? 1.0f : 0.0f);
	signature.emplace_back(attenuation, light->IsEnabled() ? 1.0f : 0.0f);
}

std::shared_ptr<BaseLight> FindPrimaryDirectionalLight(const RenderContext& context)
{
	if (!context.lightManager) {
		return nullptr;
	}

	std::shared_ptr<BaseLight> primary;
	float bestIntensity = -1.0f;
	const std::vector<std::shared_ptr<BaseLight>> enabledLights = context.lightManager->GetEnabledLights();
	for (const std::shared_ptr<BaseLight>& light : enabledLights) {
		if (!light ||
			!light->IsEnabled() ||
			light->GetLightType() != BaseLight::LightType::DIRECTIONAL) {
			continue;
		}

		const float intensity = std::max(light->GetIntensity(), 0.0f);
		if (!primary || intensity > bestIntensity) {
			primary = light;
			bestIntensity = intensity;
		}
	}
	return primary;
}

std::vector<glm::vec4> BuildLightingSignature(const RenderContext& context,
	const std::shared_ptr<DirectionalLight>& fallbackDirectionalLight)
{
	std::vector<glm::vec4> signature;
	signature.reserve(128u);

	if (context.lightManager) {
		const std::vector<std::shared_ptr<BaseLight>> enabledLights = context.lightManager->GetEnabledLights();
		const std::size_t maxLights = std::min<std::size_t>(enabledLights.size(), 32u);
		for (std::size_t i = 0; i < maxLights; ++i) {
			AppendLightingSignature(signature, enabledLights[i]);
		}
	}

	if (signature.empty() && fallbackDirectionalLight) {
		AppendLightingSignature(signature, fallbackDirectionalLight);
	}

	return signature;
}

float ComputeLightingChangeFactor(const std::vector<glm::vec4>& currentSignature,
	const std::vector<glm::vec4>& previousSignature)
{
	if (currentSignature.empty() && previousSignature.empty()) {
		return 0.0f;
	}
	if (previousSignature.empty()) {
		return 0.0f;
	}
	if (currentSignature.size() != previousSignature.size()) {
		return 1.0f;
	}

	float maxDelta = 0.0f;
	float averageDelta = 0.0f;
	for (std::size_t i = 0; i < currentSignature.size(); ++i) {
		const float delta = glm::length(currentSignature[i] - previousSignature[i]);
		maxDelta = std::max(maxDelta, delta);
		averageDelta += delta;
	}
	averageDelta /= std::max(static_cast<float>(currentSignature.size()), 1.0f);

	const float peakResponse = SmoothStepFloat(0.015f, 0.18f, maxDelta);
	const float broadResponse = SmoothStepFloat(0.006f, 0.060f, averageDelta);
	return std::clamp(std::max(peakResponse, broadResponse), 0.0f, 1.0f);
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

void SetUniform1ui(rhi::ProgramHandle program, const char* name, uint32_t value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

void SetUniform2ui(rhi::ProgramHandle program, const char* name, uint32_t x, uint32_t y)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, glm::uvec2(x, y));
	}
}

void SetUniform3ui(rhi::ProgramHandle program, const char* name, uint32_t x, uint32_t y, uint32_t z)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, glm::uvec3(x, y, z));
	}
}

void SetUniform1i(rhi::ProgramHandle program, const char* name, int value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

void SetUniform1f(rhi::ProgramHandle program, const char* name, float value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

void SetUniform2f(rhi::ProgramHandle program, const char* name, float x, float y)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, glm::vec2(x, y));
	}
}

void SetUniform3fv(rhi::ProgramHandle program, const char* name, const glm::vec3& value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

std::string EnvString(const char* name)
{
#if defined(_MSC_VER)
	char* rawValue = nullptr;
	std::size_t length = 0;
	if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr || length == 0) {
		return {};
	}
	std::string value(rawValue);
	std::free(rawValue);
	return value;
#else
	const char* rawValue = std::getenv(name);
	return rawValue ? std::string(rawValue) : std::string{};
#endif
}

bool EnvFrameListContains(const std::string& frames, uint32_t frameIndex)
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
		const unsigned long parsed = std::strtoul(cursor, &end, 10);
		if (end == cursor) {
			++cursor;
			continue;
		}
		if (parsed == static_cast<unsigned long>(frameIndex)) {
			return true;
		}
		cursor = end;
	}
	return false;
}

glm::vec3 DebugTonemapSurfelIrradiance(const glm::vec3& value)
{
	const glm::vec3 safe(std::max(value.r, 0.0f), std::max(value.g, 0.0f), std::max(value.b, 0.0f));
	const glm::vec3 mapped = safe / (safe + glm::vec3(1.0f));
	return glm::pow(glm::clamp(mapped, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(1.0f / 2.2f));
}

bool SaveSurfelDebugTexturePNG(rhi::TextureHandle texture, uint32_t width, uint32_t height, const std::filesystem::path& path)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!texture.IsValid() || width == 0u || height == 0u) {
		return false;
	}

	std::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0.0f);
	gpu.ReadTexture(texture, 0, rhi::PixelFormat::RGBA, rhi::PixelType::Float, pixels.size() * sizeof(float), pixels.data());

	std::vector<unsigned char> png(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			const std::size_t src = (static_cast<std::size_t>(y) * width + x) * 4u;
			const std::size_t dst = (static_cast<std::size_t>(height - 1u - y) * width + x) * 4u;
			const glm::vec3 mapped = DebugTonemapSurfelIrradiance(glm::vec3(pixels[src], pixels[src + 1u], pixels[src + 2u]));
			png[dst] = static_cast<unsigned char>(glm::clamp(mapped.r, 0.0f, 1.0f) * 255.0f);
			png[dst + 1u] = static_cast<unsigned char>(glm::clamp(mapped.g, 0.0f, 1.0f) * 255.0f);
			png[dst + 2u] = static_cast<unsigned char>(glm::clamp(mapped.b, 0.0f, 1.0f) * 255.0f);
			png[dst + 3u] = static_cast<unsigned char>(glm::clamp(pixels[src + 3u], 0.0f, 1.0f) * 255.0f);
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	return stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, png.data(), static_cast<int>(width * 4u)) != 0;
}

void SetUniform4fv(rhi::ProgramHandle program, const char* name, const glm::vec4& value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

void SetUniformMat4(rhi::ProgramHandle program, const char* name, const glm::mat4& value)
{
	rhi::Device& gpu = rhi::Gpu();
	const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
	if (loc >= 0) {
		gpu.SetUniform(loc, value);
	}
}

void SetSurfelGridUniforms(rhi::ProgramHandle program,
	const SurfelGridSettings& gridSettings,
	const glm::vec3& gridMin,
	const glm::vec3& gridMax)
{
	SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
	SetUniform3fv(program, "uGridMin", gridMin);
	SetUniform3fv(program, "uGridMax", gridMax);
	SetUniform1ui(program, "uUseNonLinearGrid", gridSettings.useNonLinearGrid ? 1u : 0u);
	SetUniform1f(program, "uGridFarExtent", gridSettings.useNonLinearGrid ? gridSettings.farScale : glm::length(gridSettings.worldExtent));
	SetUniform1f(program, "uSurfelGridSliceGrowth", gridSettings.sliceGrowth);
	SetUniform1f(program, "uSurfelGridReachScale", gridSettings.reachCellScale);
}

void BindSurfelGIGBufferTextures(const RenderContext& context)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!context.gbufferFBO) {
		return;
	}

	gpu.BindTexture(kSurfelGIGBufferNormalRMUnit, context.gbufferFBO->GetColorAttachment(0));
	gpu.BindTexture(kSurfelGIGBufferAlbedoAOUnit, context.gbufferFBO->GetColorAttachment(1));
	gpu.BindTexture(kSurfelGIGBufferMaterialIDUnit, context.gbufferFBO->GetColorAttachment(3));
	gpu.BindTexture(kSurfelGIGBufferEmissiveUnit, context.gbufferFBO->GetColorAttachment(4));
	gpu.BindTexture(kSurfelGIGBufferTransformIDUnit, context.gbufferFBO->GetColorAttachment(5));
	gpu.BindTexture(kSurfelGIGBufferDepthUnit, context.gbufferFBO->GetDepthTexture());
}

void SetSurfelGIGBufferSamplerUniforms(rhi::ProgramHandle program)
{
	SetUniform1i(program, "uPackedNormalRMTex", static_cast<int>(kSurfelGIGBufferNormalRMUnit));
	SetUniform1i(program, "uAlbedoAOTex", static_cast<int>(kSurfelGIGBufferAlbedoAOUnit));
	SetUniform1i(program, "uMaterialIDTex", static_cast<int>(kSurfelGIGBufferMaterialIDUnit));
	SetUniform1i(program, "uEmissiveTex", static_cast<int>(kSurfelGIGBufferEmissiveUnit));
	SetUniform1i(program, "uTransformIDTex", static_cast<int>(kSurfelGIGBufferTransformIDUnit));
	SetUniform1i(program, "uDepthTex", static_cast<int>(kSurfelGIGBufferDepthUnit));
}

void LogSurfelGIGBufferBindings(const RenderContext& context)
{
	if (!context.gbufferFBO) {
		return;
	}

	std::cout << "[SurfelGIPipeline] G-buffer bindings:"
		<< " normalRM(unit " << kSurfelGIGBufferNormalRMUnit << ")=" << context.gbufferFBO->GetColorAttachment(0)
		<< " albedoAO(unit " << kSurfelGIGBufferAlbedoAOUnit << ")=" << context.gbufferFBO->GetColorAttachment(1)
		<< " materialID(unit " << kSurfelGIGBufferMaterialIDUnit << ")=" << context.gbufferFBO->GetColorAttachment(3)
		<< " emissive(unit " << kSurfelGIGBufferEmissiveUnit << ")=" << context.gbufferFBO->GetColorAttachment(4)
		<< " transformID(unit " << kSurfelGIGBufferTransformIDUnit << ")=" << context.gbufferFBO->GetColorAttachment(5)
		<< " depth(unit " << kSurfelGIGBufferDepthUnit << ")=" << context.gbufferFBO->GetDepthTexture()
		<< "\n";
}

uint32_t DivRoundUp(uint32_t value, uint32_t divisor)
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

}

SurfelGIPipeline::~SurfelGIPipeline()
{
	Shutdown();
}

// Copies the counters into a staging buffer on the GPU. Nothing waits on the CPU here; the copy is
// mapped by PumpCounterReadback once it has had kGpuTimingReadbackFrameDelay frames to finish.
void SurfelGIPipeline::IssueCounterReadback()
{
	rhi::Device& gpu = rhi::Gpu();
	if (m_counterReadbackPending || !m_resources.countersBuffer.IsValid()) {
		return;
	}
	const uint32_t slot = m_counterReadbackSlot;
	const rhi::BufferHandle staging = m_resources.counterStaging[slot];
	if (!staging.IsValid()) {
		return;
	}
	// The copy reads counters written by shader atomics earlier in this frame.
	gpu.PipelineBarrier(rhi::BarrierStorage | rhi::BarrierBufferUpdate);
	gpu.CopyBuffer(m_resources.countersBuffer, 0, staging, 0, sizeof(SurfelCounters));
	m_counterReadbackPending = true;
	m_counterReadbackFrame = m_frameIndex;
	m_counterReadbackSlot = (slot + 1u) % 2u;
}

void SurfelGIPipeline::PumpCounterReadback()
{
	if (!m_counterReadbackPending) {
		return;
	}
	if (m_frameIndex - m_counterReadbackFrame < kGpuTimingReadbackFrameDelay) {
		return;
	}
	// Ring slot used for the request that was issued kGpuTimingReadbackFrameDelay frames ago.
	const uint32_t readSlot = (m_counterReadbackSlot + 1u) % 2u;
	const rhi::BufferHandle staging = m_resources.counterStaging[readSlot];
	rhi::Device& gpu = rhi::Gpu();
	if (staging.IsValid()) {
		const void* mapped = gpu.MapBuffer(staging, 0, sizeof(SurfelCounters), rhi::MapAccess::Read);
		if (mapped) {
			SurfelCounters counters{};
			std::memcpy(&counters, mapped, sizeof(SurfelCounters));
			gpu.UnmapBuffer(staging);
			StoreSurfelCounters(counters, m_stats);
		}
	}
	m_counterReadbackPending = false;
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
	m_historyReadIndex = 0;
	m_hasLastCameraState = false;
	m_hasTemporalHistory = false;
	m_initialized = true;

	if (!m_settings.enabled) {
		return true;
	}

	SurfelGridSettings gridSettings{};
	gridSettings.maxSurfelsPerCell = m_settings.maxSurfelsPerCell;
	gridSettings.reachCellScale = m_settings.gridReachCellScale;
	gridSettings.maxSurfels = m_settings.maxSurfels;
	gridSettings.entryCapacity = m_settings.maxSurfels * kSurfelGridMaxCellsPerSurfel;
	gridSettings.useNonLinearGrid = m_settings.useNonLinearGrid;
	gridSettings.centralResolution = 32u;
	gridSettings.axisLateralResolution = 32u;
	gridSettings.axisSliceCount = 24u;
	gridSettings.resolution = glm::uvec3(
		gridSettings.centralResolution,
		gridSettings.axisLateralResolution,
		gridSettings.axisSliceCount);
	gridSettings.centralHalfExtent = 12.0f;
	gridSettings.worldExtent = glm::vec3(gridSettings.centralHalfExtent * 2.0f);
	gridSettings.farScale = 140.0f;

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
	m_resources.gridCellSumBuffer = m_grid.GetCellSumBuffer();
	m_resources.gridCountersBuffer = m_grid.GetCountersBuffer();
	m_resources.rayRequestBuffer = m_rayQueue.GetRequestBuffer();
	m_resources.rayBuffer = m_rayQueue.GetRayBuffer();
	m_resources.rayHitBuffer = m_rayQueue.GetRayHitBuffer();
	m_resources.rayAllocScratchBuffer = m_rayQueue.GetAllocScratchBuffer();
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
	m_historyReadIndex = 0;
	m_lastTextureDumpFrame = 0;
	m_hasTemporalHistory = false;
		ReleaseAuxiliaryResources();
		CreateAuxiliaryResources();
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
	m_lastSpawnPassCount = 1;
	m_subpassMetricsInterval = 120;
	m_historyReadIndex = 0;
	m_lastTextureDumpFrame = 0;
	m_hasLastCameraState = false;
	m_hasTemporalHistory = false;
	m_loggedGBufferBindings = false;
	m_initialized = false;
	m_logSubpassMetrics = false;
	ReleaseGpuTimingQueries();
	m_subpassTimings.clear();
}

void SurfelGIPipeline::BeginFrame(RenderContext&,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&)
{
	m_logSubpassMetrics = EnvFlagEnabled("NOX_SURFEL_GI_LOG_SUBPASS_METRICS", false);
	m_subpassMetricsInterval = EnvUInt("NOX_SURFEL_GI_LOG_SUBPASS_METRICS_INTERVAL", 120u);
	m_subpassTimings.clear();
	CollectReadyGpuTimings(false);
	m_stats.configuredEnabled = m_settings.enabled;
	m_stats.ready = IsReady();
}

void SurfelGIPipeline::Execute(RenderContext& context,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!IsReady() || !context.gbufferFBO) {
		m_stats.ready = false;
		return;
	}

	// Reference mode owns the frame: no surfel pass runs, the indirect texture comes from the path tracer.
	if (m_settings.referenceMode) {
		ExecuteIndependentReference(context, sceneGraph, camera, dirLight, skybox);
		m_stats.ready = IsReady();
		return;
	}

	const uint32_t maxSurfels = m_settings.maxSurfels;
	const uint32_t groupsSurfels = DivRoundUp(maxSurfels, 64u);
	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = camera ? camera->GetCameraPosition() : CameraPositionFromView(context.view);
	const glm::vec3 fallbackDirection = glm::vec3(-0.35f, -1.0f, -0.25f);
	const std::shared_ptr<BaseLight> primaryDirectionalLight = FindPrimaryDirectionalLight(context);
	const glm::vec3 lightDirection = primaryDirectionalLight
		? NormalizeOrFallback(primaryDirectionalLight->GetDirection(), fallbackDirection)
		: (dirLight ? NormalizeOrFallback(dirLight->GetLightDirection(), fallbackDirection) : fallbackDirection);
	const glm::vec3 lightRadiance = primaryDirectionalLight
		? primaryDirectionalLight->GetColor() * std::max(primaryDirectionalLight->GetIntensity(), 0.0f)
		: glm::vec3(0.0f);  // Only a registered light feeds the GI. The default DirectionalLight object is not a light in the scene.
	const std::vector<glm::vec4> currentLightingSignature = BuildLightingSignature(context, dirLight);
	const float detectedLightingChange = ComputeLightingChangeFactor(currentLightingSignature, m_lastLightingSignature);
	if (detectedLightingChange > 0.01f) {
		m_lightingChangeResponse = std::max(m_lightingChangeResponse, detectedLightingChange);
		const uint32_t holdFrames = static_cast<uint32_t>(std::ceil(6.0f + detectedLightingChange * 18.0f));
		m_lightingChangeHoldFrames = std::max(m_lightingChangeHoldFrames, holdFrames);
	}
	else if (m_lightingChangeHoldFrames > 0u) {
		--m_lightingChangeHoldFrames;
		m_lightingChangeResponse *= 0.86f;
	}
	else {
		m_lightingChangeResponse *= 0.50f;
	}
	m_lightingChangeFactor = std::clamp(std::max(detectedLightingChange, m_lightingChangeResponse), 0.0f, 1.0f);
	if (m_lightingChangeFactor < 0.005f) {
		m_lightingChangeFactor = 0.0f;
	}
	m_lastLightingSignature = currentLightingSignature;
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);
	const bool placementOnly = m_settings.placementValidationMode;
	auto timedDispatch = [&](const char* label, auto&& work) {
		rhi::QueryHandle beginQuery;
		rhi::QueryHandle endQuery;
		BeginGpuTiming(label, beginQuery, endQuery);
		work();
		EndGpuTiming(label, beginQuery, endQuery);
	};

	rhi::BufferHandle transformBuffer;
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
	if (!m_loggedGBufferBindings) {
		LogSurfelGIGBufferBindings(context);
		m_loggedGBufferBindings = true;
	}

	bool cameraMoving = false;
	if (m_hasLastCameraState) {
		const float positionDelta = glm::length(cameraPosition - m_lastCameraPosition);
		const float viewDelta = MatrixMaxAbsDelta(context.view, m_lastView);
		cameraMoving = positionDelta > m_settings.stationaryCameraEpsilon ||
			viewDelta > m_settings.stationaryCameraEpsilon;
		if (!cameraMoving) {
			m_stationaryFrameCount = std::min(m_stationaryFrameCount + 1u, 1024u);
		} else {
			m_stationaryFrameCount = 0u;
		}
	}
	m_lastCameraPosition = cameraPosition;
	m_lastView = context.view;
	m_hasLastCameraState = true;

	auto countLiveSurfels = [&]() {
		if (m_countLiveShader && m_countLiveShader->IsValid()) {
			const rhi::ProgramHandle program = m_countLiveShader->GetProgramID();
			gpu.UseProgram(program);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			timedDispatch("debug_overhead", [&]() {
				m_countLiveShader->Dispatch(groupsSurfels, 1u, 1u);
				m_countLiveShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}
	};

	if (m_beginFrameShader && m_beginFrameShader->IsValid()) {
		gpu.UseProgram(m_beginFrameShader->GetProgramID());
		// requestedRays starts at zero: it is the sum of the surfels' requests (request_rays.comp), which
		// allocate_rays.comp shares out. No reservation is added, so the requested count is not inflated.
		SetUniform1ui(m_beginFrameShader->GetProgramID(), "uRequestReservation", 0u);
		timedDispatch("begin_frame", [&]() {
			m_beginFrameShader->Dispatch(1u, 1u, 1u);
			m_beginFrameShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	// Indirect args for the surfel-wide passes below: groups = ceil(bound / 64) with bound = the
	// high-water index from last frame's count plus the spawn slack. Written to a dedicated buffer
	// and made visible to command reads before the consumers run.
	rhi::BufferHandle surfelArgs{};
	if (m_dispatchArgsShader && m_dispatchArgsShader->IsValid() && m_resources.dispatchArgsBuffer.IsValid()) {
		const uint32_t tileCountForSlack = std::max(m_coverageTileCountX * m_coverageTileCountY, 1u);
		const uint32_t spawnSlack = std::max(SpawnBudgetCeiling(tileCountForSlack, m_settings), 16u);
		const rhi::ProgramHandle argsProgram = m_dispatchArgsShader->GetProgramID();
		gpu.UseProgram(argsProgram);
		SetUniform1ui(argsProgram, "uMaxSurfels", maxSurfels);
		SetUniform1ui(argsProgram, "uIndirectSpawnSlack", spawnSlack);
		gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::DispatchArgs), m_resources.dispatchArgsBuffer);
		timedDispatch("dispatch_args", [&]() {
			m_dispatchArgsShader->Dispatch(1u, 1u, 1u);
			m_dispatchArgsShader->WaitForCompletion(rhi::BarrierStorage | rhi::BarrierCommand);
		});
		surfelArgs = m_resources.dispatchArgsBuffer;
	}

	// One grid build per frame: count the overlapped cells (each surfel keeps the slot it was given), scan the
	// cell offsets, then scatter the entries and accumulate the cell averages. The lambda runs at the end of Execute.
	auto buildGrid = [&]() {
		if (placementOnly || !m_gridCountShader || !m_gridCountShader->IsValid() ||
			!m_gridScanShader || !m_gridScanShader->IsValid() ||
			!m_buildGridShader || !m_buildGridShader->IsValid()) {
			return;
		}

		const uint32_t cellCount = m_grid.GetCellCount();
		const uint32_t blockCount = m_grid.GetBlockCount();
		const uint32_t parity = m_frameIndex & 1u;
		const rhi::BufferHandle blockSums = m_grid.GetBlockSumBuffer();
		const rhi::BufferHandle slots = m_grid.GetSlotBuffer();
		const rhi::BufferHandle pending = m_grid.GetPendingBuffer();
		const rhi::BufferHandle overlap = m_grid.GetOverlapBuffer();

		{
			const rhi::ProgramHandle program = m_gridCountShader->GetProgramID();
			gpu.UseProgram(program);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			SetUniform1ui(program, "uGridCellCount", cellCount);
			SetUniform1ui(program, "uGridBlockCount", blockCount);
			SetUniform1ui(program, "uBlockParity", parity);
			SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridBlockSums), blockSums);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridSlots), slots);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridPending), pending);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridOverlapCells), overlap);
			timedDispatch("grid_count", [&]() {
				DispatchSurfelWide(*m_gridCountShader, surfelArgs, groupsSurfels);
				m_gridCountShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}
		{
			const rhi::ProgramHandle program = m_gridScanShader->GetProgramID();
			gpu.UseProgram(program);
			SetUniform1ui(program, "uGridCellCount", cellCount);
			SetUniform1ui(program, "uGridBlockCount", blockCount);
			SetUniform1ui(program, "uBlockParity", parity);
			SetUniform1ui(program, "uEntryCapacity", m_grid.GetEntryCapacity());
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridBlockSums), blockSums);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridPending), pending);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridCellSums), m_resources.gridCellSumBuffer);
			timedDispatch("grid_scan", [&]() {
				m_gridScanShader->Dispatch(blockCount, 1u, 1u);
				m_gridScanShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}
		{
			const rhi::ProgramHandle program = m_buildGridShader->GetProgramID();
			gpu.UseProgram(program);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			SetUniform1ui(program, "uGridCellCount", cellCount);
			SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridSlots), slots);
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridOverlapCells), overlap);
			SetUniform1ui(program, "uCellAverageAllCells", m_settings.cellAverageAllCells ? 1u : 0u);
			timedDispatch("grid_build", [&]() {
				DispatchSurfelWide(*m_buildGridShader, surfelArgs, groupsSurfels);
				m_buildGridShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}
	};

	if (placementOnly && !m_wasPlacementValidationMode) {
		// Placement validation resets on entry, then accumulates spawned
		// world-space records without transform reattachment. A per-frame reset
		// makes the debug draw look like sparse screen-tile samples instead of
		// a persistent surface distribution.
		m_pool.ResetFreeList();
		BindCoreResources(transformBuffer);
	}
	m_wasPlacementValidationMode = placementOnly;

	if (!placementOnly && m_updateShader && m_updateShader->IsValid() && transformBuffer.IsValid() && transformCount > 0u) {
		const rhi::ProgramHandle program = m_updateShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uTransformCount", transformCount);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform2f(program, "uViewportSize", static_cast<float>(m_width), static_cast<float>(m_height));
		SetUniformMat4(program, "uView", context.view);
		SetUniformMat4(program, "uProjection", context.proj);
		SetUniform1f(program, "uTargetSurfelScreenRadiusPx", m_settings.targetSurfelScreenRadiusPx);
		SetUniform1f(program, "uMinSurfelRadius", m_settings.minSurfelRadius);
		SetUniform1f(program, "uMaxSurfelRadius", m_settings.maxSurfelRadius);
		SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);
		SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
		timedDispatch("transform_update", [&]() {
			DispatchSurfelWide(*m_updateShader, surfelArgs, groupsSurfels);
			m_updateShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && m_recycleShader && m_recycleShader->IsValid()) {
		const rhi::ProgramHandle program = m_recycleShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform3fv(program, "uCameraPosition", cameraPosition);
		SetUniform1ui(program, "uMaxRecycleCount", m_settings.maxRecycleCountPerFrame);
		SetUniform1f(program, "uMaxDistance", gridSettings.useNonLinearGrid ? gridSettings.farScale : glm::length(gridSettings.worldExtent));
		SetUniform1f(program, "uSpawnPopulationKappa", std::max(m_settings.spawnPopulationKappa, 0.0f));
		SetUniform1f(program, "uSpawnPopulationCap", static_cast<float>(m_settings.spawnPopulationCap));
		const uint32_t spawnTileSizeForCount = std::max(m_settings.spawnTileSize, 1u);
		SetUniform1ui(program, "uSpawnTileCount",
			DivRoundUp(m_width, spawnTileSizeForCount) * DivRoundUp(m_height, spawnTileSizeForCount));
		timedDispatch("recycling", [&]() {
			DispatchSurfelWide(*m_recycleShader, surfelArgs, groupsSurfels);
			m_recycleShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}


	if (!placementOnly && m_coverageShader && m_coverageShader->IsValid()) {
		const rhi::ProgramHandle program = m_coverageShader->GetProgramID();
		const uint32_t tileCount = std::max(m_coverageTileCapacity, 1u);
		const uint32_t projectedWorkItems = std::min(maxSurfels, std::max(m_settings.maxProjectedSurfelsPerFrame, 1u));
		gpu.UseProgram(program);
		BindSurfelGIGBufferTextures(context);
		SetSurfelGIGBufferSamplerUniforms(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1ui(program, "uMaxProjectedSurfelsPerFrame", projectedWorkItems);
		SetUniform2ui(program, "uResolution", m_width, m_height);
		SetUniform2ui(program, "uCoverageTileCount", m_coverageTileCountX, m_coverageTileCountY);
		SetUniform1ui(program, "uTileSize", std::max(m_settings.spawnTileSize, 1u));
		SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
		SetUniformMat4(program, "uInvView", glm::inverse(context.view));
		SetUniformMat4(program, "uView", context.view);
		SetUniformMat4(program, "uProjection", context.proj);
		SetUniform2f(program, "uViewportSize", static_cast<float>(m_width), static_cast<float>(m_height));
		SetUniform1f(program, "uCoverageNormalCos", m_settings.coverageNormalCos);
		SetUniform1f(program, "uCoverageDepthTolerance", m_settings.coverageDepthTolerance);
		SetUniform1ui(program, "uCoveragePassMode", 0u);
		timedDispatch("coverage_reset", [&]() {
			m_coverageShader->Dispatch(DivRoundUp(tileCount, 64u), 1u, 1u);
			m_coverageShader->WaitForCompletion(rhi::BarrierStorage);
		});
		SetUniform1ui(program, "uCoveragePassMode", 1u);
		timedDispatch("coverage_project", [&]() {
			m_coverageShader->Dispatch(DivRoundUp(projectedWorkItems, 64u), 1u, 1u);
			m_coverageShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	const bool stationaryCoverageMaintenance =
		!cameraMoving &&
		m_stationaryFrameCount > m_settings.stationaryFastFillFrames;
	const bool runCoverageSpawn =
		!stationaryCoverageMaintenance ||
		(m_frameIndex % 3u) == 0u;
	if (runCoverageSpawn && m_spawnShader && m_spawnShader->IsValid()) {
		const uint32_t tileSize = std::max(m_settings.spawnTileSize, 1u);
		const uint32_t tilesX = DivRoundUp(m_width, tileSize);
		const uint32_t tilesY = DivRoundUp(m_height, tileSize);
		const bool cameraMotionBoost = cameraMoving || m_frameIndex < m_settings.fastFillFrameCount;
		// Each spawn dispatch searches every pixel of every tile (two-level reduction), so one pass
		// is a complete GIBS spawn decision. Repeating it per frame only multiplies the cost.
		const bool fastCoverageFill = cameraMotionBoost ||
			(m_stationaryFrameCount > 0u && m_stationaryFrameCount <= m_settings.stationaryFastFillFrames);
		const uint32_t spawnPassCount = 1u;
		const uint32_t spawnCandidateCount = fastCoverageFill ? 8u : 4u;
		m_lastSpawnPassCount = spawnPassCount;
		const uint32_t tileCount = tilesX * tilesY;
		const rhi::ProgramHandle program = m_spawnShader->GetProgramID();
		gpu.UseProgram(program);

		BindSurfelGIGBufferTextures(context);
		SetSurfelGIGBufferSamplerUniforms(program);
		SetUniform1ui(program, "uMaxSpawnCandidates", tileCount);
		SetUniform2ui(program, "uTileCount", tilesX, tilesY);
		SetUniform1ui(program, "uTileSize", tileSize);
		SetUniform1ui(program, "uSpawnPassCount", spawnPassCount);
		SetUniform1ui(program, "uSpawnCandidateCount", spawnCandidateCount);
		SetUniform1ui(program, "uSpawnSampleStride", std::max(m_settings.spawnSearchStride, 1u));
		// Coverage from last frame's gather replaces the per-pixel grid query once it exists.
		const bool useCoverageMap = m_frameIndex > 1u && m_resources.coverageTexture.IsValid() && m_indirectWidth > 0u;
		gpu.BindTexture(kSurfelGICoverageUnit, useCoverageMap ? m_resources.coverageTexture : rhi::TextureHandle{});
		SetUniform1i(program, "uCoverageTex", static_cast<int>(kSurfelGICoverageUnit));
		SetUniform1i(program, "uUseCoverageMap", useCoverageMap ? 1 : 0);
		SetUniform2f(program, "uCoverageScale", static_cast<float>(m_indirectWidth) / static_cast<float>(std::max(m_width, 1u)),
			static_cast<float>(m_indirectHeight) / static_cast<float>(std::max(m_height, 1u)));
		SetUniform1f(program, "uSpawnPopulationKappa", std::max(m_settings.spawnPopulationKappa, 0.0f));
		SetUniform1ui(program, "uMaxSpawnsPerFrame", m_settings.maxSpawnsPerFrame);
		SetUniform1f(program, "uSpawnBudgetTileDivisor", std::max(m_settings.spawnBudgetTileDivisor, 1.0f));
		SetUniform1f(program, "uSpawnPopulationCap", static_cast<float>(m_settings.spawnPopulationCap));
		SetUniform1ui(program, "uSpawnBudgetCeiling", m_settings.maxSpawnBudgetPerFrame);
		SetUniform1ui(program, "uSpawnBudgetDeficitLimit", m_settings.spawnBudgetDeficitLimit ? 1u : 0u);
		SetUniform2ui(program, "uResolution", m_width, m_height);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uTransformCount", transformCount);
		SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
		SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
		SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
		SetUniform3fv(program, "uGridMin", gridMin);
		SetUniform3fv(program, "uGridMax", gridMax);
		SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
		SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
		SetUniformMat4(program, "uInvView", glm::inverse(context.view));
		SetUniformMat4(program, "uView", context.view);
		SetUniformMat4(program, "uProjection", context.proj);
		SetUniform2f(program, "uViewportSize", static_cast<float>(m_width), static_cast<float>(m_height));
		SetUniform1f(program, "uTargetSurfelScreenRadiusPx", m_settings.targetSurfelScreenRadiusPx);
		SetUniform1f(program, "uMinSurfelRadius", m_settings.minSurfelRadius);
		SetUniform1f(program, "uMaxSurfelRadius", m_settings.maxSurfelRadius);
		SetUniform1f(program, "uCoverageThreshold", m_settings.spawnCoverageThreshold);
		SetUniform1f(program, "uPlacementThreshold", m_settings.spawnCoverageThreshold);
		SetUniform1f(program, "uRemovalThreshold", m_settings.spawnCoverageThreshold * 1.12f + 0.04f);
		SetUniform1f(program, "uNormalRejectCos", m_settings.normalRejectCos);
		SetUniform3fv(program, "uDirectionalLightDirection", lightDirection);
		SetUniform3fv(program, "uDirectionalLightRadiance", lightRadiance);
		SetUniform3fv(program, "uSkyRadiance", SurfelSkyRadiance(context, skybox, m_settings));
		SetUniform1i(program, "uPlacementValidationMode", placementOnly ? 1 : 0);
		SetUniform1ui(program, "uCameraMotionBoost", cameraMotionBoost ? 1u : 0u);
		SetUniform1ui(program, "uBypassCoverageSearch", 0u);
		SetUniform3fv(program, "uCameraPosition", cameraPosition);

		// Spawned surfels must be visible to same-frame ray tracing and final
		// gather; otherwise the composited output can remain one frame behind
		// or black while the pool is filling. Rebuild between bounded spawn
		// passes so later visible-tile samples observe fresh coverage.
		for (uint32_t spawnPass = 0u; spawnPass < spawnPassCount; ++spawnPass) {
			SetUniform1ui(program, "uSpawnPassIndex", spawnPass);
			timedDispatch("coverage_spawn", [&]() {
				// One 256-lane workgroup per tile (see spawn.comp).
				m_spawnShader->Dispatch(tileCount, 1u, 1u);
				m_spawnShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}


	}
	else if (!runCoverageSpawn) {
		m_lastSpawnPassCount = 0u;
	}

	const uint32_t rayUpdateInterval = std::max(m_settings.rayUpdateInterval, 1u);
	const bool updateRaysThisFrame = !placementOnly &&
		(rayUpdateInterval <= 1u ||
		 m_frameIndex < m_settings.fastFillFrameCount ||
		 (m_frameIndex % rayUpdateInterval) == 0u);
	if (!placementOnly && updateRaysThisFrame && m_requestRaysShader && m_requestRaysShader->IsValid()) {
		const rhi::ProgramHandle program = m_requestRaysShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);
		timedDispatch("ray_request", [&]() {
			m_requestRaysShader->Dispatch(groupsSurfels, 1u, 1u);
			m_requestRaysShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && updateRaysThisFrame && m_allocateRaysShader && m_allocateRaysShader->IsValid()) {
		const rhi::ProgramHandle program = m_allocateRaysShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRayBudget", m_settings.maxRayBudget);
		// Three phases (allocate_rays.comp): remainder histogram, threshold search (one group), grant.
		timedDispatch("ray_allocation", [&]() {
			SetUniform1ui(program, "uAllocPhase", 0u);
			m_allocateRaysShader->Dispatch(groupsSurfels, 1u, 1u);
			m_allocateRaysShader->WaitForCompletion(rhi::BarrierStorage);
			SetUniform1ui(program, "uAllocPhase", 1u);
			m_allocateRaysShader->Dispatch(1u, 1u, 1u);
			m_allocateRaysShader->WaitForCompletion(rhi::BarrierStorage);
			SetUniform1ui(program, "uAllocPhase", 2u);
			m_allocateRaysShader->Dispatch(groupsSurfels, 1u, 1u);
			m_allocateRaysShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && updateRaysThisFrame && m_generateRaysShader && m_generateRaysShader->IsValid()) {
		const rhi::ProgramHandle program = m_generateRaysShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1i(program, "uUseRayGuiding", m_settings.useRayGuiding ? 1 : 0);
		SetUniform1f(program, "uRayTMin", 0.01f);
		const float localBounceRayLength = std::clamp(m_settings.maxSurfelRadius * 2.5f, 4.0f, 12.0f);
		SetUniform1f(program, "uRayTMax", localBounceRayLength);
		SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);
		timedDispatch("ray_generation", [&]() {
			m_generateRaysShader->Dispatch(groupsSurfels, 1u, 1u);
			m_generateRaysShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	const glm::vec3 surfelSkyRadiance = SurfelSkyRadiance(context, skybox, m_settings);
	if (!placementOnly && updateRaysThisFrame && m_traceRaysShader && m_traceRaysShader->IsValid()) {
		const uint32_t rayBudget = std::max(m_settings.maxRayBudget, 1u);
		const rhi::ProgramHandle program = m_traceRaysShader->GetProgramID();
		gpu.UseProgram(program);
		BindSurfelGIGBufferTextures(context);
		SetSurfelGIGBufferSamplerUniforms(program);
		uint32_t bvhTriangleCount = 0u;
		uint32_t bvhNodeCount = 0u;
		uint32_t bvhInstanceCount = 0u;
		uint32_t bvhInstanceNodeCount = 0u;
		if (m_settings.useSoftwareBVHTrace && context.rtSceneResources && sceneGraph) {
			RTSceneResources::IncrementalBuildSettings rtBuildSettings{};
			rtBuildSettings.maxBuildMsPerFrame = std::max(static_cast<double>(context.surfelGIRTBuildBudgetMs), 0.05);
			rtBuildSettings.maxBlasTrianglesPerFrame = static_cast<std::size_t>(std::max(context.surfelGIRTMaxBLASTrianglesPerFrame, 1));
			rtBuildSettings.maxResidentBytes = static_cast<std::size_t>(std::max(context.surfelGIRTMaxResidentMB, 1)) * 1024ull * 1024ull;
			rtBuildSettings.includeSkinnedMeshes = context.surfelGIRTIncludeSkinnedMeshes;
			// GPU time of the scene update is labelled on its own; the CPU split (instance list, upload) is in the stats.
			timedDispatch("rt_scene_update", [&]() {
				context.rtSceneResources->EnsureIncrementalBLAS(sceneGraph, rtBuildSettings);
			});
			{
				const RTSceneResources::Diagnostics& rtDiagnostics = context.rtSceneResources->GetDiagnostics();
				m_stats.rtInstanceListMs = static_cast<float>(rtDiagnostics.instanceListMsThisFrame);
				m_stats.rtUploadMs = static_cast<float>(rtDiagnostics.uploadMsThisFrame);
				m_stats.rtUploadRan = rtDiagnostics.uploadRanThisFrame ? 1u : 0u;
				m_stats.rtGeometryUploaded = rtDiagnostics.geometryUploadedThisFrame ? 1u : 0u;
				m_stats.rtInstanceBufferRewritten = rtDiagnostics.instanceBufferRewrittenThisFrame ? 1u : 0u;
			}
			if (context.rtSceneResources->IsIncrementalReady()) {
				context.rtSceneResources->BindIncrementalForTracing(
					ToGLuint(SurfelGIBinding::BvhTriangles),
					ToGLuint(SurfelGIBinding::BvhNodes),
					ToGLuint(SurfelGIBinding::RtInstances),
					ToGLuint(SurfelGIBinding::RtInstanceNodes));
				bvhTriangleCount = static_cast<uint32_t>(std::min<std::size_t>(
					context.rtSceneResources->GetIncrementalTriangleCount(),
					std::numeric_limits<uint32_t>::max()));
				bvhNodeCount = static_cast<uint32_t>(std::min<std::size_t>(
					context.rtSceneResources->GetIncrementalNodeCount(),
					std::numeric_limits<uint32_t>::max()));
				bvhInstanceCount = static_cast<uint32_t>(std::min<std::size_t>(
					context.rtSceneResources->GetIncrementalInstanceCount(),
					std::numeric_limits<uint32_t>::max()));
				bvhInstanceNodeCount = static_cast<uint32_t>(std::min<std::size_t>(
					context.rtSceneResources->GetIncrementalInstanceNodeCount(),
					std::numeric_limits<uint32_t>::max()));
			}
		}
		if (bvhTriangleCount == 0u || bvhNodeCount == 0u) {
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::BvhTriangles), {});
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::BvhNodes), {});
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RtInstances), {});
			gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RtInstanceNodes), {});
		}
		SetUniform1ui(program, "uActiveRayCount", rayBudget);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
		SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
		SetUniform1ui(program, "uTriangleCount", bvhTriangleCount);
		SetUniform1ui(program, "uBVHNodeCount", bvhNodeCount);
		SetUniform1ui(program, "uRTInstanceCount", bvhInstanceCount);
		SetUniform1ui(program, "uRTInstanceNodeCount", bvhInstanceNodeCount);
		SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
		SetUniform3fv(program, "uGridMin", gridMin);
		SetUniform3fv(program, "uGridMax", gridMax);
		SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
		SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
		SetUniformMat4(program, "uInvView", glm::inverse(context.view));
		SetUniformMat4(program, "uView", context.view);
		SetUniformMat4(program, "uProjection", context.proj);
		SetUniform1f(program, "uNormalRejectCos", m_settings.normalRejectCos);
		SetUniform3fv(program, "uDirectionalLightDirection", lightDirection);
		SetUniform3fv(program, "uDirectionalLightRadiance", lightRadiance);
		SetUniform3fv(program, "uSkyRadiance", surfelSkyRadiance);
		// When the software BVH is available every ray uses it: it is exact, while the screen-space
		// and surfel-grid traces miss geometry they cannot see (a false miss drops that ray's light).
		const uint32_t bvhTraceStride = 1u;
		SetUniform1i(program, "uUseScreenSpaceTrace", m_settings.useScreenSpaceTrace ? 1 : 0);
		SetUniform1i(program, "uUseSoftwareBVHTrace", (m_settings.useSoftwareBVHTrace && bvhTriangleCount > 0u && bvhNodeCount > 0u) ? 1 : 0);
		SetUniform1i(program, "uUseSurfelFallbackTrace", m_settings.useSurfelFallbackTrace ? 1 : 0);
		SetUniform1ui(program, "uBVHTraceStride", bvhTraceStride);
		SetUniform1ui(program, "uContinuationPeriod", m_settings.pathContinuationPeriod);
		SetUniform1i(program, "uSunShadowBVHFallback", m_settings.sunShadowBVHFallback ? 1 : 0);
		SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);
		SetUniform1i(program, "uUseRayGuiding", m_settings.useRayGuiding ? 1 : 0);
		BindSoftwareTraceLighting(program, context, camera, skybox);
		timedDispatch("tracing", [&]() {
			m_traceRaysShader->Dispatch(DivRoundUp(rayBudget, 64u), 1u, 1u);
			m_traceRaysShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && updateRaysThisFrame && m_integrateShader && m_integrateShader->IsValid()) {
		const rhi::ProgramHandle program = m_integrateShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1i(program, "uUseRayGuiding", m_settings.useRayGuiding ? 1 : 0);
		SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);
		timedDispatch("temporal_integration", [&]() {
			DispatchSurfelWide(*m_integrateShader, surfelArgs, groupsSurfels);
			m_integrateShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && updateRaysThisFrame && m_settings.useRadialDepth &&
		m_radialDepthShader && m_radialDepthShader->IsValid()) {
		const rhi::ProgramHandle program = m_radialDepthShader->GetProgramID();
		gpu.UseProgram(program);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uMaxRays", m_settings.maxRayBudget);
		SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
		timedDispatch("radial_depth", [&]() {
			DispatchSurfelWide(*m_radialDepthShader, surfelArgs, groupsSurfels);
			m_radialDepthShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	if (!placementOnly && updateRaysThisFrame && m_settings.useIrradianceSharing &&
		m_irradianceSnapshotShader && m_irradianceSnapshotShader->IsValid() &&
		m_irradianceSharingShader && m_irradianceSharingShader->IsValid()) {
		{
			const rhi::ProgramHandle program = m_irradianceSnapshotShader->GetProgramID();
			gpu.UseProgram(program);
			SetUniform1ui(program, "uMaxSurfels", maxSurfels);
			timedDispatch("irradiance_snapshot", [&]() {
				m_irradianceSnapshotShader->Dispatch(groupsSurfels, 1u, 1u);
				m_irradianceSnapshotShader->WaitForCompletion(rhi::BarrierStorage);
			});
		}
		const rhi::ProgramHandle program = m_irradianceSharingShader->GetProgramID();
		gpu.UseProgram(program);
		const uint32_t maxRayBudget = std::max(m_settings.maxRayBudget, 1u);
		const uint32_t sharingPhaseCount = maxRayBudget <= 1024u ? 4u : (maxRayBudget <= 4096u ? 2u : 1u);
		SetUniform1ui(program, "uMaxSurfels", maxSurfels);
		SetUniform1ui(program, "uFrameIndex", m_frameIndex);
		SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
		SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
		SetUniform1ui(program, "uSharingPhaseCount", sharingPhaseCount);
		SetUniform1ui(program, "uSharingPhaseIndex", sharingPhaseCount > 1u ? (m_frameIndex % sharingPhaseCount) : 0u);
		SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
		SetUniform1i(program, "uUseRadialDepth", m_settings.useRadialDepth ? 1 : 0);
		SetUniform1f(program, "uRadialDepthMinVariance", std::max(m_settings.radialDepthSigmaScale, 1.0e-6f));
		const uint32_t sharingWorkItems = DivRoundUp(maxSurfels, sharingPhaseCount);
		timedDispatch("irradiance_sharing", [&]() {
			m_irradianceSharingShader->Dispatch(DivRoundUp(sharingWorkItems, 64u), 1u, 1u);
			m_irradianceSharingShader->WaitForCompletion(rhi::BarrierStorage);
		});
	}

	// One grid build per frame, after the last irradiance update. The final gather reads the cell averages
	// that the scatter pass accumulates, so this is the only grid write of the frame.
	if (!placementOnly) {
		buildGrid();
	}

	// Runs every frame: it rebuilds the high-water index that dispatch_args turns into next frame's
	// surfel-wide group counts.
	countLiveSurfels();

	gpu.UseProgram({});
	++m_frameIndex;
	const bool periodicStatsReadback = m_frameIndex == 1u || (m_frameIndex % kSurfelStatsReadbackInterval) == 0u;
	if (placementOnly && m_resources.countersBuffer.IsValid()) {
		// Validation reads synchronously so the placement counters belong to this frame.
		ReadSurfelCounters(m_resources.countersBuffer, m_stats);
	}
	else if (m_settings.debugView != SurfelGIDebugView::Off || periodicStatsReadback) {
		IssueCounterReadback();
	}
	PumpCounterReadback();
	m_stats.ready = IsReady();
}

// Light, shadow and sky inputs shared by the software BVH tracers (trace_rays.comp and reference_gi.comp).
void SurfelGIPipeline::BindSoftwareTraceLighting(rhi::ProgramHandle program, RenderContext& context,
	const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox)
{
	rhi::Device& gpu = rhi::Gpu();
const rhi::TextureHandle skyIrradianceMap = skybox && skybox->IsReady() ? skybox->GetIrradianceMap() : rhi::TextureHandle{};
const rhi::TextureHandle skyEnvironmentMap = skybox && skybox->IsReady() ? skybox->GetEnvironmentMap() : rhi::TextureHandle{};
gpu.BindTexture(kSurfelGISkyIrradianceUnit, skyIrradianceMap);
gpu.BindTexture(kSurfelGISkyEnvironmentUnit, skyEnvironmentMap);
SetUniform1i(program, "uSkyIrradianceMap", static_cast<int>(kSurfelGISkyIrradianceUnit));
SetUniform1i(program, "uSkyEnvironmentMap", static_cast<int>(kSurfelGISkyEnvironmentUnit));
SetUniform1i(program, "uUseSkyIrradianceMap", skyIrradianceMap.IsValid() ? 1 : 0);
SetUniform1i(program, "uUseSkyEnvironmentMap", skyEnvironmentMap.IsValid() ? 1 : 0);
const float skyboxBounceScale = context.iblIntensity * context.diffuseIBLScale;
SetUniform1f(program, "uSkyDiffuseScale", std::clamp(skyboxBounceScale, 0.0f, kSurfelGISkyDiffuseMaxScale));
uint32_t lightCount = 0u;
rhi::TextureHandle shadowArray;
rhi::BufferHandle shadowMatrices;
glm::vec4 cascadeSplits(10.0f, 30.0f, 100.0f, 500.0f);
if (context.lightManager && context.lightManager->GetLightDataSSBO().IsValid()) {
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Lights),
		context.lightManager->GetLightDataSSBO());
	lightCount = static_cast<uint32_t>(std::max(context.lightManager->GetActiveLightCount(), 0));
	shadowArray = context.lightManager->GetShadowArrayTexture();
	shadowMatrices = context.lightManager->GetShadowMatricesSSBO();
	const auto& shadowConfig = context.lightManager->shadowConfig;
	const float nearPlane = std::max(context.shadowNear, camera ? camera->GetCameraNearPlane() : context.shadowNear);
	const float farPlane = std::max(nearPlane + 1.0f,
		std::min(context.shadowFar, camera ? camera->GetCameraFarPlane() : context.shadowFar));
	const int cascadeCount = std::max(1, shadowConfig.directionalCascadeCount);
	const std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(
		nearPlane,
		farPlane,
		cascadeCount,
		shadowConfig.directionalSplitLambda);
	for (int i = 0; i < std::min(4, static_cast<int>(splits.size())); ++i) {
		cascadeSplits[i] = splits[i];
	}
}
else {
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Lights), {});
}
gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::ShadowMatrices),
	shadowMatrices);
gpu.BindTexture(kSurfelGIShadowArrayUnit, shadowArray);
SetUniform1i(program, "uMultiLightShadowArray", static_cast<int>(kSurfelGIShadowArrayUnit));
const bool enableTraceShadows = EnvFlagEnabled("NOX_SURFEL_GI_TRACE_SHADOWS", true);
SetUniform1i(program, "uEnableShadows", (enableTraceShadows && context.enableShadows && shadowArray.IsValid() && shadowMatrices.IsValid()) ? 1 : 0);
SetUniform4fv(program, "uCascadeSplits", cascadeSplits);
SetUniform1f(program, "uShadowBias", context.shadowBias);
SetUniform1ui(program, "uLightCount", lightCount);
	// Next-event estimation (plan section 14): the emissive triangles of the traced instances, built with the
	// instance list. Without an incremental RT scene there are none.
	const bool emittersReady = context.rtSceneResources && context.rtSceneResources->IsIncrementalReady();
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Emitters),
		emittersReady ? context.rtSceneResources->GetEmitterSSBO() : rhi::BufferHandle{});
	SetUniform1ui(program, "uEmitterCount",
		emittersReady ? static_cast<uint32_t>(context.rtSceneResources->GetEmitterCount()) : 0u);
	SetUniform1f(program, "uEmitterPowerTotal",
		emittersReady ? context.rtSceneResources->GetEmitterPowerTotal() : 0.0f);
}

// Reference mode: one path per quarter-resolution texel per frame, averaged over frames until the view changes.
constexpr uint32_t kReferenceSamplesPerFrame = 1u;
// Diffuse bounces after the first hit. Direct light is taken at every hit with shadow rays.
// Hits that are always followed. Later hits use Russian roulette (reference_gi.comp), so the reference is unbiased.
constexpr uint32_t kReferenceBounces = 3u;

void SurfelGIPipeline::ExecuteIndependentReference(RenderContext& context,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!m_referenceShader || !m_referenceShader->IsValid() ||
		!m_resources.referenceMeanTexture.IsValid() || !m_resources.referenceCountTexture.IsValid()) {
		return;
	}

	const glm::vec3 fallbackDirection = glm::vec3(-0.35f, -1.0f, -0.25f);
	const std::shared_ptr<BaseLight> primaryDirectionalLight = FindPrimaryDirectionalLight(context);
	const glm::vec3 lightDirection = primaryDirectionalLight
		? NormalizeOrFallback(primaryDirectionalLight->GetDirection(), fallbackDirection)
		: (dirLight ? NormalizeOrFallback(dirLight->GetLightDirection(), fallbackDirection) : fallbackDirection);
	const glm::vec3 lightRadiance = primaryDirectionalLight
		? primaryDirectionalLight->GetColor() * std::max(primaryDirectionalLight->GetIntensity(), 0.0f)
		: glm::vec3(0.0f);  // Only a registered light feeds the GI. The default DirectionalLight object is not a light in the scene.

	// The accumulation restarts when the view changes or the textures were just reallocated.
	const bool viewChanged = !m_hasReferenceView || MatrixMaxAbsDelta(context.view, m_referenceView) > 1.0e-6f;
	const bool reset = viewChanged || m_referenceResetPending;
	m_referenceView = context.view;
	m_hasReferenceView = true;
	m_referenceResetPending = false;

	// Scene BVH from the same incremental build the surfel trace uses. Without one the reference is sky-only.
	uint32_t bvhTriangleCount = 0u;
	uint32_t bvhNodeCount = 0u;
	uint32_t bvhInstanceCount = 0u;
	uint32_t bvhInstanceNodeCount = 0u;
	if (context.rtSceneResources && sceneGraph) {
		RTSceneResources::IncrementalBuildSettings rtBuildSettings{};
		rtBuildSettings.maxBuildMsPerFrame = std::max(static_cast<double>(context.surfelGIRTBuildBudgetMs), 0.05);
		rtBuildSettings.maxBlasTrianglesPerFrame = static_cast<std::size_t>(std::max(context.surfelGIRTMaxBLASTrianglesPerFrame, 1));
		rtBuildSettings.maxResidentBytes = static_cast<std::size_t>(std::max(context.surfelGIRTMaxResidentMB, 1)) * 1024ull * 1024ull;
		rtBuildSettings.includeSkinnedMeshes = context.surfelGIRTIncludeSkinnedMeshes;
		context.rtSceneResources->EnsureIncrementalBLAS(sceneGraph, rtBuildSettings);
		if (context.rtSceneResources->IsIncrementalReady()) {
			context.rtSceneResources->BindIncrementalForTracing(
				ToGLuint(SurfelGIBinding::BvhTriangles),
				ToGLuint(SurfelGIBinding::BvhNodes),
				ToGLuint(SurfelGIBinding::RtInstances),
				ToGLuint(SurfelGIBinding::RtInstanceNodes));
			bvhTriangleCount = static_cast<uint32_t>(std::min<std::size_t>(
				context.rtSceneResources->GetIncrementalTriangleCount(), std::numeric_limits<uint32_t>::max()));
			bvhNodeCount = static_cast<uint32_t>(std::min<std::size_t>(
				context.rtSceneResources->GetIncrementalNodeCount(), std::numeric_limits<uint32_t>::max()));
			bvhInstanceCount = static_cast<uint32_t>(std::min<std::size_t>(
				context.rtSceneResources->GetIncrementalInstanceCount(), std::numeric_limits<uint32_t>::max()));
			bvhInstanceNodeCount = static_cast<uint32_t>(std::min<std::size_t>(
				context.rtSceneResources->GetIncrementalInstanceNodeCount(), std::numeric_limits<uint32_t>::max()));
		}
	}
	if (bvhTriangleCount == 0u || bvhNodeCount == 0u) {
		gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::BvhTriangles), {});
		gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::BvhNodes), {});
		gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RtInstances), {});
		gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RtInstanceNodes), {});
	}

	const uint32_t quarterWidth = ReferenceExtent(m_width);
	const uint32_t quarterHeight = ReferenceExtent(m_height);
	const rhi::ProgramHandle program = m_referenceShader->GetProgramID();
	gpu.UseProgram(program);
	BindSurfelGIGBufferTextures(context);
	SetSurfelGIGBufferSamplerUniforms(program);
	gpu.BindImage(0, m_resources.referenceMeanTexture, 0, false, 0, rhi::ImageAccess::ReadWrite, rhi::Format::RGBA32F);
	gpu.BindImage(1, m_resources.referenceCountTexture, 0, false, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32F);
	SetUniform2ui(program, "uQuarterResolution", quarterWidth, quarterHeight);
	SetUniform1ui(program, "uFrameIndex", m_frameIndex);
	SetUniform1ui(program, "uResetAccumulation", reset ? 1u : 0u);
	SetUniform1ui(program, "uSamplesPerFrame", kReferenceSamplesPerFrame);
	SetUniform1ui(program, "uBounces", kReferenceBounces);
	SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
	SetUniformMat4(program, "uInvView", glm::inverse(context.view));
	SetUniform1i(program, "uUseSoftwareBVHTrace", (bvhTriangleCount > 0u && bvhNodeCount > 0u) ? 1 : 0);
	SetUniform1ui(program, "uTriangleCount", bvhTriangleCount);
	SetUniform1ui(program, "uBVHNodeCount", bvhNodeCount);
	SetUniform1ui(program, "uRTInstanceCount", bvhInstanceCount);
	SetUniform1ui(program, "uRTInstanceNodeCount", bvhInstanceNodeCount);
	SetUniform3fv(program, "uDirectionalLightDirection", lightDirection);
	SetUniform3fv(program, "uDirectionalLightRadiance", lightRadiance);
	SetUniform3fv(program, "uSkyRadiance", SurfelSkyRadiance(context, skybox, m_settings));
	BindSoftwareTraceLighting(program, context, camera, skybox);

	rhi::QueryHandle beginQuery;
	rhi::QueryHandle endQuery;
	BeginGpuTiming("reference_gi", beginQuery, endQuery);
	m_referenceShader->Dispatch(DivRoundUp(quarterWidth, 8u), DivRoundUp(quarterHeight, 8u), 1u);
	m_referenceShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch | rhi::BarrierStorage);
	EndGpuTiming("reference_gi", beginQuery, endQuery);
	gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::ReadWrite, rhi::Format::RGBA32F);
	gpu.BindImage(1, {}, 0, false, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32F);
	gpu.UseProgram({});
	++m_frameIndex;
}

// Reference mode: the upscale resamples the quarter-resolution accumulation into the indirect texture, which is the
// texture the lighting pass reads for the surfel term. Coverage is the reference's alpha (1 on geometry).
void SurfelGIPipeline::ApplyIndependentReference(RenderContext& context)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!m_upscaleFilterShader || !m_upscaleFilterShader->IsValid() ||
		!m_resources.referenceMeanTexture.IsValid() || !m_resources.indirectTexture.IsValid() ||
		!context.gbufferFBO) {
		return;
	}

	const rhi::ProgramHandle upscaleProgram = m_upscaleFilterShader->GetProgramID();
	gpu.UseProgram(upscaleProgram);
	gpu.BindTexture(0, m_resources.referenceMeanTexture);
	gpu.BindTexture(1, context.gbufferFBO->GetColorAttachment(0));
	gpu.BindTexture(2, context.gbufferFBO->GetDepthTexture());
	gpu.BindImage(0, m_resources.indirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	SetUniform1i(upscaleProgram, "uLowResIndirectTex", 0);
	SetUniform1i(upscaleProgram, "uPackedNormalRMTex", 1);
	SetUniform1i(upscaleProgram, "uDepthTex", 2);
	SetUniform2ui(upscaleProgram, "uResolution", m_width, m_height);
	SetUniform2ui(upscaleProgram, "uLowResolution", ReferenceExtent(m_width), ReferenceExtent(m_height));
	SetUniform1ui(upscaleProgram, "uDebugView", 0u);
	m_upscaleFilterShader->Dispatch(DivRoundUp(m_width, 8u), DivRoundUp(m_height, 8u), 1u);
	m_upscaleFilterShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
	gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.UseProgram({});
}

void SurfelGIPipeline::ApplyIndirect(RenderContext& context, FrameBuffer&)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!IsReady() || !m_applyIndirectShader || !m_applyIndirectShader->IsValid() ||
		!m_temporalFilterShader || !m_temporalFilterShader->IsValid() ||
		!m_upscaleFilterShader || !m_upscaleFilterShader->IsValid() ||
		!m_resources.indirectTexture.IsValid() || !m_resources.rawIndirectTexture.IsValid() ||
		!m_resources.filteredIndirectTexture.IsValid() || !m_resources.temporalIndirectTexture.IsValid() ||
		!context.gbufferFBO) {
		m_stats.ready = IsReady();
		return;
	}
	if (m_settings.referenceMode) {
		ApplyIndependentReference(context);
		m_stats.ready = IsReady();
		return;
	}

	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = CameraPositionFromView(context.view);
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);
	const uint32_t applyDebugView = IsSurfelFullscreenDebugView(m_settings.debugView)
		? static_cast<uint32_t>(m_settings.debugView)
		: 0u;
	const bool fullscreenDebug = applyDebugView != 0u;
	const bool denoiseFullscreenDebug =
		applyDebugView == static_cast<uint32_t>(SurfelGIDebugView::RawIndirectIrradiance);
	BindCoreResources({});
	auto timedDispatch = [&](const char* label, auto&& work) {
		rhi::QueryHandle beginQuery;
		rhi::QueryHandle endQuery;
		BeginGpuTiming(label, beginQuery, endQuery);
		work();
		EndGpuTiming(label, beginQuery, endQuery);
	};

	const rhi::ProgramHandle program = m_applyIndirectShader->GetProgramID();
	gpu.UseProgram(program);

	BindSurfelGIGBufferTextures(context);
	gpu.BindImage(0, m_resources.rawIndirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.BindImage(1, m_resources.coverageTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);

	SetSurfelGIGBufferSamplerUniforms(program);
	SetUniform1i(program, "uUseMaterialIDReject", 1);
	SetUniform2ui(program, "uResolution", m_indirectWidth, m_indirectHeight);
	SetUniform2ui(program, "uInputResolution", m_width, m_height);
	SetUniformMat4(program, "uInvProjection", glm::inverse(context.proj));
	SetUniformMat4(program, "uInvView", glm::inverse(context.view));
	SetUniform1ui(program, "uMaxSurfels", m_settings.maxSurfels);
	SetUniform1ui(program, "uTransformCount", m_lastTransformCount);
	SetUniform1ui(program, "uFrameIndex", m_frameIndex);
	SetUniform1ui(program, "uSpawnTileSize", std::max(m_settings.spawnTileSize, 1u));
	SetUniform1ui(program, "uSpawnPassCount", std::max(m_lastSpawnPassCount, 1u));
	SetUniform1ui(program, "uSpawnDebugFrameIndex", m_frameIndex > 0u ? m_frameIndex - 1u : 0u);
	SetUniform2ui(program, "uCoverageTileCount", m_coverageTileCountX, m_coverageTileCountY);
	SetUniform1ui(program, "uGridCellCount", m_grid.GetCellCount());
	SetUniform1ui(program, "uMaxSurfelsPerCell", m_grid.GetSettings().maxSurfelsPerCell);
	SetUniform1ui(program, "uMaxGatherSurfelsPerPixel", m_settings.maxGatherSurfelsPerPixel);
	SetUniform1ui(program, "uGatherNeighborRadius", m_settings.gatherNeighborRadius);
	SetUniform3ui(program, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
	SetUniform3fv(program, "uGridMin", gridMin);
	SetUniform3fv(program, "uGridMax", gridMax);
	SetSurfelGridUniforms(program, gridSettings, gridMin, gridMax);
	SetUniform1f(program, "uNormalRejectCos", m_settings.finalGatherNormalCos);
	SetUniform1i(program, "uUseRadialDepth", m_settings.useRadialDepth ? 1 : 0);
	SetUniform1f(program, "uRadialDepthMinVariance", std::max(m_settings.radialDepthSigmaScale, 1.0e-6f));
	SetUniform1f(program, "uFallbackStrength", m_settings.cellAverageFallbackStrength);
	SetUniform1ui(program, "uDebugView", applyDebugView);

	timedDispatch("final_gather", [&]() {
		m_applyIndirectShader->Dispatch(DivRoundUp(m_indirectWidth, 8u), DivRoundUp(m_indirectHeight, 8u), 1u);
		m_applyIndirectShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch | rhi::BarrierStorage);
	});
	gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);

	bool didSpatialFilter = false;
	if ((!fullscreenDebug || denoiseFullscreenDebug) && m_spatialFilterShader && m_spatialFilterShader->IsValid()) {
		const rhi::ProgramHandle filterProgram = m_spatialFilterShader->GetProgramID();
		gpu.UseProgram(filterProgram);
		gpu.BindTexture(0, m_resources.rawIndirectTexture);
		gpu.BindTexture(1, context.gbufferFBO->GetColorAttachment(0));
		gpu.BindTexture(2, context.gbufferFBO->GetDepthTexture());
		gpu.BindImage(0, m_resources.filteredIndirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		SetUniform1i(filterProgram, "uRawIndirectTex", 0);
		SetUniform1i(filterProgram, "uPackedNormalRMTex", 1);
		SetUniform1i(filterProgram, "uDepthTex", 2);
		SetUniform2ui(filterProgram, "uResolution", m_indirectWidth, m_indirectHeight);
		SetUniform2ui(filterProgram, "uInputResolution", m_width, m_height);
		timedDispatch("composite_filter", [&]() {
			m_spatialFilterShader->Dispatch(DivRoundUp(m_indirectWidth, 8u), DivRoundUp(m_indirectHeight, 8u), 1u);
			m_spatialFilterShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
		});
		gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		didSpatialFilter = true;
	}

	bool didTemporalResolve = false;
	if ((!fullscreenDebug || denoiseFullscreenDebug) && m_temporalFilterShader && m_temporalFilterShader->IsValid()) {
		const uint32_t historyReadIndex = m_historyReadIndex & 1u;
		const uint32_t historyWriteIndex = historyReadIndex ^ 1u;
		const rhi::ProgramHandle temporalProgram = m_temporalFilterShader->GetProgramID();
		gpu.UseProgram(temporalProgram);
		gpu.BindTexture(0, m_resources.filteredIndirectTexture);
		gpu.BindTexture(1, m_resources.historyIndirectTexture[historyReadIndex]);
		gpu.BindTexture(2, m_resources.historyGeometryTexture[historyReadIndex]);
		gpu.BindTexture(3, context.velocityTex);
		gpu.BindTexture(4, context.gbufferFBO->GetColorAttachment(0));
		gpu.BindTexture(5, context.gbufferFBO->GetDepthTexture());
		gpu.BindImage(0, m_resources.temporalIndirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.BindImage(1, m_resources.historyIndirectTexture[historyWriteIndex], 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.BindImage(2, m_resources.historyGeometryTexture[historyWriteIndex], 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		SetUniform1i(temporalProgram, "uCurrentIndirectTex", 0);
		SetUniform1i(temporalProgram, "uPreviousIndirectTex", 1);
		SetUniform1i(temporalProgram, "uPreviousGeometryTex", 2);
		SetUniform1i(temporalProgram, "uVelocityTex", 3);
		SetUniform1i(temporalProgram, "uPackedNormalRMTex", 4);
		SetUniform1i(temporalProgram, "uDepthTex", 5);
		SetUniform2ui(temporalProgram, "uResolution", m_indirectWidth, m_indirectHeight);
		SetUniform2ui(temporalProgram, "uInputResolution", m_width, m_height);
		SetUniform1i(temporalProgram, "uUseHistory", m_hasTemporalHistory ? 1 : 0);
		SetUniform1i(temporalProgram, "uUseVelocity", context.velocityTex.IsValid() ? 1 : 0);
		SetUniform1f(temporalProgram, "uDepthReject", 0.0025f);
		SetUniform1f(temporalProgram, "uNormalRejectCos", 0.55f);
		SetUniform1f(temporalProgram, "uMotionRejectPixels", 96.0f);
		SetUniform1f(temporalProgram, "uLightingChangeFactor", m_lightingChangeFactor);
		timedDispatch("temporal_resolve", [&]() {
			m_temporalFilterShader->Dispatch(DivRoundUp(m_indirectWidth, 8u), DivRoundUp(m_indirectHeight, 8u), 1u);
			m_temporalFilterShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
		});
		gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.BindImage(1, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.BindImage(2, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		m_historyReadIndex = historyWriteIndex;
		m_hasTemporalHistory = true;
		didTemporalResolve = true;
	}

	const rhi::TextureHandle upscaleSourceTexture = didTemporalResolve
		? m_resources.temporalIndirectTexture
		: (didSpatialFilter ? m_resources.filteredIndirectTexture : m_resources.rawIndirectTexture);
	const rhi::ProgramHandle upscaleProgram = m_upscaleFilterShader->GetProgramID();
	gpu.UseProgram(upscaleProgram);
	gpu.BindTexture(0, upscaleSourceTexture);
	gpu.BindTexture(1, context.gbufferFBO->GetColorAttachment(0));
	gpu.BindTexture(2, context.gbufferFBO->GetDepthTexture());
	gpu.BindImage(0, m_resources.indirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	SetUniform1i(upscaleProgram, "uLowResIndirectTex", 0);
	SetUniform1i(upscaleProgram, "uPackedNormalRMTex", 1);
	SetUniform1i(upscaleProgram, "uDepthTex", 2);
	SetUniform2ui(upscaleProgram, "uResolution", m_width, m_height);
	SetUniform2ui(upscaleProgram, "uLowResolution", m_indirectWidth, m_indirectHeight);
	SetUniform1ui(upscaleProgram, "uDebugView", denoiseFullscreenDebug ? 0u : applyDebugView);
	timedDispatch("upscale_filter", [&]() {
		m_upscaleFilterShader->Dispatch(DivRoundUp(m_width, 8u), DivRoundUp(m_height, 8u), 1u);
		m_upscaleFilterShader->WaitForCompletion(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
	});
	gpu.BindImage(0, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);

	const std::string dumpDir = EnvString("NOX_SURFEL_GI_DUMP_DIR");
	if (!dumpDir.empty()) {
		const std::string explicitDumpFrames = EnvString("NOX_SURFEL_GI_DUMP_FRAMES");
		const uint32_t dumpFrame = std::max(EnvUInt("NOX_SURFEL_GI_DUMP_FRAME", 240u), 1u);
		const uint32_t dumpFrameCount = std::max(EnvUInt("NOX_SURFEL_GI_DUMP_FRAME_COUNT", 1u), 1u);
		const uint32_t dumpFrameInterval = std::max(EnvUInt("NOX_SURFEL_GI_DUMP_FRAME_INTERVAL", 1u), 1u);
		const uint32_t dumpDelta = m_frameIndex >= dumpFrame ? m_frameIndex - dumpFrame : std::numeric_limits<uint32_t>::max();
		const bool dumpScheduledFrame = !explicitDumpFrames.empty()
			? EnvFrameListContains(explicitDumpFrames, m_frameIndex)
			: (m_frameIndex >= dumpFrame &&
				(dumpDelta % dumpFrameInterval) == 0u &&
				(dumpDelta / dumpFrameInterval) < dumpFrameCount);
		if (dumpScheduledFrame && m_lastTextureDumpFrame != m_frameIndex) {
			const std::filesystem::path directory(dumpDir);
			const std::string frameSuffix = (dumpFrameCount > 1u || !explicitDumpFrames.empty())
				? ("_frame" + std::to_string(m_frameIndex))
				: std::string{};
			const bool rawOk = SaveSurfelDebugTexturePNG(
				m_resources.rawIndirectTexture,
				m_indirectWidth,
				m_indirectHeight,
				directory / ("surfel_raw_indirect" + frameSuffix + ".png"));
			const bool filteredOk = SaveSurfelDebugTexturePNG(
				m_resources.filteredIndirectTexture,
				m_indirectWidth,
				m_indirectHeight,
				directory / ("surfel_filtered_indirect" + frameSuffix + ".png"));
			const bool temporalLowOk = SaveSurfelDebugTexturePNG(
				m_resources.temporalIndirectTexture,
				m_indirectWidth,
				m_indirectHeight,
				directory / ("surfel_temporal_lowres_indirect" + frameSuffix + ".png"));
			const bool finalOk = SaveSurfelDebugTexturePNG(
				m_resources.indirectTexture,
				m_width,
				m_height,
				directory / ("surfel_temporal_indirect" + frameSuffix + ".png"));
			std::cout << "[SurfelGITextureDump] frame=" << m_frameIndex
				<< " dir=" << directory.string()
				<< " raw=" << rawOk
				<< " filtered=" << filteredOk
				<< " temporalLow=" << temporalLowOk
				<< " temporal=" << finalOk
				<< '\n';
			m_lastTextureDumpFrame = m_frameIndex;
		}
	}

	gpu.UseProgram({});
	const bool logSubpassMetrics = m_logSubpassMetrics && !m_subpassTimings.empty() &&
		m_subpassMetricsInterval > 0u &&
		(m_frameIndex % m_subpassMetricsInterval) == 0u;
	const bool periodicStatsReadback = m_frameIndex == 1u || (m_frameIndex % kSurfelStatsReadbackInterval) == 0u;
	if (m_settings.debugView != SurfelGIDebugView::Off || periodicStatsReadback || logSubpassMetrics) {
		IssueCounterReadback();
	}
	if (logSubpassMetrics) {
		double totalGpuMs = 0.0;
		for (const SubpassGpuTiming& timing : m_subpassTimings) {
			totalGpuMs += timing.gpuMs;
		}
		std::cout << "[SurfelGISubpassMetrics] frame=" << m_frameIndex
			<< " totalGpuMs=" << totalGpuMs
			<< " readbackDelayFrames=" << kGpuTimingReadbackFrameDelay
			<< " pendingTimingQueries=" << m_pendingTimingQueries.size()
			<< " indirectResolution=" << m_indirectWidth << "x" << m_indirectHeight
			<< " outputResolution=" << m_width << "x" << m_height
			<< " maxSurfels=" << m_settings.maxSurfels
			<< " rayBudget=" << m_settings.maxRayBudget
			<< " gatherBudget=" << m_settings.maxGatherSurfelsPerPixel
			<< " rtInstanceListCpuMs=" << m_stats.rtInstanceListMs
			<< " rtUploadCpuMs=" << m_stats.rtUploadMs
			<< " rtUploadRan=" << m_stats.rtUploadRan
			<< " rtGeometryUploaded=" << m_stats.rtGeometryUploaded
			<< " rtInstanceBufferRewritten=" << m_stats.rtInstanceBufferRewritten
			<< '\n';
		for (const SubpassGpuTiming& timing : m_subpassTimings) {
			std::cout << "[SurfelGISubpassMetrics]   "
				<< timing.label << " gpuMs=" << timing.gpuMs << '\n';
		}
		std::cout << "[SurfelGIDiagCounters] frame=" << m_frameIndex
				<< " lookupsSharing=" << m_stats.lookupsSharing << " entriesSharing=" << m_stats.entriesSharing
				<< " lookupsGather=" << m_stats.lookupsGather << " entriesGather=" << m_stats.entriesGather
				<< " lookupsTrace=" << m_stats.lookupsTrace << " entriesTrace=" << m_stats.entriesTrace
				<< " lookupsSpawn=" << m_stats.lookupsSpawn << " entriesSpawn=" << m_stats.entriesSpawn
				<< " recycleOverTarget=" << m_stats.recycleOverTarget << " recycleStale=" << m_stats.recycleStale
				<< " recycleFar=" << m_stats.recycleFar << " recycleForced=" << m_stats.recycleForced
				<< " spawnDecisions=" << m_stats.spawnDecisions << " spawnBudgetDenied=" << m_stats.spawnBudgetDenied
				<< " radiusCapped=" << m_stats.radiusCapped << " recycledYoung=" << m_stats.recycledYoung << '\n';
		std::cout << "[SurfelGIFrameCounters] frame=" << m_frameIndex
			<< " liveSurfels=" << m_stats.liveSurfels
			<< " spawned=" << m_stats.spawnedThisFrame
			<< " recycled=" << m_stats.recycledThisFrame
			<< " requestedRays=" << m_stats.requestedRays
			<< " allocatedRays=" << m_stats.allocatedRays
			<< " coverageVisibleTileCount=" << m_stats.coverageVisibleTileCount
			<< " underCoveredTiles=" << m_stats.underCoveredTileCount
			<< " highPriorityTiles=" << m_stats.highPriorityTileCount
			<< " coverageSpawnedTiles=" << m_stats.coverageSpawnedTileCount
			<< " coverageInvalidTiles=" << m_stats.coverageInvalidTileCount
			<< " projectedSurfels=" << m_stats.projectedSurfels
			<< " coverageRejectDepthNormalMaterialRadius="
			<< m_stats.coverageDepthRejected << "/"
			<< m_stats.coverageNormalRejected << "/"
			<< m_stats.coverageMaterialRejected << "/"
			<< m_stats.coverageRadiusRejected
			<< " rayHitsGridScreenBVHEnvMiss="
			<< m_stats.rayHitGrid << "/"
			<< m_stats.rayHitScreen << "/"
			<< m_stats.rayHitBVH << "/"
			<< m_stats.rayHitEnvironment << "/"
			<< m_stats.rayHitMiss
			<< " raySupportAccepted=" << m_stats.acceptedRaySamples
			<< " temporalClamps=" << m_stats.temporalClampEvents
			<< " sharingAppliedRejected="
			<< m_stats.sharingAppliedSurfels << "/"
			<< m_stats.sharingRejectedDarkLift
			<< " gatherDirectFallbackBleedValidEmpty="
			<< m_stats.finalGatherDirectPixels << "/"
			<< m_stats.finalGatherFallbackPixels << "/"
			<< m_stats.finalGatherBleedPixels << "/"
			<< m_stats.finalGatherValidPixels << "/"
			<< m_stats.finalGatherEmptyPixels
			<< " overflowGridEntries=" << m_stats.overflowGridEntries
			<< " youngSurfels=" << m_stats.youngSurfels
			<< " rayShortfall=" << m_stats.rayShortfall
			<< " staleRaySlots=" << m_stats.staleRaySlots
			<< '\n';
	}
	m_stats.ready = IsReady();
}

void SurfelGIPipeline::RenderDebug(RenderContext& context)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!IsReady() ||
		m_settings.debugView == SurfelGIDebugView::Off) {
		return;
	}

	rhi::QueryHandle debugBeginQuery;
	rhi::QueryHandle debugEndQuery;
	BeginGpuTiming("debug_overhead", debugBeginQuery, debugEndQuery);

	if (IsSurfelFullscreenDebugView(m_settings.debugView)) {
		if (!m_debugPresentProgram.IsValid() || !context.screenQuad || !m_resources.indirectTexture.IsValid()) {
			EndGpuTiming("debug_overhead", debugBeginQuery, debugEndQuery);
			return;
		}

		SurfelRenderStateGuard renderStateGuard;
		gpu.BindFramebuffer(PresentTarget::Get());
		gpu.SetViewport(rhi::Rect{ 0, 0, context.width, context.height });
		gpu.SetDepthTest(false);
		gpu.SetBlendEnabled(false);
		gpu.SetDepthWrite(false);
		gpu.UseProgram(m_debugPresentProgram);
		gpu.BindTexture(0, m_resources.indirectTexture);
		if (const rhi::UniformLocation textureLoc = gpu.GetUniformLocation(m_debugPresentProgram, "uTexture"); textureLoc >= 0) {
			gpu.SetUniform(textureLoc, 0);
		}
		if (const rhi::UniformLocation toneMapLoc = gpu.GetUniformLocation(m_debugPresentProgram, "uToneMap"); toneMapLoc >= 0) {
			const bool toneMapDebug =
				m_settings.debugView == SurfelGIDebugView::RawIndirectIrradiance ||
				m_settings.debugView == SurfelGIDebugView::FinalGatherFallback ||
				m_settings.debugView == SurfelGIDebugView::FinalGatherBleed ||
				m_settings.debugView == SurfelGIDebugView::FinalGatherComposite;
			gpu.SetUniform(toneMapLoc, toneMapDebug ? 1 : 0);
		}
		context.screenQuad->Render();
		gpu.BindTexture(0, {});
		gpu.UseProgram({});
		gpu.SetDepthWrite(true);
		EndGpuTiming("debug_overhead", debugBeginQuery, debugEndQuery);
		return;
	}

	if (!m_debugProgram.IsValid() || !m_debugVAO.IsValid()) {
		EndGpuTiming("debug_overhead", debugBeginQuery, debugEndQuery);
		return;
	}

	SurfelRenderStateGuard renderStateGuard;
	BindCoreResources({});

	gpu.BindFramebuffer(PresentTarget::Get());
	gpu.SetViewport(rhi::Rect{ 0, 0, context.width, context.height });
	gpu.SetFeature(rhi::Feature::ProgramPointSize, true);
	gpu.SetBlendEnabled(true);
	gpu.SetBlendFunc(rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::OneMinusSrcAlpha);
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::LessEqual);
	gpu.SetDepthWrite(false);

	gpu.UseProgram(m_debugProgram);
	const SurfelGridSettings& gridSettings = m_grid.GetSettings();
	const glm::vec3 cameraPosition = CameraPositionFromView(context.view);
	glm::vec3 gridMin(0.0f);
	glm::vec3 gridMax(0.0f);
	ComputeCameraCenteredGridBounds(gridSettings, cameraPosition, gridMin, gridMax);
	const bool hasSceneDepth = context.gbufferFBO && context.gbufferFBO->GetDepthTexture().IsValid();
	gpu.BindTexture(kSurfelGIGBufferDepthUnit, hasSceneDepth ? context.gbufferFBO->GetDepthTexture() : rhi::TextureHandle{});
	SetUniformMat4(m_debugProgram, "uView", context.view);
	SetUniformMat4(m_debugProgram, "uProjection", context.proj);
	SetUniform2f(m_debugProgram, "uViewportSize", static_cast<float>(std::max(context.width, 1)), static_cast<float>(std::max(context.height, 1)));
	SetUniform1ui(m_debugProgram, "uMaxSurfels", m_settings.maxSurfels);
	SetUniform1ui(m_debugProgram, "uDebugView", static_cast<uint32_t>(m_settings.debugView));
	SetUniform1ui(m_debugProgram, "uMaxRays", m_settings.maxRayBudget);
	SetUniform1i(m_debugProgram, "uSceneDepthTex", static_cast<int>(kSurfelGIGBufferDepthUnit));
	SetUniform1i(m_debugProgram, "uUseDepthReject", hasSceneDepth ? 1 : 0);
	SetUniform1f(m_debugProgram, "uDepthRejectBias", 0.0015f);
	SetUniform3ui(m_debugProgram, "uGridResolution", gridSettings.resolution.x, gridSettings.resolution.y, gridSettings.resolution.z);
	SetUniform3fv(m_debugProgram, "uGridMin", gridMin);
	SetUniform3fv(m_debugProgram, "uGridMax", gridMax);
	SetSurfelGridUniforms(m_debugProgram, gridSettings, gridMin, gridMax);
	SetUniform1f(m_debugProgram, "uTargetSurfelScreenRadiusPx", m_settings.targetSurfelScreenRadiusPx);

	gpu.BindVertexInput(m_debugVAO);
	gpu.Draw(rhi::Topology::TriangleStrip, 0, 4, m_settings.maxSurfels);
	gpu.BindVertexInput({});
	gpu.UseProgram({});

	gpu.SetDepthWrite(true);
	EndGpuTiming("debug_overhead", debugBeginQuery, debugEndQuery);
}

bool SurfelGIPipeline::ReloadShaders()
{
	ReleaseShaders();
	const bool loaded = !m_settings.enabled || LoadShaders();
	if (loaded && m_pool.IsCreated()) {
		m_pool.ResetFreeList();
		m_frameIndex = 0u;
		m_stationaryFrameCount = 0u;
		m_historyReadIndex = 0u;
		m_lastTextureDumpFrame = 0u;
		m_hasLastCameraState = false;
		m_hasTemporalHistory = false;
		m_wasPlacementValidationMode = false;
	}
	return loaded;
}

void SurfelGIPipeline::SetSettings(const SurfelGISettings& settings)
{
	const SurfelGISettings oldSettings = m_settings;
	const uint32_t oldIndirectWidth = m_indirectWidth;
	const uint32_t oldIndirectHeight = m_indirectHeight;
	m_settings = settings;
	m_stats.configuredEnabled = settings.enabled;
	if (oldSettings.debugView != settings.debugView) {
		m_historyReadIndex = 0u;
		m_hasTemporalHistory = false;
	}
	if (m_initialized && m_settings.enabled && m_width > 0u && m_height > 0u) {
		m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
		m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
		if (m_resources.indirectTexture.IsValid() &&
			(m_indirectWidth != oldIndirectWidth || m_indirectHeight != oldIndirectHeight)) {
			ReleaseIndirectTexture();
			CreateIndirectTexture();
			m_historyReadIndex = 0u;
			m_hasTemporalHistory = false;
		}
	}
}

void SurfelGIPipeline::BeginGpuTiming(const char* label, rhi::QueryHandle& beginQuery, rhi::QueryHandle& endQuery)
{
	rhi::Device& gpu = rhi::Gpu();
	(void)label;
	beginQuery = {};
	endQuery = {};
	if (!m_logSubpassMetrics) {
		return;
	}

	if (m_pendingTimingQueries.size() >= kMaxPendingGpuTimingQueries) {
		CollectReadyGpuTimings(false);
	}
	if (m_pendingTimingQueries.size() >= kMaxPendingGpuTimingQueries) {
		PendingGpuTimingQuery oldest = m_pendingTimingQueries.front();
		gpu.DestroyQuery(oldest.beginQuery);
		gpu.DestroyQuery(oldest.endQuery);
		m_pendingTimingQueries.erase(m_pendingTimingQueries.begin());
	}

	rhi::QueryHandle queries[2];
	queries[0] = gpu.CreateQuery(rhi::QueryType::Timestamp);
	queries[1] = gpu.CreateQuery(rhi::QueryType::Timestamp);
	if (!queries[0].IsValid() || !queries[1].IsValid()) {
		gpu.DestroyQuery(queries[0]);
		gpu.DestroyQuery(queries[1]);
		return;
	}

	beginQuery = queries[0];
	endQuery = queries[1];
	gpu.WriteTimestamp(beginQuery);
}

void SurfelGIPipeline::EndGpuTiming(const char* label, rhi::QueryHandle beginQuery, rhi::QueryHandle endQuery)
{
	rhi::Device& gpu = rhi::Gpu();
	if (!beginQuery.IsValid() || !endQuery.IsValid()) {
		return;
	}

	gpu.WriteTimestamp(endQuery);
	m_pendingTimingQueries.push_back({ label, beginQuery, endQuery, m_frameIndex });
}

void SurfelGIPipeline::CollectReadyGpuTimings(bool forceDelete)
{
	rhi::Device& gpu = rhi::Gpu();
	if (m_pendingTimingQueries.empty()) {
		return;
	}

	std::vector<PendingGpuTimingQuery> pending;
	pending.reserve(m_pendingTimingQueries.size());
	for (const PendingGpuTimingQuery& query : m_pendingTimingQueries) {
		const bool oldEnough = (m_frameIndex - query.issuedFrame) >= kGpuTimingReadbackFrameDelay;
		bool deleteQuery = forceDelete;
		if (!deleteQuery && oldEnough) {
			const bool beginAvailable = gpu.IsQueryResultAvailable(query.beginQuery);
			const bool endAvailable = gpu.IsQueryResultAvailable(query.endQuery);
			if (beginAvailable && endAvailable) {
				uint64_t beginNs = 0u;
				uint64_t endNs = 0u;
				beginNs = gpu.GetQueryResult(query.beginQuery);
				endNs = gpu.GetQueryResult(query.endQuery);
				if (endNs >= beginNs) {
					m_subpassTimings.push_back({ query.label, static_cast<double>(endNs - beginNs) * 1.0e-6 });
				}
				deleteQuery = true;
			}
		}

		if (deleteQuery) {
			rhi::QueryHandle beginQuery = query.beginQuery;
			rhi::QueryHandle endQuery = query.endQuery;
			gpu.DestroyQuery(beginQuery);
			gpu.DestroyQuery(endQuery);
		} else {
			pending.push_back(query);
		}
	}
	m_pendingTimingQueries = std::move(pending);
}

void SurfelGIPipeline::ReleaseGpuTimingQueries()
{
	CollectReadyGpuTimings(true);
	m_pendingTimingQueries.clear();
}

bool SurfelGIPipeline::IsReady() const
{
	return m_initialized &&
		m_resources.surfelBuffer.IsValid() &&
		m_resources.freeListBuffer.IsValid() &&
		m_resources.countersBuffer.IsValid() &&
		m_resources.gridHeaderBuffer.IsValid() &&
		m_resources.gridEntryBuffer.IsValid() &&
		m_resources.gridCellSumBuffer.IsValid() &&
		m_resources.rayRequestBuffer.IsValid() &&
		m_resources.rayBuffer.IsValid() &&
		m_resources.rayHitBuffer.IsValid() &&
		m_resources.irradianceSnapshotBuffer.IsValid() &&
		m_resources.coverageTileBuffer.IsValid() &&
		m_resources.rawIndirectTexture.IsValid() &&
		m_resources.coverageTexture.IsValid() &&
		m_resources.filteredIndirectTexture.IsValid() &&
		m_resources.temporalIndirectTexture.IsValid() &&
		m_resources.indirectTexture.IsValid() &&
		m_resources.historyIndirectTexture[0].IsValid() &&
		m_resources.historyIndirectTexture[1].IsValid() &&
		m_resources.historyGeometryTexture[0].IsValid() &&
		m_resources.historyGeometryTexture[1].IsValid();
}

bool SurfelGIPipeline::CreateAuxiliaryResources()
{
	std::ptrdiff_t radialBytes = 0;
	std::ptrdiff_t guideMapBytes = 0;
	std::ptrdiff_t irradianceSnapshotBytes = 0;
	std::ptrdiff_t coverageTileBytes = 0;
	const uint32_t tileSize = std::max(m_settings.spawnTileSize, 1u);
	m_coverageTileCountX = DivRoundUp(std::max(m_width, 1u), tileSize);
	m_coverageTileCountY = DivRoundUp(std::max(m_height, 1u), tileSize);
	m_coverageTileCapacity = std::max(m_coverageTileCountX * m_coverageTileCountY, 1u);
	if (!CheckedBytes(
			static_cast<uint64_t>(m_settings.maxSurfels) * kRadialDepthTexelsPerSurfel,
			sizeof(float) * 2u,
			radialBytes) ||
		!CheckedBytes(
			static_cast<uint64_t>(m_settings.maxSurfels) * kGuideCellsPerSurfel,
			sizeof(uint32_t),
			guideMapBytes) ||
		!CheckedBytes(m_settings.maxSurfels, sizeof(glm::vec4), irradianceSnapshotBytes) ||
		!CheckedBytes(m_coverageTileCapacity, sizeof(SurfelCoverageTile), coverageTileBytes)) {
		std::cerr << "[SurfelGIPipeline] Auxiliary buffer size overflow.\n";
		return false;
	}

	const bool ok =
		AllocateBuffer(m_resources.dispatchArgsBuffer, 3 * sizeof(uint32_t), "SurfelGI.DispatchArgs") &&
		AllocateBuffer(m_resources.counterStaging[0], sizeof(SurfelCounters), "SurfelGI.CounterStaging0") &&
		AllocateBuffer(m_resources.counterStaging[1], sizeof(SurfelCounters), "SurfelGI.CounterStaging1") &&
		AllocateBuffer(m_resources.radialDepthBuffer, radialBytes, "SurfelGI.RadialDepth") &&
		AllocateBuffer(m_resources.guideMapBuffer, guideMapBytes, "SurfelGI.GuideMap") &&
		AllocateBuffer(m_resources.irradianceSnapshotBuffer, irradianceSnapshotBytes, "SurfelGI.IrradianceSnapshot") &&
		AllocateBuffer(m_resources.coverageTileBuffer, coverageTileBytes, "SurfelGI.CoverageTiles");

	if (!ok) {
		ReleaseAuxiliaryResources();
	}
	else {
		ClearBufferUInt(m_resources.radialDepthBuffer, 0u);
		ClearBufferUInt(m_resources.guideMapBuffer, 0u);
		ClearBufferVec4(m_resources.irradianceSnapshotBuffer, glm::vec4(0.0f));
		ClearBufferUInt(m_resources.coverageTileBuffer, 0u);
	}
	return ok;
}

void SurfelGIPipeline::ReleaseAuxiliaryResources()
{
	DeleteBuffer(m_resources.dispatchArgsBuffer);
	DeleteBuffer(m_resources.counterStaging[0]);
	DeleteBuffer(m_resources.counterStaging[1]);
	m_counterReadbackPending = false;
	DeleteBuffer(m_resources.radialDepthBuffer);
	DeleteBuffer(m_resources.guideMapBuffer);
	DeleteBuffer(m_resources.irradianceSnapshotBuffer);
	DeleteBuffer(m_resources.coverageTileBuffer);
	m_coverageTileCountX = 0;
	m_coverageTileCountY = 0;
	m_coverageTileCapacity = 0;
}

bool SurfelGIPipeline::LoadShaders()
{
	rhi::Device& gpu = rhi::Gpu();
	const bool computeOk = CreateCompute(m_beginFrameShader, "shaders/surfel_gi/begin_frame.comp") &&
		CreateCompute(m_countLiveShader, "shaders/surfel_gi/count_live_surfels.comp") &&
		CreateCompute(m_dispatchArgsShader, "shaders/surfel_gi/dispatch_args.comp") &&
		CreateCompute(m_updateShader, "shaders/surfel_gi/update_surfels.comp") &&
		CreateCompute(m_recycleShader, "shaders/surfel_gi/recycle_surfels.comp") &&
		CreateCompute(m_coverageShader, "shaders/surfel_gi/coverage.comp") &&
		CreateCompute(m_spawnShader, "shaders/surfel_gi/spawn.comp") &&
		CreateCompute(m_gridCountShader, "shaders/surfel_gi/grid_count.comp") &&
		CreateCompute(m_gridScanShader, "shaders/surfel_gi/grid_scan.comp") &&
		CreateCompute(m_buildGridShader, "shaders/surfel_gi/build_grid.comp") &&
		CreateCompute(m_requestRaysShader, "shaders/surfel_gi/request_rays.comp") &&
		CreateCompute(m_allocateRaysShader, "shaders/surfel_gi/allocate_rays.comp") &&
		CreateCompute(m_generateRaysShader, "shaders/surfel_gi/generate_rays.comp") &&
		CreateCompute(m_traceRaysShader, "shaders/surfel_gi/trace_rays.comp") &&
		CreateCompute(m_integrateShader, "shaders/surfel_gi/integrate.comp") &&
		CreateCompute(m_radialDepthShader, "shaders/surfel_gi/radial_depth_update.comp") &&
		CreateCompute(m_irradianceSnapshotShader, "shaders/surfel_gi/irradiance_snapshot.comp") &&
		CreateCompute(m_irradianceSharingShader, "shaders/surfel_gi/irradiance_sharing.comp") &&
		CreateCompute(m_applyIndirectShader, "shaders/surfel_gi/apply_indirect.comp") &&
		CreateCompute(m_spatialFilterShader, "shaders/surfel_gi/spatial_filter.comp") &&
		CreateCompute(m_temporalFilterShader, "shaders/surfel_gi/temporal_filter.comp") &&
		CreateCompute(m_upscaleFilterShader, "shaders/surfel_gi/upscale_filter.comp");

	if (!computeOk) {
		return false;
	}
	// Loaded only in reference mode, so the default configuration neither compiles nor dispatches it.
	if (m_settings.referenceMode && !CreateCompute(m_referenceShader, "shaders/surfel_gi/reference_gi.comp")) {
		return false;
	}

	m_debugProgram = CreateShaderProgram(
		"shaders/surfel_gi/debug_surfels.vert",
		"shaders/surfel_gi/debug_surfels.frag");
	if (!m_debugProgram.IsValid()) {
		std::cerr << "[SurfelGIPipeline] Failed to compile surfel debug shader.\n";
		return false;
	}

	m_debugPresentProgram = CreateShaderProgram(
		"shaders/fullscreen_vert.glsl",
		"shaders/indirect_diffuse_debug_present_frag.glsl");
	if (!m_debugPresentProgram.IsValid()) {
		std::cerr << "[SurfelGIPipeline] Failed to compile surfel fullscreen debug present shader.\n";
		return false;
	}

	if (!m_debugVAO.IsValid()) {
		m_debugVAO = gpu.CreateVertexInput(rhi::VertexInputDesc{});
	}
	return m_debugVAO.IsValid();
}

void SurfelGIPipeline::ReleaseShaders()
{
	rhi::Device& gpu = rhi::Gpu();
	m_beginFrameShader.reset();
	m_countLiveShader.reset();
	m_dispatchArgsShader.reset();
	m_updateShader.reset();
	m_recycleShader.reset();
	m_coverageShader.reset();
	m_spawnShader.reset();
	m_gridCountShader.reset();
	m_gridScanShader.reset();
	m_buildGridShader.reset();
	m_requestRaysShader.reset();
	m_allocateRaysShader.reset();
	m_generateRaysShader.reset();
	m_traceRaysShader.reset();
	m_integrateShader.reset();
	m_radialDepthShader.reset();
	m_irradianceSnapshotShader.reset();
	m_irradianceSharingShader.reset();
	m_applyIndirectShader.reset();
	m_spatialFilterShader.reset();
	m_temporalFilterShader.reset();
	m_upscaleFilterShader.reset();
	m_referenceShader.reset();
	if (m_debugProgram.IsValid()) {
		gpu.DestroyProgram(m_debugProgram);
		m_debugProgram = {};
	}
	if (m_debugPresentProgram.IsValid()) {
		gpu.DestroyProgram(m_debugPresentProgram);
		m_debugPresentProgram = {};
	}
	if (m_debugVAO.IsValid()) {
		gpu.DestroyVertexInput(m_debugVAO);
		m_debugVAO = {};
	}
}

bool SurfelGIPipeline::CreateIndirectTexture()
{
	m_indirectWidth = ScaledExtent(m_width, m_settings.finalGatherResolutionScale);
	m_indirectHeight = ScaledExtent(m_height, m_settings.finalGatherResolutionScale);
	const bool ok =
		AllocateIndirectTexture(m_resources.rawIndirectTexture, m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.coverageTexture, m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.filteredIndirectTexture, m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.temporalIndirectTexture, m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.indirectTexture, m_width, m_height) &&
		AllocateIndirectTexture(m_resources.historyIndirectTexture[0], m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.historyIndirectTexture[1], m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.historyGeometryTexture[0], m_indirectWidth, m_indirectHeight) &&
		AllocateIndirectTexture(m_resources.historyGeometryTexture[1], m_indirectWidth, m_indirectHeight);
	const bool referenceOk = !m_settings.referenceMode ||
		AllocateReferenceTextures(m_resources.referenceMeanTexture, m_resources.referenceCountTexture,
			ReferenceExtent(m_width), ReferenceExtent(m_height));
	if (!ok || !referenceOk) {
		ReleaseIndirectTexture();
		return false;
	}
	m_historyReadIndex = 0u;
	m_hasTemporalHistory = false;
	m_referenceResetPending = true;
	m_hasReferenceView = false;
	return true;
}

void SurfelGIPipeline::ReleaseIndirectTexture()
{
	DeleteTexture(m_resources.rawIndirectTexture);
	DeleteTexture(m_resources.coverageTexture);
	DeleteTexture(m_resources.filteredIndirectTexture);
	DeleteTexture(m_resources.temporalIndirectTexture);
	DeleteTexture(m_resources.indirectTexture);
	DeleteTexture(m_resources.historyIndirectTexture[0]);
	DeleteTexture(m_resources.historyIndirectTexture[1]);
	DeleteTexture(m_resources.historyGeometryTexture[0]);
	DeleteTexture(m_resources.historyGeometryTexture[1]);
	DeleteTexture(m_resources.referenceMeanTexture);
	DeleteTexture(m_resources.referenceCountTexture);
	m_historyReadIndex = 0u;
	m_hasTemporalHistory = false;
}

void SurfelGIPipeline::BindCoreResources(rhi::BufferHandle transformBuffer) const
{
	rhi::Device& gpu = rhi::Gpu();
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Surfels), m_resources.surfelBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::SurfelFreeList), m_resources.freeListBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::SurfelCounters), m_resources.countersBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridHeaders), m_resources.gridHeaderBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridEntries), m_resources.gridEntryBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridCellSums), m_resources.gridCellSumBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RayRequests), m_resources.rayRequestBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Rays), m_resources.rayBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RayHits), m_resources.rayHitBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RayAllocScratch), m_resources.rayAllocScratchBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GridCounters), m_resources.gridCountersBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::RadialDepth), m_resources.radialDepthBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::GuideMap), m_resources.guideMapBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::IrradianceSnapshot), m_resources.irradianceSnapshotBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::CoverageTiles), m_resources.coverageTileBuffer);
	gpu.BindStorageBuffer(ToGLuint(SurfelGIBinding::Transforms), transformBuffer);
}
