#include "DefaultTextures.h"
#include <iostream>

namespace {
	// Static flag to prevent recreation every frame
	static bool g_texturesCreated = false;

	// REFACTORED: Use TexturePtr for automatic resource management
	static TexturePtr g_white;
	static TexturePtr g_black;
	static TexturePtr g_normal;
	static TexturePtr g_aowhite;
	static TexturePtr g_mrDefault;
}

namespace DefaultTextures {
	void EnsureCreated() {
		// Only create textures once
		if (g_texturesCreated) {
			return;
		}

		std::cout << "[DefaultTextures] Creating default textures (one-time initialization)..." << std::endl;

		// REFACTORED: Use TextureFactory for clean, declarative 1x1 texture creation
		g_white = TextureFactory::CreateSolidColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), GL_RGBA8);
		g_black = TextureFactory::CreateSolidColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), GL_RGBA8);

		// Standard normal map (0,0,1) encoded as (128,128,255)
		g_normal = TextureFactory::CreateSolidColor(glm::vec4(128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f), GL_RGB8);

		// White AO (no occlusion)
		g_aowhite = TextureFactory::CreateSolidColor(glm::vec4(1.0f), GL_RGBA8);

		// Default metallic-roughness: R unused (white), G roughness ~0.8, B metallic 0
		g_mrDefault = TextureFactory::CreateSolidColor(glm::vec4(1.0f, 204.0f / 255.0f, 0.0f, 1.0f), GL_RGBA8);

		// Validate all textures were created successfully
		if (!g_white || !g_white->IsValid() ||
			!g_black || !g_black->IsValid() ||
			!g_normal || !g_normal->IsValid() ||
			!g_aowhite || !g_aowhite->IsValid() ||
			!g_mrDefault || !g_mrDefault->IsValid()) {
			std::cerr << "[DefaultTextures] Some default textures failed to create!" << std::endl;
			return;
		}

		g_texturesCreated = true;
		std::cout << "[DefaultTextures] All default textures created successfully!" << std::endl;
		std::cout << "[DefaultTextures] White: " << g_white->ID() << ", Black: " << g_black->ID()
			<< ", Normal: " << g_normal->ID() << ", AOWhite: " << g_aowhite->ID()
			<< ", MR: " << g_mrDefault->ID() << std::endl;
	}

	// Backward-compatible GLuint accessors
	GLuint White() { EnsureCreated(); return g_white ? g_white->ID() : 0; }
	GLuint Black() { EnsureCreated(); return g_black ? g_black->ID() : 0; }
	GLuint Normal() { EnsureCreated(); return g_normal ? g_normal->ID() : 0; }
	GLuint AOWhite() { EnsureCreated(); return g_aowhite ? g_aowhite->ID() : 0; }
	GLuint MetallicRoughnessDefault() { EnsureCreated(); return g_mrDefault ? g_mrDefault->ID() : 0; }

	// Modern TexturePtr accessors for new code
	TexturePtr GetWhiteTexture() { EnsureCreated(); return g_white; }
	TexturePtr GetBlackTexture() { EnsureCreated(); return g_black; }
	TexturePtr GetNormalTexture() { EnsureCreated(); return g_normal; }
	TexturePtr GetAOWhiteTexture() { EnsureCreated(); return g_aowhite; }
	TexturePtr GetMRDefaultTexture() { EnsureCreated(); return g_mrDefault; }
}
