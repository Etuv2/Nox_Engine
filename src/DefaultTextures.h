#pragma once

#include <GL/glew.h>
#include "Texture.h"
#include <memory>

/**
 * @brief Default fallback textures for PBR rendering
 * Refactored to use the new Texture class for better resource management
 */
namespace DefaultTextures {
// Ensure default textures are created (lazy initialization)
    void EnsureCreated();
    
    // REFACTORED: Return raw GLuint for backward compatibility
    // but internally use TexturePtr for resource management
    GLuint White();
    GLuint Black();
    GLuint Normal();
    GLuint AOWhite();
    GLuint MetallicRoughnessDefault();
    
    //Access underlying TexturePtr
    TexturePtr GetWhiteTexture();
    TexturePtr GetBlackTexture();
    TexturePtr GetNormalTexture();
    TexturePtr GetAOWhiteTexture();
    TexturePtr GetMRDefaultTexture();
}