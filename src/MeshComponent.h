#pragma once
#include <GL/glew.h>
#include "Vertex.h"
#include "Texture.h"
#include <vector>
#include <memory>

class MeshComponent {
public:
    GLuint VAO;
    GLuint VBO;
    GLuint EBO;
    size_t indexCount;
 
    // glTF 2.0 standard texture assignments (using new Texture system)
    std::shared_ptr<Texture> diffuseTexture;        // Base color (albedo) texture
    std::shared_ptr<Texture> normalTexture;         // Normal map
    std::shared_ptr<Texture> roughnessTexture;      // Metallic-Roughness texture (G=roughness, B=metallic)
    std::shared_ptr<Texture> emissiveTexture;       // Emissive texture
    std::shared_ptr<Texture> occlusionTexture;      // Ambient occlusion texture (R channel)
    std::shared_ptr<Texture> specularTexture;       // Specular texture (for KHR_materials_specular extension)

    // Material properties
    bool hasAlpha;                // Requires alpha blending
    bool doubleSided;             // Disable backface culling (from glTF material)
    
    // Animation and morphing
    std::vector<GLuint> morphVBOs; // One VBO per morph target
    int morphTargetCount = 0;

    // glTF 2.0 standard material factors
    glm::vec4 baseColorFactor = glm::vec4(1.0f);     // RGBA base color multiplier
	float metallicFactor = 0.0f;                     // Metallic factor [0,1] - FIXED: was 1.0, now 0.0 per glTF spec
	float roughnessFactor = 1.0f;                    // Roughness factor [0,1] - FIXED: was 0.0, now 1.0 per glTF spec
    float alphaCutoff = 0.5f;                        // Alpha cutoff for alpha testing
    glm::vec3 emissiveFactor = glm::vec3(0.0f);      // Emissive color multiplier
    
    // Additional material factors for extensions
    glm::vec3 specularFactor = glm::vec3(0.0f);      // KHR_materials_specular
    glm::vec3 specularColorFactor = glm::vec3(1.0f); // KHR_materials_specular
    float occlusionStrength = 1.0f;                  // Occlusion strength
    float normalScale = 1.0f;                        // Normal map intensity
    
    // Alpha mode enumeration following glTF spec
    enum AlphaMode {
        ALPHA_OPAQUE = 0,     // No alpha processing
        ALPHA_MASK = 1,       // Alpha testing with cutoff
        ALPHA_BLEND = 2       // Alpha blending
    };
    AlphaMode alphaMode = ALPHA_OPAQUE;

    // Culling mode enumeration for better control
    enum CullingMode {
        CULL_BACK = 0,        // Standard back-face culling
        CULL_FRONT = 1,       // Front-face culling
        CULL_NONE = 2,        // No culling (double-sided)
        CULL_DEFAULT = 3      // Use node/material default
    };
    CullingMode cullingMode = CULL_DEFAULT;

    // Raw mesh data for CPU operations
    std::vector<Vertex> rawVertices;
    std::vector<uint32_t> rawIndices;

    MeshComponent()
        : VAO(0), VBO(0), EBO(0),
        indexCount(0),
        diffuseTexture(nullptr), normalTexture(nullptr),
        roughnessTexture(nullptr), emissiveTexture(nullptr),
        occlusionTexture(nullptr), specularTexture(nullptr),
        hasAlpha(false), doubleSided(false) {}

    // Determine if this mesh needs special rendering treatment
    bool RequiresAlphaBlending() const {
        return alphaMode == ALPHA_BLEND || hasAlpha;
    }
    
    bool RequiresAlphaTesting() const {
        return alphaMode == ALPHA_MASK;
    }
    
    bool IsOpaque() const {
        return alphaMode == ALPHA_OPAQUE && !hasAlpha;
    }
    
    // Get effective culling mode considering both mesh and material settings
    CullingMode GetEffectiveCullingMode() const {
        if (cullingMode != CULL_DEFAULT) {
            return cullingMode;
        }
        // Fall back to material double-sided property
        return doubleSided ? CULL_NONE : CULL_BACK;
    }

    void Cleanup() {
        if (VAO) glDeleteVertexArrays(1, &VAO);
        if (VBO) glDeleteBuffers(1, &VBO);
        if (EBO) glDeleteBuffers(1, &EBO);
        
    // Textures will be automatically cleaned up by shared_ptr destructors
        diffuseTexture.reset();
        normalTexture.reset();
    roughnessTexture.reset();
        emissiveTexture.reset();
     occlusionTexture.reset();
        specularTexture.reset();
    
 for (GLuint vbo : morphVBOs) {
          if (vbo) glDeleteBuffers(1, &vbo);
        }
        
    // Reset all handles
    VAO = VBO = EBO = 0;
        morphVBOs.clear();
    }
};
