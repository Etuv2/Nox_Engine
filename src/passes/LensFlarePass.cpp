#include "LensFlarePass.h"
#include "ExposurePass.h"
#include "../PresentTarget.h"
#include "../ComputeShader.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <glm/gtc/type_ptr.hpp>

namespace {
constexpr uint32_t kSourceBinding = 45;      // shaders/includes/lens_flare_common.glsl
constexpr uint32_t kVisibilityBinding = 46;
constexpr uint32_t kStyleBinding = 47;

struct GpuFlareSource {
	glm::vec4 posStyle;
	glm::vec4 facingLuminance;
	glm::vec4 tintSize;
};
struct GpuFlareLayer {
	glm::vec4 layerGainWidthGlareMin;
	glm::vec4 tintGlareWeight;
	glm::vec4 flags;
};
static_assert(sizeof(GpuFlareSource) == 3 * sizeof(glm::vec4), "SSBO layout");
static_assert(sizeof(GpuFlareLayer) == 3 * sizeof(glm::vec4), "SSBO layout");
}

LensFlarePass::LensFlarePass() = default;

LensFlarePass::~LensFlarePass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	if (m_drawShader) gpu.DestroyProgram(m_drawShader);
	if (m_sourceSSBO) gpu.DestroyBuffer(m_sourceSSBO);
	if (m_visibilitySSBO) gpu.DestroyBuffer(m_visibilitySSBO);
	if (m_styleSSBO) gpu.DestroyBuffer(m_styleSSBO);
	if (m_vao) gpu.DestroyVertexInput(m_vao);
}

bool LensFlarePass::Initialize(RenderContext& context)
{
	m_drawShader = CreateShaderProgram("shaders/lens_flare_vert.glsl", "shaders/lens_flare_frag.glsl");
	m_visibilityShader = std::make_unique<ComputeShader>();
	if (!m_drawShader || !m_visibilityShader->CreateFromFile("shaders/lens_flare_visibility_comp.glsl")) {
		std::cerr << "[LensFlarePass] Failed to create lens flare shaders" << std::endl;
		return false;
	}
	rhi::Device& gpu = rhi::Gpu();
	m_vao = gpu.CreateVertexInput(rhi::VertexInputDesc{});   // attribute-less: corners come from gl_VertexID
	m_sourceSSBO = gpu.CreateBuffer(rhi::BufferDesc{});
	m_visibilitySSBO = gpu.CreateBuffer(rhi::BufferDesc{});
	m_styleSSBO = gpu.CreateBuffer(rhi::BufferDesc{});
	CreateTarget(context.width, context.height);
	return true;
}

void LensFlarePass::CreateTarget(int width, int height)
{
	m_output = std::make_unique<FrameBuffer>(std::max(width, 1), std::max(height, 1),
		std::vector<rhi::Format>{ rhi::Format::RGBA16F }, false, false);
}

void LensFlarePass::Resize(RenderContext&, int newWidth, int newHeight)
{
	CreateTarget(newWidth, newHeight);
}

void LensFlarePass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (!sceneGraph || !ctx.gbufferFBO || !m_output) return;
	const SceneRenderExtras& extras = sceneGraph->renderExtras;
	const LensFlareSet& flares = extras.lensFlares;
	if (flares.sources.empty() || flares.styles.empty() || flares.gain <= 0.0f) return;
	const rhi::TextureHandle sprites = TextureArrayBuilder::GetOrBuild(flares.layers, flares.textureOptions);
	if (!sprites) return;

	rhi::Device& gpu = rhi::Gpu();
	if (extras.revision != m_revision) {
		std::vector<GpuFlareSource> sources;
		sources.reserve(flares.sources.size());
		for (const LensFlareSource& s : flares.sources) {
			const float facingLen = glm::length(s.facing);
			const glm::vec3 facing = facingLen > 1e-3f ? s.facing / facingLen : glm::vec3(0.0f);
			sources.push_back({ glm::vec4(s.position, static_cast<float>(s.style)), glm::vec4(facing, s.luminance),
				glm::vec4(s.tint, s.size) });
		}
		std::vector<GpuFlareLayer> layers(flares.styles.size() * LensFlareStyle::kMaxLayers, GpuFlareLayer{});
		for (size_t st = 0; st < flares.styles.size(); ++st) {
			const auto& styleLayers = flares.styles[st].layers;
			for (size_t l = 0; l < styleLayers.size() && l < static_cast<size_t>(LensFlareStyle::kMaxLayers); ++l) {
				const LensFlareLayer& L = styleLayers[l];
				layers[st * LensFlareStyle::kMaxLayers + l] = {
					glm::vec4(static_cast<float>(L.textureLayer), L.gain, L.width, L.glareMin),
					glm::vec4(L.tint, L.glareWeight),
					glm::vec4(L.useSourceTint ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f) };
			}
		}
		gpu.ReallocateBuffer(m_sourceSSBO, sources.size() * sizeof(GpuFlareSource), sources.data(), rhi::BufferUsage::StaticDraw);
		gpu.ReallocateBuffer(m_styleSSBO, layers.size() * sizeof(GpuFlareLayer), layers.data(), rhi::BufferUsage::StaticDraw);
		const std::vector<float> zero(sources.size(), 0.0f);
		gpu.ReallocateBuffer(m_visibilitySSBO, zero.size() * sizeof(float), zero.data(), rhi::BufferUsage::DynamicCopy);
		m_count = static_cast<int>(sources.size());
		m_styleCount = static_cast<int>(flares.styles.size());
		m_revision = extras.revision;
		m_resetVisibility = true;
		std::cout << "[LensFlarePass] Uploaded " << m_count << " sources, " << m_styleCount << " styles" << std::endl;
	}

	// Unjittered projection for the sprites (drawn on the TAA-resolved image); the jittered one for the
	// depth test, which must match this frame's depth buffer.
	glm::mat4 projNoJitter = ctx.proj;
	projNoJitter[2][0] -= ctx.jitterNdc.x;
	projNoJitter[2][1] -= ctx.jitterNdc.y;
	const glm::vec3 camPos = glm::vec3(glm::inverse(ctx.view)[3]);
	const glm::vec2 screen(static_cast<float>(ctx.width), static_cast<float>(ctx.height));

	gpu.BindStorageBuffer(kSourceBinding, m_sourceSSBO);
	gpu.BindStorageBuffer(kVisibilityBinding, m_visibilitySSBO);
	gpu.BindStorageBuffer(kStyleBinding, m_styleSSBO);

	// 1. Visibility.
	{
		const rhi::ProgramHandle p = m_visibilityShader->GetProgramID();
		gpu.UseProgram(p);
		gpu.SetUniform(gpu.GetUniformLocation(p, "uCount"), static_cast<uint32_t>(m_count));
		gpu.SetUniform(gpu.GetUniformLocation(p, "uViewProj"), ctx.proj * ctx.view);
		gpu.SetUniform(gpu.GetUniformLocation(p, "uProj"), ctx.proj);
		gpu.SetUniform(gpu.GetUniformLocation(p, "uScreenSize"), screen);
		const float dt = std::clamp(ctx.deltaTime, 0.0f, 0.1f);
		gpu.SetUniform(gpu.GetUniformLocation(p, "uBlend"), 1.0f - std::exp(-dt * std::max(flares.visibilityRate, 0.0f)));
		gpu.SetUniform(gpu.GetUniformLocation(p, "uReset"), m_resetVisibility ? 1 : 0);
		gpu.BindTexture(0, ctx.gbufferFBO->GetDepthTexture());
		gpu.SetUniform(gpu.GetUniformLocation(p, "uDepth"), 0);
		gpu.Dispatch((static_cast<uint32_t>(m_count) + 63u) / 64u, 1, 1);
		gpu.PipelineBarrier(rhi::BarrierStorage);
		gpu.BindTexture(0, rhi::TextureHandle());
		m_resetVisibility = false;

		// NOX_FLARE_DEBUG=1: log the smoothed visibility of the sources nearest the camera every 120 frames.
		static const bool debug = [] { const char* v = std::getenv("NOX_FLARE_DEBUG"); return v && v[0] == '1'; }();
		static int frame = 0;
		if (debug && (++frame % 120) == 0) {
			std::vector<float> vis(static_cast<size_t>(m_count));
			gpu.ReadBuffer(m_visibilitySSBO, 0, vis.size() * sizeof(float), vis.data());
			std::vector<size_t> order(vis.size());
			for (size_t i = 0; i < order.size(); ++i) order[i] = i;
			auto distTo = [&](size_t i) { return glm::length(flares.sources[i].position - camPos); };
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distTo(a) < distTo(b); });
			std::cout << "[LensFlarePass] nearest sources (dist m: visibility):";
			for (size_t k = 0; k < std::min<size_t>(order.size(), 6); ++k) std::cout << " " << distTo(order[k]) << ":" << vis[order[k]];
			std::cout << std::endl;
		}
	}

	// 2. Copy the post source (TAA / motion blur output) and add the sprites on top.
	FrameBuffer* source = ctx.postSourceFBO ? ctx.postSourceFBO
		: ((ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : ctx.hdrFBO.get());
	if (!source) return;
	if (m_output->GetWidth() != ctx.width || m_output->GetHeight() != ctx.height) {
		CreateTarget(ctx.width, ctx.height);
	}
	const rhi::Rect copyRect{ 0, 0, ctx.width, ctx.height };
	gpu.BlitFramebuffer(source->GetFBO(), 0, copyRect, m_output->GetFBO(), copyRect, rhi::ClearColor, rhi::BlitFilter::Nearest);

	m_output->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetDepthTest(false);
	gpu.SetDepthWrite(false);
	gpu.SetCullEnabled(false);
	gpu.SetBlendEnabled(true);
	gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::One);

	const float resScale = screen.y / std::max(flares.referenceHeight, 1.0f);
	gpu.UseProgram(m_drawShader);
	ExposurePass::Bind(ctx);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uViewProj"), projNoJitter * ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uProj"), projNoJitter);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uCameraPos"), camPos);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uScreenSize"), screen);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uMinPixels"), flares.minPixels * resScale);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uMaxPixels"), flares.maxPixels * resScale);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uGain"), flares.luminanceToGlare * flares.gain);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uFadeStart"), flares.fadeStart);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uFadeEnd"), flares.fadeEnd);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uStyleCount"), m_styleCount);
	gpu.BindTexture(0, sprites);
	gpu.SetUniform(gpu.GetUniformLocation(m_drawShader, "uFlares"), 0);

	gpu.BindVertexInput(m_vao);
	gpu.Draw(rhi::Topology::TriangleStrip, 0, 4, static_cast<uint32_t>(m_count * LensFlareStyle::kMaxLayers));
	gpu.BindVertexInput(rhi::VertexInputHandle());

	gpu.BindTexture(0, rhi::TextureHandle());
	gpu.SetBlendEnabled(false);
	gpu.SetDepthWrite(true);
	gpu.SetDepthTest(true);
	gpu.SetCullEnabled(true);
	gpu.BindFramebuffer(PresentTarget::Get());

	ctx.postSourceFBO = m_output.get();
}
