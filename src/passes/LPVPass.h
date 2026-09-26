#pragma once

#include "../RenderPass.h"
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <vector>

class FrameBuffer;
class DirectionalLight;

/**
 * @brief Light Propagation Volume (LPV) Global Illumination Pass
 * 
 * Implements Frostbite-style dynamic global illumination using:
 * - Reflective Shadow Maps (RSM) for VPL generation
 * - 3D grid of Spherical Harmonics coefficients for light storage
 * - Iterative light propagation for indirect diffuse bounces
 * - Geometry Volume for occlusion-aware propagation
 * 
 * Pipeline:
 * 1. Render RSM from directional light (position, normal, flux)
 * 2. Inject VPLs into LPV grid as SH coefficients
 * 3. Propagate light through grid (4-6 iterations)
 * 4. Sample LPV in deferred lighting for GI contribution
 */
class LPVPass : public RenderPass
{
public:
    LPVPass();
    ~LPVPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                const std::shared_ptr<SceneGraph>& sceneGraph,
                const std::shared_ptr<Camera>& camera,
                const std::shared_ptr<DirectionalLight>& dirLight,
                const std::shared_ptr<Skybox>& skybox) override;

    // Configuration
    struct Config {
        // LPV grid settings
        int gridResolution = 128;           // 128^3 voxels
        float voxelSize = 0.5f;             // World-space size per voxel
        glm::vec3 gridCenter = glm::vec3(0.0f); // World-space center (usually camera position)
        glm::quat gridOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Grid orientation
        
        // RSM settings
        int rsmResolution = 512;            // 512x512 RSM
        int vplSampleCount = 32000;         // Number of VPLs to inject
        
        // Propagation settings
        int propagationIterations = 8;      // Propagation steps; light travels one cell per step
        
        // Rendering settings
        float giStrength = 1.0f;            // GI contribution multiplier
        bool enableOcclusion = true;        // Use geometry volume for blocking
        bool enableDebugVisualization = false;
        bool useAllLights = true;           // Use all lights for RSM (not just directional)
        int maxRSMLights = 3;               // Maximum number of lights to use for RSM
        
        // Performance settings
        bool enableLPV = true;              // Master switch
        bool useAtomicInjection = true;     // Use atomic operations if available
        int updateFrequency = 1;            // Update every N frames (1 = every frame)
    } config;

    // Accumulated LPV (radiant intensity SH per cell) for debug tooling and legacy inspectors.
    GLuint GetLPVTextureR() const { return m_lpvAccumTextures[0]; }
    GLuint GetLPVTextureG() const { return m_lpvAccumTextures[1]; }
    GLuint GetLPVTextureB() const { return m_lpvAccumTextures[2]; }
    GLuint GetGeometryVolume() const { return m_geometryVolume; }
    GLuint GetResolvedIndirectTexture() const { return m_resolvedIndirectTexture; }

    // Debug visualization
    void RenderDebugVisualization(const glm::mat4& view, const glm::mat4& projection);

private:
    // Initialization helpers
    bool CreateLPVResources();
    bool CreateRSMResources();
    bool CreateShaders();
    void CleanupResources();

    // Recreate helpers when resolution changes at runtime
    void DestroyLPVResourcesOnly();
    void DestroyRSMResourcesOnly();
    bool EnsureResolvedIndirectTexture(int width, int height);

    // The directional light that direct lighting uses (first enabled one in the light manager).
    bool FindInjectedLight(const RenderContext& ctx,
                           const std::shared_ptr<DirectionalLight>& fallback,
                           glm::vec3& direction,
                           glm::vec3& irradiance) const;

    // Rendering stages
    void RenderRSM(const std::shared_ptr<SceneGraph>& sceneGraph,
                   const glm::vec3& lightDirection,
                   const glm::vec3& lightIrradiance);

    void InjectVPLs(const glm::vec3& lightIrradiance);
    void PropagateLPV();
    void ResolveIndirect(RenderContext& ctx);
    void VoxelizeGeometry(const std::shared_ptr<SceneGraph>& sceneGraph);

    // Utility
    glm::vec3 WorldToVoxel(const glm::vec3& worldPos) const;
    glm::vec3 VoxelToWorld(const glm::ivec3& voxelPos) const;

    // Injection accumulators: signed fixed-point SH (R32I, 4x wider in X, one block per coefficient)
    GLuint m_lpvTextures[3] = {0, 0, 0};

    // Propagation waves (ping-pong) and their running sum, RGBA16F = 4 SH coefficients per cell
    GLuint m_lpvSampleTextures[3] = {0, 0, 0};
    GLuint m_lpvSampleTexturesTemp[3] = {0, 0, 0};
    GLuint m_lpvAccumTextures[3] = {0, 0, 0};
    float m_fixedPointScale = 1.0f;

    GLuint m_geometryVolume = 0;                 // Directional occlusion bits (R32UI)
    GLuint m_resolvedIndirectTexture = 0;         // Full-screen RGBA16F irradiance + visibility
    
    // RSM framebuffer and textures
    std::unique_ptr<FrameBuffer> m_rsmFBO;
    GLuint m_rsmPosition = 0;       // World-space position (RGB16F)
    GLuint m_rsmNormal = 0;         // World-space normal (RGB16F)
    GLuint m_rsmFlux = 0;           // Reflected flux per unit light-perpendicular area (RGB16F)
    GLuint m_rsmDepth = 0;          // Depth buffer
    
    // Shaders
    GLuint m_rsmShader = 0;          // RSM generation shader
    GLuint m_injectionShader = 0;    // VPL injection compute shader
    GLuint m_propagationShader = 0;  // Light propagation compute shader
    GLuint m_convertShader = 0;      // Convert R32UI extended grid -> RGBA16F per-voxel textures
    GLuint m_resolveShader = 0;      // Resolve LPV SH volumes into generic screen-space indirect
    GLuint m_voxelizeShader = 0;     // Geometry voxelization shader
    GLuint m_debugShader = 0;        // Debug visualization shader
    
    // State tracking
    int m_frameCounter = 0;
    bool m_geometryDirty = true;     // Needs re-voxelization
    glm::vec3 m_lastGridCenter = glm::vec3(0.0f);

    // Track allocated resolutions to recreate textures when user changes settings
    int m_allocatedGridResolution = 0;  // base resolution used to allocate LPV/geometry textures
    int m_allocatedRSMResolution = 0;   // resolution used to allocate RSM resources
    int m_resolvedIndirectWidth = 0;
    int m_resolvedIndirectHeight = 0;
};
