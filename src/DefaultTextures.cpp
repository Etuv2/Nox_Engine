#include "DefaultTextures.h"
#include <array>
#include <iostream>
#include <GL/glew.h>

namespace {
    // Static flag to prevent recreation every frame
    static bool g_texturesCreated = false;
    
    GLuint g_white = 0;
    GLuint g_black = 0;
    GLuint g_normal = 0;
    GLuint g_aowhite = 0;
    GLuint g_mrDefault = 0;

    GLuint Create1x1(const std::array<unsigned char,4>& rgba, GLenum internalFormat = GL_RGBA8, GLenum format = GL_RGBA) {
        //Save current OpenGL state to prevent conflicts
        GLint lastTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
        
        GLuint tex = 0; 
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, 1, 1, 0, format, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        
        //Restore previous texture binding
        glBindTexture(GL_TEXTURE_2D, lastTexture);
        
        // Check for errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "[DefaultTextures] OpenGL error creating texture: " << error << std::endl;
            glDeleteTextures(1, &tex);
            return 0;
        }
        
        return tex;
    }
}

namespace DefaultTextures {
    void EnsureCreated() {
        // Only create textures once
        if (g_texturesCreated) {
            return;
        }
        
        //Save current OpenGL state
        GLint lastTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
        
        std::cout << "[DefaultTextures] Creating default textures (one-time initialization)..." << std::endl;
        
        g_white = Create1x1({255,255,255,255});
        g_black = Create1x1({0,0,0,255});
        g_normal = Create1x1({128,128,255,255}); // Standard normal map (0,0,1) encoded as (128,128,255)
        g_aowhite = Create1x1({255,255,255,255});
        g_mrDefault = Create1x1({255,204,0,255}); // R unused, G roughness ~0.8, B metallic 0
        
        //Restore previous texture binding
        glBindTexture(GL_TEXTURE_2D, lastTexture);
        
        // Validate all textures were created successfully
        if (!glIsTexture(g_white) || !glIsTexture(g_black) || !glIsTexture(g_normal) || 
            !glIsTexture(g_aowhite) || !glIsTexture(g_mrDefault)) {
            std::cerr << "[DefaultTextures]Some default textures failed to create!" << std::endl;
            return;
        }
        
        g_texturesCreated = true;
        std::cout << "[DefaultTextures] All default textures created successfully!" << std::endl;
        std::cout << "[DefaultTextures] White: " << g_white << ", Black: " << g_black 
                  << ", Normal: " << g_normal << ", AOWhite: " << g_aowhite 
                  << ", MR: " << g_mrDefault << std::endl;
    }
    
    GLuint White(){ EnsureCreated(); return g_white; }
    GLuint Black(){ EnsureCreated(); return g_black; }
    GLuint Normal(){ EnsureCreated(); return g_normal; }
    GLuint AOWhite(){ EnsureCreated(); return g_aowhite; }
    GLuint MetallicRoughnessDefault(){ EnsureCreated(); return g_mrDefault; }
}
