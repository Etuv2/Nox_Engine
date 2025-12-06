#pragma once
#include <GL/glew.h>
#include "Vertex.h"
#include "Texture.h"
#include "GLBuffer.h"
#include <vector>
#include <memory>

class MeshComponent {
public:
	GLuint VAO;
	GLBufferPtr vertexBuffer;   // Replaces raw VBO
	GLBufferPtr indexBuffer;    // Replaces raw EBO
	size_t indexCount;

	// Legacy accessors for compatibility
	GLuint GetVBO() const { return vertexBuffer ? vertexBuffer->GetID() : 0; }
	GLuint GetEBO() const { return indexBuffer ? indexBuffer->GetID() : 0; }
	// Legacy property accessors (for code that directly accessed VBO/EBO)
	GLuint VBO() const { return GetVBO(); }
	GLuint EBO() const { return GetEBO(); }

	// glTF 2.0 standard texture assignments (using new Texture system)
	std::shared_ptr<Texture> diffuseTexture;        // Base color (albedo) texture
	std::shared_ptr<Texture> normalTexture;         // Normal map
	std::shared_ptr<Texture> roughnessTexture;      // Metallic-Roughness texture (G=roughness, B=metallic)
	std::shared_ptr<Texture> emissiveTexture;       // Emissive texture
	std::shared_ptr<Texture> occlusionTexture;      // Ambient occlusion texture (R channel)
	std::shared_ptr<Texture> specularTexture;       // Specular texture (for KHR_materials_specular extension)
	std::shared_ptr<Texture> specularColorTexture;  // Specular color texture (KHR_materials_specular)
	std::shared_ptr<Texture> transmissionTexture;   // Transmission texture (KHR_materials_transmission)

	// Material properties
	bool hasAlpha;                // Requires alpha blending
	bool doubleSided;             // Disable backface culling (from glTF material)

	// Animation and morphing
	std::vector<GLBufferPtr> morphBuffers; // One buffer per morph target (replaces morphVBOs)
	int morphTargetCount = 0;  // Number of morph targets

	// glTF 2.0 standard material factors
	glm::vec4 baseColorFactor = glm::vec4(1.0f);     // RGBA base color multiplier
	float metallicFactor = 0.0f;                     // Metallic factor [0,1]
	float roughnessFactor = 1.0f;                    // Roughness factor [0,1]
	float alphaCutoff = 0.5f;                        // Alpha cutoff for alpha testing
	glm::vec3 emissiveFactor = glm::vec3(0.0f);      // Emissive color multiplier

	// Additional material factors for extensions
	glm::vec3 specularFactor = glm::vec3(0.0f);      // KHR_materials_specular
	glm::vec3 specularColorFactor = glm::vec3(1.0f); // KHR_materials_specular
	float occlusionStrength = 1.0f;                  // Occlusion strength
	float normalScale = 1.0f;                        // Normal map intensity

	// KHR_materials_transmission extension
	float transmissionFactor = 0.0f;                 // Transmission factor [0,1]

	// KHR_materials_ior extension
	float ior = 1.5f;                                // Index of refraction (default 1.5 for glass)

	// KHR_materials_pbrSpecularGlossiness extension support
	bool useSpecularGlossinessWorkflow = false;
	glm::vec3 diffuseFactor = glm::vec3(1.0f);
	glm::vec3 specularGlossinessFactor = glm::vec3(1.0f);
	float glossinessFactor = 1.0f;

	// Alpha mode enumeration following glTF spec
	enum AlphaMode {
		ALPHA_OPAQUE = 0,
		ALPHA_MASK = 1,
		ALPHA_BLEND = 2
	};
	AlphaMode alphaMode = ALPHA_OPAQUE;

	// Culling mode enumeration for better control
	enum CullingMode {
		CULL_BACK = 0,
		CULL_FRONT = 1,
		CULL_NONE = 2,
		CULL_DEFAULT = 3
	};
	CullingMode cullingMode = CULL_DEFAULT;

	// Raw mesh data for CPU operations
	std::vector<Vertex> rawVertices;
	std::vector<uint32_t> rawIndices;

	// Precomputed bounding volume
	glm::vec3 boundingCenter = glm::vec3(0.0f);
	float boundingRadius = 1.0f;
	bool boundingVolumeValid = false;

	MeshComponent()
		: VAO(0),
		indexCount(0),
		diffuseTexture(nullptr), normalTexture(nullptr),
		roughnessTexture(nullptr), emissiveTexture(nullptr),
		occlusionTexture(nullptr), specularTexture(nullptr),
		specularColorTexture(nullptr), transmissionTexture(nullptr),
		hasAlpha(false), doubleSided(false) {
	}

	// Move constructor
	MeshComponent(MeshComponent&& other) noexcept
		: VAO(other.VAO)
		, vertexBuffer(std::move(other.vertexBuffer))
		, indexBuffer(std::move(other.indexBuffer))
		, indexCount(other.indexCount)
		, diffuseTexture(std::move(other.diffuseTexture))
		, normalTexture(std::move(other.normalTexture))
		, roughnessTexture(std::move(other.roughnessTexture))
		, emissiveTexture(std::move(other.emissiveTexture))
		, occlusionTexture(std::move(other.occlusionTexture))
		, specularTexture(std::move(other.specularTexture))
		, specularColorTexture(std::move(other.specularColorTexture))
		, transmissionTexture(std::move(other.transmissionTexture))
		, hasAlpha(other.hasAlpha)
		, doubleSided(other.doubleSided)
		, morphBuffers(std::move(other.morphBuffers))
		, morphTargetCount(other.morphTargetCount)
		, baseColorFactor(other.baseColorFactor)
		, metallicFactor(other.metallicFactor)
		, roughnessFactor(other.roughnessFactor)
		, alphaCutoff(other.alphaCutoff)
		, emissiveFactor(other.emissiveFactor)
		, specularFactor(other.specularFactor)
		, specularColorFactor(other.specularColorFactor)
		, occlusionStrength(other.occlusionStrength)
		, normalScale(other.normalScale)
		, transmissionFactor(other.transmissionFactor)
		, ior(other.ior)
		, useSpecularGlossinessWorkflow(other.useSpecularGlossinessWorkflow)
		, diffuseFactor(other.diffuseFactor)
		, specularGlossinessFactor(other.specularGlossinessFactor)
		, glossinessFactor(other.glossinessFactor)
		, alphaMode(other.alphaMode)
		, cullingMode(other.cullingMode)
		, rawVertices(std::move(other.rawVertices))
		, rawIndices(std::move(other.rawIndices))
		, boundingCenter(other.boundingCenter)
		, boundingRadius(other.boundingRadius)
		, boundingVolumeValid(other.boundingVolumeValid)
	{
		other.VAO = 0;
		other.indexCount = 0;
	}

	// Move assignment operator
	MeshComponent& operator=(MeshComponent&& other) noexcept {
		if (this != &other) {
			Cleanup();
			VAO = other.VAO;
			vertexBuffer = std::move(other.vertexBuffer);
			indexBuffer = std::move(other.indexBuffer);
			indexCount = other.indexCount;
			diffuseTexture = std::move(other.diffuseTexture);
			normalTexture = std::move(other.normalTexture);
			roughnessTexture = std::move(other.roughnessTexture);
			emissiveTexture = std::move(other.emissiveTexture);
			occlusionTexture = std::move(other.occlusionTexture);
			specularTexture = std::move(other.specularTexture);
			specularColorTexture = std::move(other.specularColorTexture);
			transmissionTexture = std::move(other.transmissionTexture);
			hasAlpha = other.hasAlpha;
			doubleSided = other.doubleSided;
			morphBuffers = std::move(other.morphBuffers);
			morphTargetCount = other.morphTargetCount;
			baseColorFactor = other.baseColorFactor;
			metallicFactor = other.metallicFactor;
			roughnessFactor = other.roughnessFactor;
			alphaCutoff = other.alphaCutoff;
			emissiveFactor = other.emissiveFactor;
			specularFactor = other.specularFactor;
			specularColorFactor = other.specularColorFactor;
			occlusionStrength = other.occlusionStrength;
			normalScale = other.normalScale;
			transmissionFactor = other.transmissionFactor;
			ior = other.ior;
			useSpecularGlossinessWorkflow = other.useSpecularGlossinessWorkflow;
			diffuseFactor = other.diffuseFactor;
			specularGlossinessFactor = other.specularGlossinessFactor;
			glossinessFactor = other.glossinessFactor;
			alphaMode = other.alphaMode;
			cullingMode = other.cullingMode;
			rawVertices = std::move(other.rawVertices);
			rawIndices = std::move(other.rawIndices);
			boundingCenter = other.boundingCenter;
			boundingRadius = other.boundingRadius;
			boundingVolumeValid = other.boundingVolumeValid;
			other.VAO = 0;
			other.indexCount = 0;
		}
		return *this;
	}

	// Delete copy operations (unique_ptr can't be copied)
	MeshComponent(const MeshComponent&) = delete;
	MeshComponent& operator=(const MeshComponent&) = delete;

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
		return doubleSided ? CULL_NONE : CULL_BACK;
	}

	// Compute and cache bounding volume once at mesh creation
	void ComputeBoundingVolume() {
		if (rawVertices.empty()) {
			boundingCenter = glm::vec3(0.0f);
			boundingRadius = 1.0f;
			boundingVolumeValid = false;
			return;
		}

		glm::vec3 minB(std::numeric_limits<float>::max());
		glm::vec3 maxB(std::numeric_limits<float>::lowest());

		for (const auto& v : rawVertices) {
			minB = glm::min(minB, v.position);
			maxB = glm::max(maxB, v.position);
		}

		boundingCenter = (minB + maxB) * 0.5f;
		boundingRadius = glm::length(maxB - boundingCenter);
		boundingVolumeValid = true;
	}

	void Cleanup() {
		if (VAO) glDeleteVertexArrays(1, &VAO);
		
		// GLBuffer smart pointers handle cleanup automatically
		vertexBuffer.reset();
		indexBuffer.reset();
		morphBuffers.clear();

		// Textures will be automatically cleaned up by shared_ptr destructors
		diffuseTexture.reset();
		normalTexture.reset();
		roughnessTexture.reset();
		emissiveTexture.reset();
		occlusionTexture.reset();
		specularTexture.reset();
		specularColorTexture.reset();
		transmissionTexture.reset();

		// Reset VAO handle
		VAO = 0;
	}
};
