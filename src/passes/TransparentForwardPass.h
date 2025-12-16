#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

/**
 * @brief TransparentForwardPass renders transparent objects with physically correct glass-like materials.
 * 
 * Key Features:
 * - Automatic detection of transparent nodes based on mesh alpha properties
 * - Depth-aware alpha blending (depth test enabled, depth writes disabled)
 * - Physically correct Fresnel reflections for glass
 * - Environment-based refraction with proper IOR
 * - Back-to-front sorting for correct transparency
 * - IBL integration for realistic reflections
 * - Linear color space blending (gamma correction in post-processing)
 * 
 * Transparency Detection:
 * - Scans entire scene hierarchy recursively
 * - Checks each model's meshes for RequiresAlphaBlending()
 * - Detects materials with:
 *   - alphaMode == ALPHA_BLEND
 *   - hasAlpha == true
 *   - baseColorFactor.a < 1.0
 * 
 * Rendering Pipeline Position:
 * This pass MUST execute AFTER the skybox pass to ensure correct depth ordering:
 * 1. Deferred lighting renders opaque geometry to HDR FBO
 * 2. Skybox renders at maximum depth (z=w, GL_LEQUAL)
 * 3. **Transparent pass renders here** - tests against skybox depth
 * 4. Post-processing (bloom, TAA, tonemapping)
 * 
 * Rendering State:
 * 1. Assumes HDR FBO is already bound (from skybox rendering)
 * 2. Enables depth testing (GL_LESS) to respect opaque geometry + skybox
 * 3. Disables depth writes (glDepthMask(GL_FALSE)) for transparent layering
 * 4. Uses standard alpha blending (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
 * 5. Collects transparent nodes from scene graph
 * 6. Sorts transparent objects back-to-front based on distance to camera
 * 7. Renders with full PBR lighting and IBL reflections
 * 8. Restores depth writes and disables blending
 * 9. Keeps HDR FBO bound for subsequent passes
 * 
 * Material Properties:
 * - transmissionFactor: Controls transparency (0.9 = 90% transparent glass)
 * - refractionIndex: Index of refraction (1.5 for standard glass)
 * - Fresnel effects: Increased reflection at grazing angles
 * - Base color tinting: For colored glass materials
 * 
 * Supported Alpha Modes:
 * - ALPHA_BLEND: Full alpha blending for smooth transparency
 * - Materials with hasAlpha flag set
 * - Materials with alpha in baseColorFactor
 * 
 * This pass does NOT clear or rebind framebuffers. It operates on the
 * existing HDR FBO content with depth buffer from previous passes.
 * 
 * Note: Requires valid IBL textures from skybox for realistic reflections.
 */
class TransparentForwardPass : public RenderPass {
public:
    TransparentForwardPass();
    ~TransparentForwardPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    GLuint m_shader = 0; // Forward PBR shader for transparent materials
};
