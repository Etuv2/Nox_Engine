#pragma once
#include <GL/glew.h>
#include "Vertex.h"
#include "Texture.h"
#include "GLBuffer.h"
#include <vector>
#include <memory>
#include <cstdint>

/**
 * Canonical CPU-side material contract shared by import, renderer uploads, and RT packing.
 *
 * The goal is to normalize all material data toward glTF metallic-roughness while retaining
 * the supported extension values needed by the engine today and by future RT paths.
 */
struct MaterialDesc {
	enum class AlphaMode : uint32_t {
		Opaque = 0,
		Mask = 1,
		Blend = 2
	};

	uint32_t stableMaterialID = 1u;
	AlphaMode alphaMode = AlphaMode::Opaque;
	bool doubleSided = false;
	bool hasAlpha = false;

	glm::vec4 baseColorFactor = glm::vec4(1.0f);
	float metallicFactor = 0.0f;
	float roughnessFactor = 1.0f;
	float alphaCutoff = 0.5f;
	glm::vec3 emissiveFactor = glm::vec3(0.0f);
	float emissiveStrength = 1.0f;

	glm::vec3 specularFactor = glm::vec3(1.0f);
	glm::vec3 specularColorFactor = glm::vec3(1.0f);
	float clearcoatFactor = 0.0f;
	float clearcoatRoughnessFactor = 0.0f;
	float transmissionFactor = 0.0f;
	float thicknessFactor = 0.0f;
	float attenuationDistance = 0.0f;
	glm::vec3 attenuationColor = glm::vec3(1.0f);
	float ior = 1.5f;

	float occlusionStrength = 1.0f;
	float normalScale = 1.0f;
};

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

	// Canonical normalized material state used by import, raster uploads, and RT packing.
	MaterialDesc material;

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
	glm::vec3 specularFactor = glm::vec3(1.0f);      // KHR_materials_specular
	glm::vec3 specularColorFactor = glm::vec3(1.0f); // KHR_materials_specular
	float emissiveStrength = 1.0f;                   // KHR_materials_emissive_strength
	float clearcoatFactor = 0.0f;                    // KHR_materials_clearcoat
	float clearcoatRoughnessFactor = 0.0f;           // KHR_materials_clearcoat
	float occlusionStrength = 1.0f;                  // Occlusion strength
	float normalScale = 1.0f;                        // Normal map intensity

	// KHR_materials_transmission extension
	float transmissionFactor = 0.0f;                 // Transmission factor [0,1]
	float thicknessFactor = 0.0f;                    // KHR_materials_volume thickness
	float attenuationDistance = 0.0f;                // KHR_materials_volume attenuation distance
	glm::vec3 attenuationColor = glm::vec3(1.0f);    // KHR_materials_volume attenuation color

	// KHR_materials_ior extension
	float ior = 1.5f;                                // Index of refraction (default 1.5 for glass)

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
 glm::vec3 boundingMin = glm::vec3(0.0f);
	glm::vec3 boundingMax = glm::vec3(0.0f);
	glm::vec3 boundingCenter = glm::vec3(0.0f);
	float boundingRadius = 1.0f;
	bool boundingVolumeValid = false;
	glm::mat4 localTransform = glm::mat4(1.0f);
	int sourceNodeIndex = -1;

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
		, material(other.material)
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
		, emissiveStrength(other.emissiveStrength)
		, clearcoatFactor(other.clearcoatFactor)
		, clearcoatRoughnessFactor(other.clearcoatRoughnessFactor)
		, occlusionStrength(other.occlusionStrength)
		, normalScale(other.normalScale)
		, transmissionFactor(other.transmissionFactor)
		, thicknessFactor(other.thicknessFactor)
		, attenuationDistance(other.attenuationDistance)
		, attenuationColor(other.attenuationColor)
		, ior(other.ior)
		, alphaMode(other.alphaMode)
		, cullingMode(other.cullingMode)
		, rawVertices(std::move(other.rawVertices))
		, rawIndices(std::move(other.rawIndices))
      , boundingMin(other.boundingMin)
		, boundingMax(other.boundingMax)
		, boundingCenter(other.boundingCenter)
		, boundingRadius(other.boundingRadius)
		, boundingVolumeValid(other.boundingVolumeValid)
		, localTransform(other.localTransform)
		, sourceNodeIndex(other.sourceNodeIndex)
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
			material = other.material;
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
			emissiveStrength = other.emissiveStrength;
			clearcoatFactor = other.clearcoatFactor;
			clearcoatRoughnessFactor = other.clearcoatRoughnessFactor;
			occlusionStrength = other.occlusionStrength;
			normalScale = other.normalScale;
			transmissionFactor = other.transmissionFactor;
			thicknessFactor = other.thicknessFactor;
			attenuationDistance = other.attenuationDistance;
			attenuationColor = other.attenuationColor;
			ior = other.ior;
			alphaMode = other.alphaMode;
			cullingMode = other.cullingMode;
			rawVertices = std::move(other.rawVertices);
			rawIndices = std::move(other.rawIndices);
			boundingCenter = other.boundingCenter;
			boundingRadius = other.boundingRadius;
			boundingVolumeValid = other.boundingVolumeValid;
			localTransform = other.localTransform;
			sourceNodeIndex = other.sourceNodeIndex;
			other.VAO = 0;
			other.indexCount = 0;
		}
		return *this;
	}

	// Delete copy operations (unique_ptr can't be copied)
	MeshComponent(const MeshComponent&) = delete;
	MeshComponent& operator=(const MeshComponent&) = delete;

	void SetMaterialDesc(const MaterialDesc& desc) {
		material = desc;
		baseColorFactor = desc.baseColorFactor;
		metallicFactor = desc.metallicFactor;
		roughnessFactor = desc.roughnessFactor;
		alphaCutoff = desc.alphaCutoff;
		emissiveFactor = desc.emissiveFactor;
		emissiveStrength = desc.emissiveStrength;
		specularFactor = desc.specularFactor;
		specularColorFactor = desc.specularColorFactor;
		clearcoatFactor = desc.clearcoatFactor;
		clearcoatRoughnessFactor = desc.clearcoatRoughnessFactor;
		transmissionFactor = desc.transmissionFactor;
		thicknessFactor = desc.thicknessFactor;
		attenuationDistance = desc.attenuationDistance;
		attenuationColor = desc.attenuationColor;
		ior = desc.ior;
		occlusionStrength = desc.occlusionStrength;
		normalScale = desc.normalScale;
		hasAlpha = desc.hasAlpha;
		doubleSided = desc.doubleSided;
		alphaMode = static_cast<AlphaMode>(static_cast<uint32_t>(desc.alphaMode));
	}

	MaterialDesc GetMaterialDesc() const {
		return material;
	}

	// Determine if this mesh needs special rendering treatment
	bool RequiresAlphaBlending() const {
		return alphaMode == ALPHA_BLEND;
	}

	bool RequiresAlphaTesting() const {
		return alphaMode == ALPHA_MASK;
	}

	bool IsOpaque() const {
		return alphaMode != ALPHA_BLEND;
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
           boundingMin = glm::vec3(0.0f);
			boundingMax = glm::vec3(0.0f);
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

      boundingMin = minB;
		boundingMax = maxB;
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
