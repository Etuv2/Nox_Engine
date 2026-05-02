#include "SurfelGIManager.h"

#include "SurfelGIPipeline.h"

namespace {
bool RequiresPipelineReset(const SurfelGISettings& oldSettings, const SurfelGISettings& newSettings)
{
	return oldSettings.maxSurfels != newSettings.maxSurfels ||
		oldSettings.maxRayBudget != newSettings.maxRayBudget ||
		oldSettings.spawnTileSize != newSettings.spawnTileSize ||
		oldSettings.maxSurfelsPerCell != newSettings.maxSurfelsPerCell ||
		oldSettings.useNonLinearGrid != newSettings.useNonLinearGrid ||
		oldSettings.placementValidationMode != newSettings.placementValidationMode;
}

bool IsSurfelOverlayDebugView(SurfelGIDebugView view)
{
	switch (view) {
	case SurfelGIDebugView::SurfelSpheres:
	case SurfelGIDebugView::SurfelNormals:
	case SurfelGIDebugView::SurfelAge:
	case SurfelGIDebugView::SurfelVariance:
	case SurfelGIDebugView::SurfelCoverage:
	case SurfelGIDebugView::CellOccupancy:
	case SurfelGIDebugView::SpawnRecycle:
	case SurfelGIDebugView::RayCounts:
	case SurfelGIDebugView::RayGuide:
	case SurfelGIDebugView::RadialDepth:
	case SurfelGIDebugView::IndirectOnly:
	case SurfelGIDebugView::RecycleScore:
	case SurfelGIDebugView::StaleSurfels:
	case SurfelGIDebugView::RayHitRadiance:
	case SurfelGIDebugView::StoredSurfelWorldPosition:
	case SurfelGIDebugView::StoredSurfelRadius:
	case SurfelGIDebugView::StoredSurfelTransformID:
	case SurfelGIDebugView::StoredSurfelFlags:
	case SurfelGIDebugView::DebugDrawPosition:
	case SurfelGIDebugView::StoredSurfelAlbedo:
	case SurfelGIDebugView::RadiusError:
	case SurfelGIDebugView::GridAxisRegion:
	case SurfelGIDebugView::GridOverflow:
	case SurfelGIDebugView::IrradianceConfidence:
		return true;
	default:
		return false;
	}
}
}

SurfelGIManager::SurfelGIManager()
	: m_pipeline(std::make_unique<SurfelGIPipeline>())
{
}

SurfelGIManager::~SurfelGIManager()
{
	Shutdown();
}

bool SurfelGIManager::Init(RenderContext& context, const SurfelGISettings& settings)
{
	m_settings = settings;
	m_stats = {};
	m_initialized = true;

	if (!m_settings.enabled) {
		return true;
	}

	if (!m_pipeline) {
		m_pipeline = std::make_unique<SurfelGIPipeline>();
	}

	return m_pipeline->Init(context, m_settings);
}

void SurfelGIManager::Resize(RenderContext& context, uint32_t width, uint32_t height)
{
	if (!HasReadyPipeline()) {
		return;
	}

	m_pipeline->Resize(context, width, height);
}

void SurfelGIManager::Shutdown()
{
	if (m_pipeline) {
		m_pipeline->Shutdown();
	}
	m_initialized = false;
	m_stats = {};
}

void SurfelGIManager::BeginFrame(RenderContext& context,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera)
{
	m_stats.configuredEnabled = m_settings.enabled;

	if (!EnsurePipeline(context)) {
		m_stats.ready = false;
		return;
	}

	m_pipeline->BeginFrame(context, sceneGraph, camera);
	m_stats = m_pipeline->GetStats();
}

void SurfelGIManager::Execute(RenderContext& context,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox)
{
	if (!EnsurePipeline(context)) {
		return;
	}

	m_pipeline->Execute(context, sceneGraph, camera, dirLight, skybox);
	m_stats = m_pipeline->GetStats();
}

void SurfelGIManager::ApplyIndirect(RenderContext& context, FrameBuffer& lightingBuffer)
{
	if (!EnsurePipeline(context)) {
		return;
	}

	m_pipeline->ApplyIndirect(context, lightingBuffer);
	m_stats = m_pipeline->GetStats();
}

void SurfelGIManager::RenderDebug(RenderContext& context) const
{
	if (!HasReadyPipeline() || !IsSurfelOverlayDebugView(m_settings.debugView)) {
		return;
	}

	m_pipeline->RenderDebug(context);
}

bool SurfelGIManager::ReloadShaders()
{
	if (!m_initialized) {
		return false;
	}

	if (!m_pipeline) {
		return true;
	}

	return m_pipeline->ReloadShaders();
}

bool SurfelGIManager::IsEnabled() const
{
	return m_initialized && m_settings.enabled && HasReadyPipeline();
}

void SurfelGIManager::SetEnabled(bool enabled)
{
	SurfelGISettings settings = m_settings;
	settings.enabled = enabled;
	SetSettings(settings);
}

void SurfelGIManager::SetSettings(const SurfelGISettings& settings)
{
	const bool resetRequired = RequiresPipelineReset(m_settings, settings);
	m_settings = settings;
	m_stats.configuredEnabled = settings.enabled;

	if (!m_pipeline) {
		return;
	}

	if (!m_settings.enabled) {
		m_pipeline->Shutdown();
		m_stats.ready = false;
		return;
	}

	if (m_pipeline->IsInitialized()) {
		if (resetRequired) {
			m_pipeline->Shutdown();
		} else {
			m_pipeline->SetSettings(m_settings);
		}
	}
}

GLuint SurfelGIManager::GetSurfelBuffer() const
{
	return m_pipeline ? m_pipeline->GetResources().surfelBuffer : 0u;
}

GLuint SurfelGIManager::GetCountersBuffer() const
{
	return m_pipeline ? m_pipeline->GetResources().countersBuffer : 0u;
}

GLuint SurfelGIManager::GetGridHeaderBuffer() const
{
	return m_pipeline ? m_pipeline->GetResources().gridHeaderBuffer : 0u;
}

GLuint SurfelGIManager::GetGridEntryBuffer() const
{
	return m_pipeline ? m_pipeline->GetResources().gridEntryBuffer : 0u;
}

GLuint SurfelGIManager::GetIndirectTexture() const
{
	return m_pipeline ? m_pipeline->GetResources().indirectTexture : 0u;
}

bool SurfelGIManager::HasReadyPipeline() const
{
	return m_initialized && m_settings.enabled && m_pipeline && m_pipeline->IsReady();
}

bool SurfelGIManager::EnsurePipeline(RenderContext& context)
{
	if (!m_initialized || !m_settings.enabled) {
		return false;
	}

	if (!m_pipeline) {
		m_pipeline = std::make_unique<SurfelGIPipeline>();
	}

	if (!m_pipeline->IsInitialized()) {
		return m_pipeline->Init(context, m_settings);
	}

	return m_pipeline->IsReady();
}
