#pragma once

#include "../RenderPass.h"
#include "../ComputeShader.h"
#include <GL/glew.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>

class SurfelGIPass : public RenderPass {
public:
    static constexpr uint32_t kMaxSurfels = 262144u;

    struct Stats {
        uint32_t liveCount = 0;
        uint32_t freeCount = 0;
        uint32_t spawnedThisFrame = 0;
        uint32_t recycledThisFrame = 0;
        uint32_t dormantCount = 0;
        uint32_t tileCountX = 0;
        uint32_t tileCountY = 0;
        uint32_t gridCellCount = 0;
    };

    SurfelGIPass();
    ~SurfelGIPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    void RenderDebug(RenderContext& ctx) const;

    GLuint GetSurfelBuffer() const { return m_surfelSSBO; }
    GLuint GetGridHeaderBuffer() const { return m_gridHeaderSSBO; }
    GLuint GetGridEntryBuffer() const { return m_gridEntrySSBO; }
    const Stats& GetLastStats() const { return m_lastStats; }

private:
    struct GpuSurfelRecord {
        glm::vec4 worldPositionRadius{ 0.0f };
        glm::vec4 localPositionAge{ 0.0f };
        glm::vec4 worldNormalRecycle{ 0.0f };
        glm::vec4 localNormalDebug{ 0.0f };
        glm::uvec4 ids{ 0u };
        glm::uvec4 frames{ 0u };
        glm::uvec4 grid{ 0u };
        glm::vec4 metrics{ 0.0f };
    };

    struct GpuPoolHeader {
        glm::uvec4 counts{ 0u };      // x=maxSurfels,y=live,z=free,w=frame
        glm::uvec4 frameStats{ 0u };  // x=spawned,y=recycled,z=dormant,w=flags
        glm::uvec4 tiling{ 0u };      // x=tileCountX,y=tileCountY,z=tileSize,w=gridCellCount
        glm::uvec4 gridDims{ 0u };    // x,y,z dims,w=maxEntriesPerCell
        glm::vec4 gridParams{ 0.0f }; // x=centralExtent,y=nearScale,z=farScale,w=cellWorldSize
    };

    void EnsureResources(RenderContext& ctx);
    void ReleaseBuffers();
    void InitializePool();
    void ReadBackStats();
    void BindCommonBuffers(GLuint transformBuffer) const;

    GLuint m_surfelSSBO = 0;
    GLuint m_headerSSBO = 0;
    GLuint m_freeStackSSBO = 0;
    GLuint m_recycleStackSSBO = 0;
    GLuint m_tileCoverageSSBO = 0;
    GLuint m_gridHeaderSSBO = 0;
    GLuint m_gridEntrySSBO = 0;
    GLuint m_statsReadbackPBO = 0;
    GLuint m_debugVAO = 0;

    std::unique_ptr<ComputeShader> m_initShader;
    std::unique_ptr<ComputeShader> m_lifecycleShader;
    std::unique_ptr<ComputeShader> m_gridClearShader;
    std::unique_ptr<ComputeShader> m_gridBuildShader;
    std::unique_ptr<ComputeShader> m_spawnShader;
    GLuint m_debugProgram = 0;

    uint32_t m_allocatedTileCountX = 0;
    uint32_t m_allocatedTileCountY = 0;
    uint32_t m_allocatedGridCellCount = 0;
    uint32_t m_frameIndex = 0;
    bool m_needsPoolInit = true;

    Stats m_lastStats{};
};
