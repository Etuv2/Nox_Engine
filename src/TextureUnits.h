#pragma once

/**
 * @file TextureUnits.h
 * @brief Global texture unit assignment system to prevent conflicts across the rendering pipeline
 * 
 * This file defines a standardized texture unit allocation scheme that must be followed
 * across all shaders and rendering passes to prevent texture binding conflicts.
 */

namespace TextureUnits {
    // ======================================
    // GEOMETRY PASS (G-Buffer Generation)
    // ======================================
    // Material textures for mesh rendering
    constexpr int MATERIAL_BASE_COLOR = 0;      // texture_diffuse
    constexpr int MATERIAL_NORMAL = 1;          // texture_normal  
    constexpr int MATERIAL_METALLIC_ROUGHNESS = 2; // texture_metallic_roughness
    constexpr int MATERIAL_EMISSIVE = 3;        // texture_emissive
    constexpr int MATERIAL_OCCLUSION = 4;       // texture_occlusion
    constexpr int MATERIAL_SPECULAR = 5;        // texture_specular (KHR_materials_specular F0 color/factor)
    constexpr int MATERIAL_SPECULAR_COLOR = 6;  // texture_specular_color (KHR_materials_specular)
    constexpr int MATERIAL_TRANSMISSION = 7;    // texture_transmission (KHR_materials_transmission)
    
    // ======================================
    // DEFERRED LIGHTING PASS (SEPARATE FROM GEOMETRY)
    // ======================================
    constexpr int GBUFFER_NORMAL = 8;           // gNormal (oct-encoded in RG)
    constexpr int GBUFFER_ROUGH_METAL = 9;      // gRoughMetal (R=roughness, G=metallic)
    constexpr int GBUFFER_ALBEDO = 10;          // gAlbedo (RGB)
    constexpr int GBUFFER_EMISSIVE = 11;        // gEmissive (RGB)
    constexpr int GBUFFER_DEPTH = 12;           // gDepth
    constexpr int SHADOW_MAP_ARRAY = 13;        // shadowMapArray
    constexpr int ENVIRONMENT_MAP = 14;         // environmentMap
    constexpr int SSAO_MAP = 15;                // ssaoMap
    constexpr int IRRADIANCE_MAP = 16;          // irradianceMap  
    constexpr int PREFILTERED_ENV_MAP = 17;     // prefilteredEnvMap
    constexpr int BRDF_LUT = 18;                // brdfLUT
    constexpr int KEY_LIGHT_SHADOW = 19;        // keyLightShadowMap
    constexpr int RIM_LIGHT_SHADOW = 20;        // rimLightShadowMap
    constexpr int SCREEN_SPACE_SHADOW_MAP = 23; // screenSpaceShadowMap (contact shadows)
    // Extended G-buffer (bound after creation in renderer)
    constexpr int GBUFFER_SPECULAR = 21;        // gSpecularF0 (RGB16F - full color)
    constexpr int GBUFFER_OCCLUSION = 22;       // gOcclusion (R)
    constexpr int GBUFFER_MATERIAL_ID = 28;     // gMaterialID (R8UI - material routing)
    constexpr int GBUFFER_EMISSIVE_COLOR = 29;  // gEmissive color (RGB16F - separate from specular)
    constexpr int GBUFFER_TRANSFORM_ID = 30;    // gTransformID (R32UI - stable surface identity)
    constexpr int GBUFFER_CLEARCOAT = 31;       // gClearCoat (RG16F - factor + roughness)
    constexpr int GBUFFER_PRINCIPLED = 32;      // gPrincipledParams (RGBA16F - transmission/ior + reserved principled slots)
    
    // ======================================
    // LIGHT PROPAGATION VOLUMES (LPV) - GLOBAL ILLUMINATION
    // ======================================
    constexpr int LPV_TEXTURE_R = 24;           // lpvTextureR (3D texture - Red SH coefficient)
    constexpr int LPV_TEXTURE_G = 25;           // lpvTextureG (3D texture - Green SH coefficient)
    constexpr int LPV_TEXTURE_B = 26;           // lpvTextureB (3D texture - Blue SH coefficient)
    
    // ======================================
    // SCREEN SPACE INDIRECT DIFFUSE
    // ======================================
    constexpr int INDIRECT_DIFFUSE_MAP = 27;    // indirectDiffuseMap (RGBA16F - indirect diffuse RGB + AO/visibility A)
    
    // ======================================
    // POST-PROCESSING PASSES
    // ======================================
    constexpr int HDR_COLOR_BUFFER = 0;         // Post-process input
    constexpr int BLOOM_TEXTURE = 1;            // Bloom contribution
    constexpr int SSAO_BLUR = 2;                // Blurred SSAO
    
    // SKYBOX
    constexpr int SKYBOX_CUBEMAP = 0;           // Skybox texture

    // ======================================
    // FORWARD TRANSPARENT PASS (IBL + MATERIALS)
    // ======================================
    // Forward transparent uses same material units as geometry pass (0-7)
    // Plus specialized IBL units to avoid conflicts with deferred pass
    constexpr int FORWARD_IRRADIANCE_MAP = 16;      // Reuse from deferred
    constexpr int FORWARD_PREFILTERED_ENV_MAP = 17; // Reuse from deferred  
    constexpr int FORWARD_BRDF_LUT = 18;            // Reuse from deferred
    
    // VALIDATION
    static_assert(MATERIAL_OCCLUSION < GBUFFER_NORMAL, "Material texture units must not overlap with lighting pass units");
    static_assert(GBUFFER_DEPTH < SHADOW_MAP_ARRAY, "G-buffer texture units must not overlap with shadow units");
    static_assert(BRDF_LUT < KEY_LIGHT_SHADOW, "IBL texture units must not overlap with studio lighting units");
}

#define BIND_TEXTURE_2D(unit, texture) do { glActiveTexture(GL_TEXTURE0 + (unit)); glBindTexture(GL_TEXTURE_2D, (texture)); } while(0)
#define BIND_TEXTURE_CUBE(unit, texture) do { glActiveTexture(GL_TEXTURE0 + (unit)); glBindTexture(GL_TEXTURE_CUBE_MAP, (texture)); } while(0)
#define BIND_TEXTURE_ARRAY(unit, texture) do { glActiveTexture(GL_TEXTURE0 + (unit)); glBindTexture(GL_TEXTURE_2D_ARRAY, (texture)); } while(0)
#define SET_UNIFORM_TEXTURE_UNIT(shader, uniform_name, unit) do { GLint loc = glGetUniformLocation((shader), (uniform_name)); if (loc >= 0) glUniform1i(loc, (unit)); } while(0)
