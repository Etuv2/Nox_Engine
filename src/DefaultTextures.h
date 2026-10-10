#pragma once

#include "Texture.h"
#include <memory>

/**
 * @brief Default fallback textures for PBR rendering
 * Refactored to use the new Texture class for better resource management
 */
namespace DefaultTextures {
// Ensure default textures are created (lazy initialization)
    void EnsureCreated();
    
    // Texture handles for direct binding
    // (the textures are owned by TexturePtrs below)
    rhi::TextureHandle White();
    rhi::TextureHandle Black();
    rhi::TextureHandle Normal();
    rhi::TextureHandle AOWhite();
    rhi::TextureHandle MetallicRoughnessDefault();
    
    //Access underlying TexturePtr
    TexturePtr GetWhiteTexture();
    TexturePtr GetBlackTexture();
    TexturePtr GetNormalTexture();
    TexturePtr GetAOWhiteTexture();
    TexturePtr GetMRDefaultTexture();
}