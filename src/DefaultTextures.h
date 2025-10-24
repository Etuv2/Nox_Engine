#pragma once
#include <GL/glew.h>

namespace DefaultTextures {
    // Initializes all default textures on first use.
    void EnsureCreated();

    // Returns 1x1 white RGBA texture (255,255,255,255)
    GLuint White();
    // Returns 1x1 black RGBA texture (0,0,0,255)
    GLuint Black();
    // Returns 1x1 normal map RGB texture (128,128,255)
    GLuint Normal();
    // Returns 1x1 occlusion texture (R=255)
    GLuint AOWhite();
    // Returns 1x1 metallic-roughness (R unused=255, G=roughness, B=metallic, A=255)
    // Defaults: roughness=204 (~0.8), metallic=0
    GLuint MetallicRoughnessDefault();
}