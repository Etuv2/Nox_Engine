#pragma once
#include "rhi/RHI.h"
#include "Vertex.h"
#include "Texture.h"
#include "GpuBuffer.h"
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <array>

/**
 * KHR_texture_transform for one texture reference, glTF order: uv' = T * R * S * uv.
 * The texCoord override of the extension is folded into the slot's *TexCoord field.
 */
struct TextureTransform {
	glm::vec2 offset = glm::vec2(0.0f);
	glm::vec2 scale = glm::vec2(1.0f);
	float rotation = 0.0f;   // radians, glTF convention (counter-clockwise in UV space)

	bool IsIdentity() const {
		return offset == glm::vec2(0.0f) && scale == glm::vec2(1.0f) && rotation == 0.0f;
	}
	bool operator==(const TextureTransform& o) const {
		return offset == o.offset && scale == o.scale && rotation == o.rotation;
	}
	bool operator!=(const TextureTransform& o) const { return !(*this == o); }

	glm::mat3 ToMat3() const {
		const float c = std::cos(rotation);
		const float s = std::sin(rotation);
		glm::mat3 T(1.0f);
		T[2] = glm::vec3(offset, 1.0f);
		// Rows of R are (c, s) and (-s, c) per the spec, so columns are (c, -s) and (s, c).
		const glm::mat3 R(glm::vec3(c, -s, 0.0f), glm::vec3(s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		glm::mat3 S(1.0f);
		S[0][0] = scale.x;
		S[1][1] = scale.y;
		return T * R * S;
	}
};

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

	// Texture reference slots. Shader uniforms, JSON keys and editor rows all index by this order.
	enum TextureSlot : int {
		SlotBaseColor = 0,
		SlotNormal,
		SlotMetallicRoughness,
		SlotEmissive,
		SlotOcclusion,
		SlotSpecular,
		SlotSpecularColor,
		SlotTransmission,
		SlotClearcoat,
		SlotClearcoatRoughness,
		SlotCount
	};
	static_assert(SlotCount == 10, "uvXform arrays in RenderSystem and the shaders are sized for 10 texture slots");
	static const char* TextureSlotKey(int slot) {
		static const char* const keys[SlotCount] = {
			"base_color", "normal", "metallic_roughness", "emissive", "occlusion",
			"specular", "specular_color", "transmission", "clearcoat", "clearcoat_roughness" };
		return (slot >= 0 && slot < SlotCount) ? keys[slot] : "";
	}
	static const char* TextureSlotLabel(int slot) {
		static const char* const labels[SlotCount] = {
			"Base color", "Normal", "Metallic-roughness", "Emissive", "Occlusion",
			"Specular", "Specular color", "Transmission", "Clearcoat (stored, not sampled)", "Clearcoat roughness (stored, not sampled)" };
		return (slot >= 0 && slot < SlotCount) ? labels[slot] : "";
	}
	// Slot index for a JSON key (see TextureSlotKey), or -1 when the key is unknown.
	static int TextureSlotFromKey(const std::string& key) {
		for (int slot = 0; slot < SlotCount; ++slot) {
			if (key == TextureSlotKey(slot)) return slot;
		}
		return -1;
	}
	// Whether the slot holds colour data (sRGB texture format); everything else is linear.
	static bool TextureSlotIsSRGB(int slot) {
		return slot == SlotBaseColor || slot == SlotEmissive || slot == SlotSpecularColor;
	}

	uint32_t stableMaterialID = 1u;
	AlphaMode alphaMode = AlphaMode::Opaque;
	bool doubleSided = false;
	bool hasAlpha = false;

	glm::vec4 baseColorFactor = glm::vec4(1.0f);
	float metallicFactor = 1.0f;
	float roughnessFactor = 1.0f;
	float alphaCutoff = 0.5f;
	glm::vec3 emissiveFactor = glm::vec3(0.0f);
	float emissiveStrength = 1.0f;

	float specularFactor = 1.0f;
	glm::vec3 specularColorFactor = glm::vec3(1.0f);
	float clearcoatFactor = 0.0f;
	float clearcoatRoughnessFactor = 0.0f;
	float transmissionFactor = 0.0f;
	float thicknessFactor = 0.0f;
	float attenuationDistance = std::numeric_limits<float>::infinity();
	glm::vec3 attenuationColor = glm::vec3(1.0f);
	float ior = 1.5f;

	float occlusionStrength = 1.0f;
	float normalScale = 1.0f;

	// NOX surface extras (glTF material extras "nox" or scene material_overrides).
	// All default to 0 so imported content is unaffected unless opted in.
	float wetnessResponse = 0.0f;   // How strongly the scene wetness darkens/smooths this surface [0,1]
	float puddleResponse = 0.0f;    // Whether standing water may form here (roads, sidewalks) [0,1]
	float rainBeadResponse = 0.0f;  // Rain droplets on smooth coated surfaces (car paint, glass) [0,1]
	float flakeIntensity = 0.0f;    // Metallic paint flake sparkle strength [0,1]
	float flakeDensity = 4000.0f;   // Flake cells per object-space metre

	// Window interior mapping (shaders/includes/interior_mapping.glsl). Appended; 0 = off.
	// Only meaningful on pane quads with a tangent frame (TEXCOORD_0 + tangent).
	float interiorMapping = 0.0f;   // 0 = off, 1 = parallax room behind the glass, 2 = flat room mean (cheap)
	float interiorDepth = 4.0f;     // Room depth behind the glass in metres [1,20]
	float interiorStrength = 1.0f;  // Room radiance multiplier applied on top of emissiveFactor*emissiveStrength
	float interiorSeed = 0.0f;      // Per-material variation seed mixed into the per-window hash
	float interiorLighting = 0.5f;  // Occupancy bias: 0 = mostly dark rooms, 1 = mostly lit rooms [0,1]
	float interiorBlinds = 0.0f;    // Probability a window has blinds or curtains [0,1]
	float interiorStyle = 0.0f;     // Interior atlas variant weight row (clamped to the atlas's rows)
	glm::vec2 interiorPane = glm::vec2(0.0f);   // pane half extents in metres; 0 = from the UV derivatives
	glm::vec3 interiorPaneUV = glm::vec3(0.0f); // pane UV layout (grid, inset, extent); grid 0 = atlas default
	float interiorSill = 0.8f;      // room floor distance below the pane's bottom edge in metres
	float interiorFarDistance = -1.0f; // flat room mean beyond this camera distance (m); < 0 = atlas default, 0 = never

	int baseColorTexCoord = 0;
	int normalTexCoord = 0;
	int metallicRoughnessTexCoord = 0;
	int emissiveTexCoord = 0;
	int occlusionTexCoord = 0;
	int specularTexCoord = 0;
	int specularColorTexCoord = 0;
	int transmissionTexCoord = 0;
	int clearcoatTexCoord = 0;
	int clearcoatRoughnessTexCoord = 0;

	// KHR_texture_transform per slot (identity unless the material uses the extension).
	TextureTransform textureTransforms[SlotCount];

	// KHR_materials_unlit: base colour is written as emission and no light is applied.
	bool unlit = false;

	// Parsed and stored, not shaded yet (see docs/rendering_components.md, "Extension status").
	glm::vec3 sheenColorFactor = glm::vec3(0.0f);       // KHR_materials_sheen
	float sheenRoughnessFactor = 0.0f;                  // KHR_materials_sheen
	float anisotropyStrength = 0.0f;                    // KHR_materials_anisotropy
	float anisotropyRotation = 0.0f;                    // KHR_materials_anisotropy (radians)
	float iridescenceFactor = 0.0f;                     // KHR_materials_iridescence
	float iridescenceIor = 1.3f;                        // KHR_materials_iridescence
	float iridescenceThicknessMin = 100.0f;             // KHR_materials_iridescence (nm)
	float iridescenceThicknessMax = 400.0f;             // KHR_materials_iridescence (nm)
	float dispersion = 0.0f;                            // KHR_materials_dispersion

	int& TexCoordOf(int slot) {
		switch (slot) {
		case SlotBaseColor: return baseColorTexCoord;
		case SlotNormal: return normalTexCoord;
		case SlotMetallicRoughness: return metallicRoughnessTexCoord;
		case SlotEmissive: return emissiveTexCoord;
		case SlotOcclusion: return occlusionTexCoord;
		case SlotSpecular: return specularTexCoord;
		case SlotSpecularColor: return specularColorTexCoord;
		case SlotTransmission: return transmissionTexCoord;
		case SlotClearcoat: return clearcoatTexCoord;
		default: return clearcoatRoughnessTexCoord;
		}
	}
	int TexCoordOf(int slot) const {
		return const_cast<MaterialDesc*>(this)->TexCoordOf(slot);
	}

	void Normalize() {
		baseColorFactor = glm::clamp(baseColorFactor, glm::vec4(0.0f), glm::vec4(1.0f));
		metallicFactor = std::clamp(metallicFactor, 0.0f, 1.0f);
		roughnessFactor = std::clamp(roughnessFactor, 0.0f, 1.0f);
		alphaCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f);
		emissiveStrength = std::max(emissiveStrength, 0.0f);
		specularFactor = std::clamp(specularFactor, 0.0f, 1.0f);
		specularColorFactor = glm::clamp(specularColorFactor, glm::vec3(0.0f), glm::vec3(1.0f));
		clearcoatFactor = std::clamp(clearcoatFactor, 0.0f, 1.0f);
		clearcoatRoughnessFactor = std::clamp(clearcoatRoughnessFactor, 0.0f, 1.0f);
		transmissionFactor = std::clamp(transmissionFactor, 0.0f, 1.0f);
		thicknessFactor = std::max(thicknessFactor, 0.0f);
		if (!std::isfinite(attenuationDistance)) {
			attenuationDistance = std::numeric_limits<float>::infinity();
		}
		else {
			attenuationDistance = std::max(attenuationDistance, 0.0f);
		}
		attenuationColor = glm::clamp(attenuationColor, glm::vec3(0.0f), glm::vec3(1.0f));
		ior = std::max(ior, 1.0f);
		occlusionStrength = std::clamp(occlusionStrength, 0.0f, 1.0f);
		normalScale = std::max(normalScale, 0.0f);
		wetnessResponse = std::clamp(wetnessResponse, 0.0f, 1.0f);
		puddleResponse = std::clamp(puddleResponse, 0.0f, 1.0f);
		rainBeadResponse = std::clamp(rainBeadResponse, 0.0f, 1.0f);
		flakeIntensity = std::clamp(flakeIntensity, 0.0f, 1.0f);
		flakeDensity = std::clamp(flakeDensity, 10.0f, 100000.0f);
		interiorMapping = std::clamp(std::round(interiorMapping), 0.0f, 2.0f);
		interiorDepth = std::clamp(interiorDepth, 1.0f, 20.0f);
		interiorStrength = std::max(interiorStrength, 0.0f);
		interiorLighting = std::clamp(interiorLighting, 0.0f, 1.0f);
		interiorBlinds = std::clamp(interiorBlinds, 0.0f, 1.0f);
		interiorStyle = std::max(std::round(interiorStyle), 0.0f);
		interiorPane = (interiorPane.x > 0.0f && interiorPane.y > 0.0f)
			? glm::clamp(interiorPane, glm::vec2(0.05f), glm::vec2(50.0f)) : glm::vec2(0.0f);
		if (interiorPaneUV.x > 0.0f) {
			interiorPaneUV.x = std::clamp(std::round(interiorPaneUV.x), 1.0f, 64.0f);
			interiorPaneUV.y = std::clamp(interiorPaneUV.y, 0.0f, 1.0f / interiorPaneUV.x);
			interiorPaneUV.z = std::clamp(interiorPaneUV.z, 1e-4f, 1.0f / interiorPaneUV.x - interiorPaneUV.y);
		}
		else {
			interiorPaneUV = glm::vec3(0.0f);
		}
		interiorSill = std::clamp(interiorSill, 0.0f, 10.0f);
		interiorFarDistance = interiorFarDistance < 0.0f ? -1.0f : interiorFarDistance;

		auto clampSupportedUVSet = [](int uvSet) { return uvSet == 1 ? 1 : 0; };
		baseColorTexCoord = clampSupportedUVSet(baseColorTexCoord);
		normalTexCoord = clampSupportedUVSet(normalTexCoord);
		metallicRoughnessTexCoord = clampSupportedUVSet(metallicRoughnessTexCoord);
		emissiveTexCoord = clampSupportedUVSet(emissiveTexCoord);
		occlusionTexCoord = clampSupportedUVSet(occlusionTexCoord);
		specularTexCoord = clampSupportedUVSet(specularTexCoord);
		specularColorTexCoord = clampSupportedUVSet(specularColorTexCoord);
		transmissionTexCoord = clampSupportedUVSet(transmissionTexCoord);
		clearcoatTexCoord = clampSupportedUVSet(clearcoatTexCoord);
		clearcoatRoughnessTexCoord = clampSupportedUVSet(clearcoatRoughnessTexCoord);
		for (TextureTransform& xf : textureTransforms) {
			xf.scale = glm::clamp(xf.scale, glm::vec2(-1e4f), glm::vec2(1e4f));
			if (!std::isfinite(xf.rotation)) xf.rotation = 0.0f;
		}
		sheenColorFactor = glm::clamp(sheenColorFactor, glm::vec3(0.0f), glm::vec3(1.0f));
		sheenRoughnessFactor = std::clamp(sheenRoughnessFactor, 0.0f, 1.0f);
		anisotropyStrength = std::clamp(anisotropyStrength, 0.0f, 1.0f);
		iridescenceFactor = std::clamp(iridescenceFactor, 0.0f, 1.0f);
		iridescenceIor = std::max(iridescenceIor, 1.0f);
		iridescenceThicknessMin = std::max(iridescenceThicknessMin, 0.0f);
		iridescenceThicknessMax = std::max(iridescenceThicknessMax, iridescenceThicknessMin);
		dispersion = std::max(dispersion, 0.0f);

		hasAlpha = hasAlpha || alphaMode == AlphaMode::Blend || baseColorFactor.a < 1.0f || transmissionFactor > 0.0f;
	}
};

class MeshComponent {
public:
	rhi::VertexInputHandle VAO;   // vertex + index buffer bindings
	GpuBufferPtr vertexBuffer;
	GpuBufferPtr indexBuffer;
	size_t indexCount;

	// Legacy accessors for compatibility
	rhi::BufferHandle GetVBO() const { return vertexBuffer ? vertexBuffer->GetID() : rhi::BufferHandle{}; }
	rhi::BufferHandle GetEBO() const { return indexBuffer ? indexBuffer->GetID() : rhi::BufferHandle{}; }
	// Legacy property accessors (for code that directly accessed VBO/EBO)
	rhi::BufferHandle VBO() const { return GetVBO(); }
	rhi::BufferHandle EBO() const { return GetEBO(); }

	// glTF 2.0 standard texture assignments (using new Texture system)
	std::shared_ptr<Texture> diffuseTexture;        // Base color (albedo) texture
	std::shared_ptr<Texture> normalTexture;         // Normal map
	std::shared_ptr<Texture> roughnessTexture;      // Metallic-Roughness texture (G=roughness, B=metallic)
	std::shared_ptr<Texture> emissiveTexture;       // Emissive texture
	std::shared_ptr<Texture> occlusionTexture;      // Ambient occlusion texture (R channel)
	std::shared_ptr<Texture> specularTexture;       // Specular texture (for KHR_materials_specular extension)
	std::shared_ptr<Texture> specularColorTexture;  // Specular color texture (KHR_materials_specular)
	std::shared_ptr<Texture> transmissionTexture;   // Transmission texture (KHR_materials_transmission)
	std::shared_ptr<Texture> clearcoatTexture;      // Clearcoat intensity (R channel, KHR_materials_clearcoat)
	std::shared_ptr<Texture> clearcoatRoughnessTexture; // Clearcoat roughness (G channel, KHR_materials_clearcoat)
	// Source file of each texture slot when the editor or a scene override assigned it ("" = imported).
	std::array<std::string, MaterialDesc::SlotCount> textureSourcePaths;

	// Canonical normalized material state used by import, raster uploads, and RT packing.
	MaterialDesc material;
	std::string materialName; // Source material name (glTF), used by scene material_overrides

	// Material properties
	bool hasAlpha;                // Requires alpha blending
	bool doubleSided;             // Disable backface culling (from glTF material)

	// Animation and morphing
	std::vector<GpuBufferPtr> morphBuffers; // One buffer per morph target (replaces morphVBOs)
	int morphTargetCount = 0;  // Number of morph targets

	// glTF 2.0 standard material factors
	glm::vec4 baseColorFactor = glm::vec4(1.0f);     // RGBA base color multiplier
	float metallicFactor = 1.0f;                     // Metallic factor [0,1]
	float roughnessFactor = 1.0f;                    // Roughness factor [0,1]
	float alphaCutoff = 0.5f;                        // Alpha cutoff for alpha testing
	glm::vec3 emissiveFactor = glm::vec3(0.0f);      // Emissive color multiplier

	// Additional material factors for extensions
	float specularFactor = 1.0f;                     // KHR_materials_specular scalar weight
	glm::vec3 specularColorFactor = glm::vec3(1.0f); // KHR_materials_specular
	float emissiveStrength = 1.0f;                   // KHR_materials_emissive_strength
	float clearcoatFactor = 0.0f;                    // KHR_materials_clearcoat
	float clearcoatRoughnessFactor = 0.0f;           // KHR_materials_clearcoat
	float occlusionStrength = 1.0f;                  // Occlusion strength
	float normalScale = 1.0f;                        // Normal map intensity

	// KHR_materials_transmission extension
	float transmissionFactor = 0.0f;                 // Transmission factor [0,1]
	float thicknessFactor = 0.0f;                    // KHR_materials_volume thickness
	float attenuationDistance = std::numeric_limits<float>::infinity(); // KHR_materials_volume attenuation distance
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
		: VAO(),
		indexCount(0),
		diffuseTexture(nullptr), normalTexture(nullptr),
		roughnessTexture(nullptr), emissiveTexture(nullptr),
		occlusionTexture(nullptr), specularTexture(nullptr),
		specularColorTexture(nullptr), transmissionTexture(nullptr),
		clearcoatTexture(nullptr), clearcoatRoughnessTexture(nullptr),
		hasAlpha(false), doubleSided(false) {
	}

	// Owns its GPU vertex input and buffers: released with the mesh (scene swaps, model reloads, scene destruction).
	~MeshComponent() { Cleanup(); }

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
		, clearcoatTexture(std::move(other.clearcoatTexture))
		, clearcoatRoughnessTexture(std::move(other.clearcoatRoughnessTexture))
		, textureSourcePaths(std::move(other.textureSourcePaths))
		, material(other.material)
		, materialName(std::move(other.materialName))
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
		other.VAO = {};
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
			clearcoatTexture = std::move(other.clearcoatTexture);
			clearcoatRoughnessTexture = std::move(other.clearcoatRoughnessTexture);
			textureSourcePaths = std::move(other.textureSourcePaths);
			material = other.material;
			materialName = std::move(other.materialName);
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
			other.VAO = {};
			other.indexCount = 0;
		}
		return *this;
	}

	// Delete copy operations (unique_ptr can't be copied)
	MeshComponent(const MeshComponent&) = delete;
	MeshComponent& operator=(const MeshComponent&) = delete;

	void SyncLegacyMaterialStateFromDesc() {
		baseColorFactor = material.baseColorFactor;
		metallicFactor = material.metallicFactor;
		roughnessFactor = material.roughnessFactor;
		alphaCutoff = material.alphaCutoff;
		emissiveFactor = material.emissiveFactor;
		emissiveStrength = material.emissiveStrength;
		specularFactor = material.specularFactor;
		specularColorFactor = material.specularColorFactor;
		clearcoatFactor = material.clearcoatFactor;
		clearcoatRoughnessFactor = material.clearcoatRoughnessFactor;
		transmissionFactor = material.transmissionFactor;
		thicknessFactor = material.thicknessFactor;
		attenuationDistance = material.attenuationDistance;
		attenuationColor = material.attenuationColor;
		ior = material.ior;
		occlusionStrength = material.occlusionStrength;
		normalScale = material.normalScale;
		hasAlpha = material.hasAlpha;
		doubleSided = material.doubleSided;
		alphaMode = static_cast<AlphaMode>(static_cast<uint32_t>(material.alphaMode));
	}

	MaterialDesc BuildMaterialDescFromLegacy() const {
		MaterialDesc desc = material;
		desc.baseColorFactor = baseColorFactor;
		desc.metallicFactor = metallicFactor;
		desc.roughnessFactor = roughnessFactor;
		desc.alphaCutoff = alphaCutoff;
		desc.emissiveFactor = emissiveFactor;
		desc.emissiveStrength = emissiveStrength;
		desc.specularFactor = specularFactor;
		desc.specularColorFactor = specularColorFactor;
		desc.clearcoatFactor = clearcoatFactor;
		desc.clearcoatRoughnessFactor = clearcoatRoughnessFactor;
		desc.transmissionFactor = transmissionFactor;
		desc.thicknessFactor = thicknessFactor;
		desc.attenuationDistance = attenuationDistance;
		desc.attenuationColor = attenuationColor;
		desc.ior = ior;
		desc.occlusionStrength = occlusionStrength;
		desc.normalScale = normalScale;
		desc.hasAlpha = hasAlpha;
		desc.doubleSided = doubleSided;
		desc.alphaMode = static_cast<MaterialDesc::AlphaMode>(static_cast<uint32_t>(alphaMode));
		desc.Normalize();
		return desc;
	}

	void SetMaterialDesc(const MaterialDesc& desc) {
		material = desc;
		material.Normalize();
		SyncLegacyMaterialStateFromDesc();
	}

	MaterialDesc GetMaterialDesc() const {
		return BuildMaterialDescFromLegacy();
	}

	// Texture object behind a MaterialDesc::TextureSlot (the per-slot member above). slot < SlotCount.
	std::shared_ptr<Texture>& TextureForSlot(int slot) {
		switch (slot) {
		case MaterialDesc::SlotBaseColor: return diffuseTexture;
		case MaterialDesc::SlotNormal: return normalTexture;
		case MaterialDesc::SlotMetallicRoughness: return roughnessTexture;
		case MaterialDesc::SlotEmissive: return emissiveTexture;
		case MaterialDesc::SlotOcclusion: return occlusionTexture;
		case MaterialDesc::SlotSpecular: return specularTexture;
		case MaterialDesc::SlotSpecularColor: return specularColorTexture;
		case MaterialDesc::SlotTransmission: return transmissionTexture;
		case MaterialDesc::SlotClearcoat: return clearcoatTexture;
		default: return clearcoatRoughnessTexture;
		}
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
		if (VAO && rhi::HasGpu()) rhi::Gpu().DestroyVertexInput(VAO);
		
		// GpuBuffer smart pointers handle cleanup automatically
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
		clearcoatTexture.reset();
		clearcoatRoughnessTexture.reset();

		// Reset VAO handle
		VAO = {};
	}
};
