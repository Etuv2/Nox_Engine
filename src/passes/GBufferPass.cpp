#include "GBufferPass.h"
#include "../PresentTarget.h"
#include "PassLogging.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include "../TextureUnits.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>
#include <vector>

#pragma warning(disable: 4996)  // Suppress deprecated function warnings for SceneGraph legacy API

GBufferPass::GBufferPass() {}

GBufferPass::~GBufferPass() {
    if (m_shader) {
        rhi::Gpu().DestroyProgram(m_shader);
        m_shader = {};
    }
}

bool GBufferPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/gbuffer_vert.glsl", 
                                   "shaders/gbuffer_frag.glsl");
    if (!m_shader) {
        std::cerr << "[GBufferPass] Failed to create shader.\n";
        return false;
    }
    m_locView = rhi::Gpu().GetUniformLocation(m_shader, "view");
    m_locProjection = rhi::Gpu().GetUniformLocation(m_shader, "projection");
    std::cout << "[GBufferPass] Initialized successfully.\n";
    return true;
}

void GBufferPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // G-buffer FBO is resized by Renderer coordinator
    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[GBufferPass] Resized to " << newWidth << "x" << newHeight);
}

// Interior mapping atlas (SceneRenderExtras::interiorAtlas). The array sampler always gets its own
// unit so it never aliases a material sampler2D on unit 0, even when no atlas is bound.
void GBufferPass::UploadInteriorAtlas(const InteriorAtlas& atlas) {
    const int rooms = std::min(atlas.RoomCount(), InteriorAtlas::kMaxRooms);
    const rhi::TextureHandle texture = rooms > 0 ? TextureArrayBuilder::GetOrBuild(atlas.faceLayers, atlas.textureOptions) : rhi::TextureHandle{};
    SET_UNIFORM_TEXTURE_UNIT(m_shader, "uInteriorAtlas", TextureUnits::INTERIOR_ATLAS);
    BIND_TEXTURE_ARRAY(TextureUnits::INTERIOR_ATLAS, texture);
    auto loc = [&](const char* name) { return rhi::Gpu().GetUniformLocation(m_shader, name); };
    if (const rhi::UniformLocation l = loc("uInteriorAtlasReady"); l >= 0) rhi::Gpu().SetUniform(l, texture.IsValid() ? 1 : 0);
    if (!texture) return;

    std::vector<glm::vec3> means(static_cast<size_t>(InteriorAtlas::kMaxRooms) * 5, glm::vec3(0.1f));
    std::vector<glm::vec3> light(InteriorAtlas::kMaxRooms, glm::vec3(1.0f, 0.0f, 0.0f));
    std::vector<float> weights(static_cast<size_t>(InteriorAtlas::kMaxStyles) * InteriorAtlas::kMaxRooms, 0.0f);
    for (size_t i = 0; i < atlas.faceMean.size() && i < means.size(); ++i) means[i] = atlas.faceMean[i];
    for (size_t i = 0; i < atlas.roomLight.size() && i < light.size(); ++i) light[i] = atlas.roomLight[i];
    for (size_t s = 0; s < static_cast<size_t>(InteriorAtlas::kMaxStyles); ++s) {
        // Styles without a row fall back to the first row (or uniform when there is none).
        const std::vector<float>* row = s < atlas.styleWeights.size() ? &atlas.styleWeights[s]
            : (!atlas.styleWeights.empty() ? &atlas.styleWeights[0] : nullptr);
        for (int r = 0; r < rooms; ++r) {
            weights[s * InteriorAtlas::kMaxRooms + r] = row && r < static_cast<int>(row->size()) ? (*row)[r] : (row ? 0.0f : 1.0f);
        }
    }
    if (const rhi::UniformLocation l = loc("uInteriorRoomCount"); l >= 0) rhi::Gpu().SetUniform(l, rooms);
    if (const rhi::UniformLocation l = loc("uInteriorFaceSize"); l >= 0) rhi::Gpu().SetUniform(l, static_cast<float>(atlas.textureOptions.size));
    if (const rhi::UniformLocation l = loc("uInteriorFaceMean"); l >= 0) rhi::Gpu().SetUniformArray(l, means.data(), static_cast<int>(means.size()));
    if (const rhi::UniformLocation l = loc("uInteriorRoomLight"); l >= 0) rhi::Gpu().SetUniformArray(l, light.data(), static_cast<int>(light.size()));
    if (const rhi::UniformLocation l = loc("uInteriorStyleWeights"); l >= 0) rhi::Gpu().SetUniformArray(l, weights.data(), static_cast<int>(weights.size()));
}

void GBufferPass::Execute(RenderContext& ctx,
                          const std::shared_ptr<SceneGraph>& sceneGraph,
                          const std::shared_ptr<Camera>& camera,
                          const std::shared_ptr<DirectionalLight>& dirLight,
                          const std::shared_ptr<Skybox>& skybox) {
    if (!sceneGraph || !camera) {
        std::cerr << "[GBufferPass] ERROR: Missing sceneGraph or camera!" << std::endl;
        return;
    }

    if (!ctx.gbufferFBO) {
        std::cerr << "[GBufferPass] ERROR: G-buffer FBO is null!" << std::endl;
        return;
    }

    // Bind G-buffer FBO
    ctx.gbufferFBO->Bind();
    rhi::Gpu().SetViewport({ 0, 0, ctx.width, ctx.height });
    rhi::Gpu().SetDepthTest(true);
    rhi::Gpu().SetBlendEnabled(false);
    rhi::Gpu().Clear(rhi::ClearColor | rhi::ClearDepth);

    // Apply wireframe mode if enabled
    rhi::PolygonMode oldPolygonMode = rhi::PolygonMode::Fill;
    if (ctx.wireframeMode) {
        oldPolygonMode = rhi::Gpu().CaptureState().polygonMode;
        rhi::Gpu().SetPolygonMode(rhi::PolygonMode::Line);
        rhi::Gpu().SetLineWidth(1.0f);
        // Wireframe state is surfaced via UI/profiler context instead of per-frame console logging.
    }

    // Apply force backface culling setting to RenderSystem
    if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
        renderSystem->SetForceBackfaceCulling(ctx.forceBackfaceCulling);
    }

    rhi::Gpu().UseProgram(m_shader);

    // Upload matrices
    if (m_locView >= 0) {
        rhi::Gpu().SetUniform(m_locView, ctx.view);
    }
    if (m_locProjection >= 0) {
        rhi::Gpu().SetUniform(m_locProjection, ctx.proj);
    }

    // Weather state (shaders/includes/weather_common.glsl)
    {
        auto setWeather = [&](const char* name, float v) { const rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_shader, name); if (loc >= 0) rhi::Gpu().SetUniform(loc, v); };
        setWeather("uWeatherWetness", ctx.weatherWetness);
        setWeather("uWeatherPuddles", ctx.weatherPuddles);
        setWeather("uWeatherRain", ctx.weatherRain);
        setWeather("uWeatherPuddleScale", ctx.weatherPuddleScale);
        setWeather("uWeatherTime", ctx.timeSeconds);
    }

    UploadInteriorAtlas(sceneGraph->renderExtras.interiorAtlas);

    // Render scene geometry to G-buffer
    if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
        renderSystem->RenderGeometryBatchedGBuffer(m_shader);
    } else {
        sceneGraph->RenderGeometry(m_shader);
    }

    // Reset force backface culling after geometry rendering to ensure
    // it doesn't affect other passes (skybox, UI, transparent, etc.)
    if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
        renderSystem->SetForceBackfaceCulling(false);
    }

    // Restore default culling state after geometry pass
    rhi::Gpu().SetCullEnabled(true);
    rhi::Gpu().SetCullFace(rhi::CullFace::Back);

    // Restore wireframe state
    if (ctx.wireframeMode) {
        rhi::Gpu().SetPolygonMode(oldPolygonMode);
    }

    // Unbind FBO
    rhi::Gpu().BindFramebuffer(PresentTarget::Get());
}
