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
    if (m_interior.roomData) rhi::Gpu().DestroyTexture(m_interior.roomData);
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

// Interior mapping atlas (SceneRenderExtras::interiorAtlas). Sizes come from the atlas data; the
// engine limits only clamp runaway content (with a warning). The array sampler always gets its own
// unit so it never aliases a material sampler2D on unit 0, even when no atlas is bound.
void GBufferPass::UploadInteriorAtlas(const InteriorAtlas& atlas, uint64_t revision) {
    InteriorGpu& g = m_interior;
    if (g.source != &atlas || g.revision != revision || g.layerCount != atlas.faceLayers.size()) {
        g.source = &atlas;
        g.revision = revision;
        g.layerCount = atlas.faceLayers.size();
        g.faces = {};
        g.rooms = 0;
        g.styles = 1;
        g.facesPerRoom = 0;
        for (int& slot : g.roleSlot) slot = -1;

        const int facesPerRoom = atlas.FacesPerRoom();
        int rooms = atlas.RoomCount();
        if (facesPerRoom > InteriorAtlas::kMaxFacesPerRoom) {
            std::cerr << "[GBufferPass] Interior atlas has " << facesPerRoom << " faces per room; limit is "
                      << InteriorAtlas::kMaxFacesPerRoom << ". Interior atlas disabled." << std::endl;
            rooms = 0;
        }
        if (rooms > 0 && atlas.faceLayers.size() != static_cast<size_t>(rooms) * facesPerRoom) {
            std::cerr << "[GBufferPass] Interior atlas: " << atlas.faceLayers.size() << " face layers is not a multiple of "
                      << facesPerRoom << " faces per room; trailing layers ignored." << std::endl;
        }
        if (rooms > InteriorAtlas::kMaxRooms) {
            std::cerr << "[GBufferPass] Interior atlas has " << rooms << " rooms; only the first "
                      << InteriorAtlas::kMaxRooms << " are used." << std::endl;
            rooms = InteriorAtlas::kMaxRooms;
        }
        for (int slot = 0; slot < facesPerRoom && rooms > 0; ++slot) {
            const int role = static_cast<int>(atlas.faceRoles[slot]);
            if (role < 0 || role >= static_cast<int>(InteriorAtlas::FaceRole::Count)) continue;
            if (g.roleSlot[role] >= 0) {
                std::cerr << "[GBufferPass] Interior atlas: face role " << role << " appears twice; slot " << slot << " ignored." << std::endl;
                continue;
            }
            g.roleSlot[role] = slot;
        }

        if (rooms > 0) {
            const size_t layerCount = static_cast<size_t>(rooms) * facesPerRoom;
            if (layerCount == atlas.faceLayers.size()) {
                g.faces = TextureArrayBuilder::GetOrBuild(atlas.faceLayers, atlas.textureOptions);
            }
            else {
                const std::vector<TextureArrayBuilder::Layer> used(atlas.faceLayers.begin(), atlas.faceLayers.begin() + layerCount);
                g.faces = TextureArrayBuilder::GetOrBuild(used, atlas.textureOptions);
            }
        }

        if (g.faces) {
            int styles = static_cast<int>(atlas.styleWeights.size());
            if (styles > InteriorAtlas::kMaxStyles) {
                std::cerr << "[GBufferPass] Interior atlas has " << styles << " style rows; only the first "
                          << InteriorAtlas::kMaxStyles << " are used." << std::endl;
                styles = InteriorAtlas::kMaxStyles;
            }
            if (atlas.faceMean.size() < static_cast<size_t>(rooms) * facesPerRoom) {
                std::cerr << "[GBufferPass] Interior atlas: missing face means; using 0.1." << std::endl;
            }
            const int styleRows = std::max(styles, 1);
            const int rows = 2 + styleRows;
            std::vector<glm::vec4> data(static_cast<size_t>(rooms) * rows, glm::vec4(0.0f));
            auto at = [&](int room, int row) -> glm::vec4& { return data[static_cast<size_t>(row) * rooms + room]; };
            const glm::vec3 lumaW(0.2126f, 0.7152f, 0.0722f);
            // Mean radiance of the room seen through a pane: mostly the back wall.
            const float roleWeight[5] = { 0.4f, 0.15f, 0.15f, 0.15f, 0.15f };
            for (int r = 0; r < rooms; ++r) {
                const glm::vec3 light = r < static_cast<int>(atlas.roomLight.size()) ? atlas.roomLight[r] : glm::vec3(1.0f, 0.0f, 0.0f);
                at(r, 0) = glm::vec4(light, 0.0f);
                glm::vec3 mean(0.0f);
                float weight = 0.0f;
                float backLum = -1.0f;
                for (int slot = 0; slot < facesPerRoom; ++slot) {
                    const size_t layer = static_cast<size_t>(r) * facesPerRoom + slot;
                    const glm::vec3 m = layer < atlas.faceMean.size() ? atlas.faceMean[layer] : glm::vec3(0.1f);
                    const int role = static_cast<int>(atlas.faceRoles[slot]);
                    if (role < 0 || role >= 5 || g.roleSlot[role] != slot) continue;
                    mean += m * roleWeight[role];
                    weight += roleWeight[role];
                    if (role == static_cast<int>(InteriorAtlas::FaceRole::Back)) backLum = glm::dot(m, lumaW);
                }
                mean = weight > 0.0f ? mean / weight : glm::vec3(0.1f);
                at(r, 1) = glm::vec4(mean, backLum >= 0.0f ? backLum : glm::dot(mean, lumaW));
            }
            // Cumulative, normalised variant weights per style (uniform when a row has no weight).
            for (int s = 0; s < styleRows; ++s) {
                std::vector<float> w(static_cast<size_t>(rooms), 0.0f);
                if (s < styles) {
                    const std::vector<float>& row = atlas.styleWeights[s];
                    if (static_cast<int>(row.size()) > rooms) {
                        std::cerr << "[GBufferPass] Interior atlas: style row " << s << " has more weights than rooms; extra ignored." << std::endl;
                    }
                    for (int r = 0; r < rooms && r < static_cast<int>(row.size()); ++r) w[r] = std::max(row[r], 0.0f);
                }
                float total = 0.0f;
                for (float v : w) total += v;
                if (total <= 0.0f) { std::fill(w.begin(), w.end(), 1.0f); total = static_cast<float>(rooms); }
                float acc = 0.0f;
                for (int r = 0; r < rooms; ++r) {
                    acc += w[r] / total;
                    at(r, 2 + s).x = (r == rooms - 1) ? 1.0f : acc;
                }
            }

            rhi::TextureDesc desc;
            desc.type = rhi::TextureType::Tex2D;
            desc.format = rhi::Format::RGBA32F;
            desc.width = rooms;
            desc.height = rows;
            desc.debugName = "interior room data";
            if (g.roomData) rhi::Gpu().DestroyTexture(g.roomData);
            g.roomData = rhi::Gpu().CreateTexture(desc);
            rhi::Gpu().SetTextureFilter(g.roomData, rhi::Filter::Nearest, rhi::Filter::Nearest);
            rhi::Gpu().SetTextureWrap(g.roomData, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge);
            rhi::TextureRegion region;
            region.extent = glm::ivec3(rooms, rows, 1);
            rhi::Gpu().UpdateTexture(g.roomData, region, rhi::PixelFormat::RGBA, rhi::PixelType::Float, data.data());
            g.rooms = rooms;
            g.styles = styleRows;
            g.facesPerRoom = facesPerRoom;
            std::cout << "[GBufferPass] Interior atlas: " << rooms << " rooms x " << facesPerRoom << " faces, "
                      << g.styles << " style rows" << std::endl;
        }
    }

    const bool ready = g.faces.IsValid() && g.roomData.IsValid() && g.rooms > 0;
    SET_UNIFORM_TEXTURE_UNIT(m_shader, "uInteriorAtlas", TextureUnits::INTERIOR_ATLAS);
    SET_UNIFORM_TEXTURE_UNIT(m_shader, "uInteriorRoomData", TextureUnits::INTERIOR_ROOM_DATA);
    BIND_TEXTURE_ARRAY(TextureUnits::INTERIOR_ATLAS, ready ? g.faces : rhi::TextureHandle{});
    BIND_TEXTURE_2D(TextureUnits::INTERIOR_ROOM_DATA, ready ? g.roomData : rhi::TextureHandle{});
    auto loc = [&](const char* name) { return rhi::Gpu().GetUniformLocation(m_shader, name); };
    if (const rhi::UniformLocation l = loc("uInteriorAtlasReady"); l >= 0) rhi::Gpu().SetUniform(l, ready ? 1 : 0);
    if (const rhi::UniformLocation l = loc("uInteriorRoomCount"); l >= 0) rhi::Gpu().SetUniform(l, ready ? g.rooms : 0);
    if (const rhi::UniformLocation l = loc("uInteriorStyleCount"); l >= 0) rhi::Gpu().SetUniform(l, g.styles);
    if (const rhi::UniformLocation l = loc("uInteriorFacesPerRoom"); l >= 0) rhi::Gpu().SetUniform(l, g.facesPerRoom);
    static const char* kRoleSlotNames[5] = { "uInteriorRoleSlot[0]", "uInteriorRoleSlot[1]", "uInteriorRoleSlot[2]",
                                             "uInteriorRoleSlot[3]", "uInteriorRoleSlot[4]" };
    for (int role = 0; role < 5; ++role) {
        if (const rhi::UniformLocation l = loc(kRoleSlotNames[role]); l >= 0) rhi::Gpu().SetUniform(l, ready ? g.roleSlot[role] : -1);
    }
    if (const rhi::UniformLocation l = loc("uInteriorFaceSize"); l >= 0) rhi::Gpu().SetUniform(l, static_cast<float>(std::max(atlas.textureOptions.size, 1)));
    const InteriorAtlas::RoomShape& shape = atlas.roomShape;
    if (const rhi::UniformLocation l = loc("uInteriorRoomShape"); l >= 0)
        rhi::Gpu().SetUniform(l, glm::vec4(shape.sideMargin.x, shape.sideMargin.y, shape.height.x, shape.height.y));
    if (const rhi::UniformLocation l = loc("uInteriorRoomJitter"); l >= 0)
        rhi::Gpu().SetUniform(l, glm::vec3(shape.depthJitter, shape.sillScale.x, shape.sillScale.y));
    if (const rhi::UniformLocation l = loc("uInteriorPaneUV"); l >= 0)
        rhi::Gpu().SetUniform(l, atlas.paneUV.grid >= 1.0f ? glm::vec3(atlas.paneUV.grid, atlas.paneUV.inset, std::max(atlas.paneUV.extent, 1e-4f)) : glm::vec3(0.0f));
    if (const rhi::UniformLocation l = loc("uInteriorFarDistance"); l >= 0) rhi::Gpu().SetUniform(l, std::max(atlas.farDistance, 0.0f));
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

    UploadInteriorAtlas(sceneGraph->renderExtras.interiorAtlas, sceneGraph->renderExtras.revision);

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
