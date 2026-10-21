#include "reflect/Reflection.h"
#include "SceneLoader.h"
#include "physics/MeshCollider.h"
#include "SceneNode.h"
#include "AudioNode.h"
#include "GuiNode.h"
#include "LightNode.h"
#include "DirectionalLight.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "ModelManager.h"
#include "SliceTask.h"
#include "SceneExtensions.h"
#include "cook/CookedResults.h"
#include "InteriorAtlasManifest.h"
#include "ShaderLoader.h"
#include "assets/AssetIO.h"
#include "assets/AssetIOJson.h"
#include "Scene.h"
#include "HierarchySystem.h"
#include "AnimationSystem.h"
#include "AnimatorRuntime.h"
#include "RenderSystem.h"
#include "Camera.h"
#include "GuiElementJson.h"
#include "scene/Prefab.h"
#include "RenderContext.h"
#include "SceneRenderSettings.h"
#include "reflect/ReflectServices.h"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <thread>
#include <set>
#include <unordered_set>
#include <ctime>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <sstream>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using json = nlohmann::json;

namespace {
constexpr bool kVerboseEntityCreationLogs = false;
constexpr float kImportedTransformTolerance = 1e-3f;
constexpr float kMinimumSceneScale = 1e-5f;

namespace fs = std::filesystem;

namespace {
	// Transform values in scene files: rounded to a step (degrees for rotation), and -0 written as 0.
	// factor is 10^decimals: dividing the rounded integer by it gives the nearest double to the decimal value.
	double QuantizeTransform(double value, double factor) {
		return std::round(value * factor) / factor + 0.0;
	}
}

uint64_t NextRenderExtrasRevision()
{
	static uint64_t revision = 0;
	return ++revision;
}

std::string TrimPathValue(const std::string& value)
{
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	std::string trimmed = value.substr(begin, end - begin);
	if (trimmed.size() >= 2) {
		const char first = trimmed.front();
		const char last = trimmed.back();
		if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
			trimmed = trimmed.substr(1, trimmed.size() - 2);
		}
	}
	return trimmed;
}

// Logical asset path for an authored reference. Absolute physical paths inside the loose root map to
// their logical form; anything else must be a valid logical path (no root escapes).
std::optional<std::string> ToLogicalPath(const std::string& raw)
{
	if (fs::path(raw).is_absolute()) {
		return AssetIO::LogicalFromPhysical(fs::path(raw));
	}
	return AssetIO::Normalize(raw);
}

bool ReadableFileExists(const std::string& logicalPath)
{
	const auto st = AssetIO::Stat(logicalPath);
	return st.has_value() && !st->isDirectory;
}

void PushUniqueString(std::vector<std::string>& values, const std::string& value)
{
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
}

std::vector<std::string> BuildRelativeAssetCandidates(const std::string& rawPath, const std::string& sceneDirectory)
{
	std::vector<std::string> candidates;
	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		return candidates;
	}

	if (auto authored = ToLogicalPath(trimmed)) {
		PushUniqueString(candidates, *authored);
	}
	if (!fs::path(trimmed).is_absolute() && !sceneDirectory.empty()) {
		// Scene-relative candidates; references that escape the asset root are skipped.
		const std::string base = sceneDirectory + "/_";
		if (auto sceneRelative = AssetIO::ResolveRelative(base, trimmed)) {
			PushUniqueString(candidates, *sceneRelative);
		}
		if (auto parentRelative = AssetIO::ResolveRelative(base, "../" + trimmed)) {
			PushUniqueString(candidates, *parentRelative);
		}
	}
	return candidates;
}

std::string ResolveReadableAssetPath(
	const std::string& rawPath,
	const std::string& sceneDirectory,
	const char* assetKind,
	bool required,
	bool* resolved)
{
	if (resolved) {
		*resolved = false;
	}

	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		if (required) {
			std::cerr << "[SceneLoader] Missing required " << assetKind << " path." << std::endl;
		}
		return {};
	}

	const auto candidates = BuildRelativeAssetCandidates(trimmed, sceneDirectory);
	for (const std::string& candidate : candidates) {
		if (ReadableFileExists(candidate)) {
			if (resolved) {
				*resolved = true;
			}
			return candidate;
		}
	}

	std::ostringstream message;
	message << "[SceneLoader] " << (required ? "ERROR" : "Warning")
		<< ": could not resolve " << assetKind << " path '" << trimmed << "'. Tried:";
	for (const std::string& candidate : candidates) {
		message << "\n  - " << candidate;
	}

	if (required) {
		std::cerr << message.str() << std::endl;
	}
	else {
		std::cout << message.str() << std::endl;
	}
	return trimmed;
}

std::string ResolveSceneJsonPath(const std::string& rawPath)
{
	const std::string trimmed = TrimPathValue(rawPath);
	if (trimmed.empty()) {
		return {};
	}

	std::vector<std::string> candidates;
	const fs::path authoredPath(trimmed);
	if (auto authored = ToLogicalPath(trimmed)) {
		PushUniqueString(candidates, *authored);
		if (!authoredPath.has_extension()) {
			PushUniqueString(candidates, *authored + ".json");
		}
	}
	if (!authoredPath.is_absolute()) {
		if (auto inScenes = AssetIO::Normalize("scenes/" + trimmed)) {
			PushUniqueString(candidates, *inScenes);
			if (!authoredPath.has_extension()) {
				PushUniqueString(candidates, *inScenes + ".json");
			}
		}
	}

	for (const std::string& candidate : candidates) {
		if (ReadableFileExists(candidate)) {
			return candidate;
		}
	}

	return trimmed;
}

// Authoring writes (scene saves) go to the loose source tree only. Packed builds refuse them.
struct AuthoringTarget {
	std::string logical;
	fs::path physical;
};

std::optional<AuthoringTarget> ResolveAuthoringTarget(const std::string& rawPath, std::string* error)
{
	const std::string trimmed = TrimPathValue(rawPath);
	auto logical = ToLogicalPath(trimmed);
	if (!logical) {
		*error = "path is outside the asset root: " + trimmed;
		return std::nullopt;
	}
	auto physical = AssetIO::SourcePath(*logical);
	if (!physical) {
		*error = "scene authoring is disabled in packed (read-only) builds: " + *logical;
		return std::nullopt;
	}
	return AuthoringTarget{ *logical, *physical };
}

// Runtime state and other per-user files live in the user directory; relative names are user-relative.
std::optional<fs::path> ResolveUserPath(const std::string& rawPath, std::string* error)
{
	if (AssetIO::UserDir().empty()) {
		*error = "user directory is not configured";
		return std::nullopt;
	}
	const fs::path path(rawPath);
	return path.is_absolute() ? path : AssetIO::UserDir() / path;
}

bool IsFiniteMatrix(const glm::mat4& m)
{
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (!std::isfinite(m[c][r])) {
				return false;
			}
		}
	}
	return true;
}

float MaxMatrixAbsDelta(const glm::mat4& a, const glm::mat4& b)
{
	float delta = 0.0f;
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			delta = std::max(delta, std::abs(a[c][r] - b[c][r]));
		}
	}
	return delta;
}

bool IsFiniteVec3(const glm::vec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool TryReadVec3Array(const json& parent, const char* key, glm::vec3& value)
{
	if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() != 3) {
		return false;
	}

	const auto& array = parent[key];
	for (size_t i = 0; i < 3; ++i) {
		if (!array[i].is_number()) {
			return false;
		}
	}

	value = glm::vec3(
		array[0].get<float>(),
		array[1].get<float>(),
		array[2].get<float>());
	return IsFiniteVec3(value);
}

glm::vec3 ReadSceneVec3(const json& parent, const char* key, const glm::vec3& fallback, const std::string& nodeName)
{
	glm::vec3 value = fallback;
	if (!parent.contains(key)) {
		return value;
	}

	if (!TryReadVec3Array(parent, key, value)) {
		std::cerr << "[SceneLoader] Warning: node '" << nodeName
			<< "' has invalid " << key << "; using fallback ("
			<< fallback.x << ", " << fallback.y << ", " << fallback.z << ")." << std::endl;
		return fallback;
	}

	return value;
}

glm::vec3 SanitizeSceneScale(const glm::vec3& scale, const std::string& nodeName)
{
	glm::vec3 sanitized = scale;
	bool changed = false;
	for (int i = 0; i < 3; ++i) {
		if (!std::isfinite(sanitized[i]) || std::abs(sanitized[i]) < kMinimumSceneScale) {
			sanitized[i] = 1.0f;
			changed = true;
		}
	}
	if (changed) {
		std::cerr << "[SceneLoader] Warning: node '" << nodeName
			<< "' had invalid or near-zero scale; sanitized to ("
			<< sanitized.x << ", " << sanitized.y << ", " << sanitized.z << ")." << std::endl;
	}
	return sanitized;
}

std::vector<glm::mat4> ComputeModelNodeWorldTransforms(const Scene& model)
{
	std::vector<glm::mat4> world(model.nodes.size(), glm::mat4(1.0f));
	std::vector<uint8_t> state(model.nodes.size(), 0u);

	std::function<glm::mat4(int)> computeWorld = [&](int nodeIndex) -> glm::mat4 {
		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
			return glm::mat4(1.0f);
		}

		if (state[nodeIndex] == 2u) {
			return world[nodeIndex];
		}

		if (state[nodeIndex] == 1u) {
			// Defensive cycle break for malformed content.
			return model.nodes[nodeIndex].localTransform;
		}

		state[nodeIndex] = 1u;
		const Scene::NodeInfo& nodeInfo = model.nodes[nodeIndex];
		glm::mat4 local = IsFiniteMatrix(nodeInfo.localTransform)
			? nodeInfo.localTransform
			: glm::mat4(1.0f);
		glm::mat4 nodeWorld = local;
		if (nodeInfo.parent >= 0) {
			nodeWorld = computeWorld(nodeInfo.parent) * local;
		}
		world[nodeIndex] = nodeWorld;
		state[nodeIndex] = 2u;
		return nodeWorld;
	};

	for (size_t i = 0; i < model.nodes.size(); ++i) {
		computeWorld(static_cast<int>(i));
	}

	return world;
}

float ComputeRenderableBoundingRadius(const Scene& model, const std::vector<uint32_t>& meshIndices, int referenceNodeIndex)
{
	float radius = 1.0f;
	bool foundMesh = false;
	for (uint32_t meshIndex : meshIndices) {
		if (meshIndex >= model.meshes.size()) {
			continue;
		}

		const MeshComponent& mesh = model.meshes[meshIndex];
		float candidate = mesh.boundingRadius;
		if (mesh.boundingVolumeValid) {
			candidate = glm::length(mesh.boundingCenter) + mesh.boundingRadius;
		}

		radius = foundMesh ? std::max(radius, candidate) : candidate;
		foundMesh = true;
	}

	return foundMesh ? radius : 1.0f;
}

// Scene-file material overrides for imported models (docs/nfs2015/configuration.md).
// "material_overrides": [ { "match": "paint", ...fields } ]; match is a case-insensitive substring of
// the glTF material name and "*" matches every material. Overrides mutate the shared model asset, so
// every node using the same model file sees them.
bool ReadOverrideVec(const json& j, const char* key, int components, glm::vec4& out)
{
	if (!j.contains(key) || !j[key].is_array() || static_cast<int>(j[key].size()) < components) {
		return false;
	}
	for (int i = 0; i < components; ++i) {
		out[i] = j[key][i].get<float>();
	}
	return true;
}

std::string ToLowerCopy(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

// Optional number with a fallback. Used for the newer keys so that a null or mistyped value cannot throw.
float OverrideFloat(const json& j, const char* key, float fallback)
{
	return (j.is_object() && j.contains(key) && j[key].is_number()) ? j[key].get<float>() : fallback;
}

// Texture entries of a material override (see docs/editor.md, "Material overrides").
//   "textures": { "<slot>": "textures/file.png" | null }        assign a texture, or clear the slot
//   "texture_transforms": { "<slot>": { "offset": [u, v], "scale": [u, v], "rotation_degrees": r, "texcoord": 0|1 } }
void ApplyTextureOverrides(const json& entry, MeshComponent& mesh, MaterialDesc& d, const std::string& nodeName)
{
	if (entry.contains("textures") && entry["textures"].is_object()) {
		for (auto it = entry["textures"].begin(); it != entry["textures"].end(); ++it) {
			const std::string key = it.key();
			const int slot = MaterialDesc::TextureSlotFromKey(key);
			if (slot < 0) {
				std::cout << "[SceneLoader] material_overrides: unknown texture slot '" << key << "' on node '" << nodeName << "'" << std::endl;
				continue;
			}
			const json& value = it.value();
			if (value.is_string()) {
				const std::string path = value.get<std::string>();
				TexturePtr texture = TextureFactory::FromFile(path, MaterialDesc::TextureSlotIsSRGB(slot));
				if (texture) {
					mesh.TextureForSlot(slot) = texture;
					mesh.textureSourcePaths[slot] = path;
				}
				else {
					std::cout << "[SceneLoader] material_overrides: could not load '" << path << "' for '" << key << "' on node '" << nodeName << "'" << std::endl;
				}
			}
			else if (value.is_null()) {
				mesh.TextureForSlot(slot).reset();
				mesh.textureSourcePaths[slot].clear();
			}
		}
	}
	if (entry.contains("texture_transforms") && entry["texture_transforms"].is_object()) {
		for (auto it = entry["texture_transforms"].begin(); it != entry["texture_transforms"].end(); ++it) {
			const int slot = MaterialDesc::TextureSlotFromKey(it.key());
			const json& xj = it.value();
			if (slot < 0 || !xj.is_object()) {
				continue;
			}
			TextureTransform& xf = d.textureTransforms[slot];
			glm::vec4 v(0.0f);
			if (ReadOverrideVec(xj, "offset", 2, v)) xf.offset = glm::vec2(v);
			if (ReadOverrideVec(xj, "scale", 2, v)) xf.scale = glm::vec2(v);
			xf.rotation = glm::radians(OverrideFloat(xj, "rotation_degrees", glm::degrees(xf.rotation)));
			if (xj.contains("texcoord") && xj["texcoord"].is_number_integer()) {
				d.TexCoordOf(slot) = std::clamp(xj["texcoord"].get<int>(), 0, 1);
			}
		}
	}
}

int ApplyMaterialOverrides(Scene& model, const json& overrides, const std::string& nodeName)
{
	if (!overrides.is_array()) {
		return 0;
	}
	int applied = 0;
	for (const json& entry : overrides) {
		if (!entry.is_object()) {
			continue;
		}
		const std::string match = ToLowerCopy(entry.value("match", std::string("*")));
		// "slot": N limits the entry to the primitive at index N of the model (per-primitive editor overrides).
		const bool hasSlot = entry.contains("slot") && entry["slot"].is_number_integer();
		const int64_t slot = hasSlot ? entry["slot"].get<int64_t>() : -1;
		int hits = 0;
		for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
			MeshComponent& mesh = model.meshes[meshIndex];
			if (hasSlot && static_cast<int64_t>(meshIndex) != slot) {
				continue;
			}
			if (match != "*" && ToLowerCopy(mesh.materialName).find(match) == std::string::npos) {
				continue;
			}
			MaterialDesc d = mesh.GetMaterialDesc();
			glm::vec4 v(0.0f);
			if (ReadOverrideVec(entry, "base_color", 4, v)) d.baseColorFactor = v;
			else if (ReadOverrideVec(entry, "base_color", 3, v)) d.baseColorFactor = glm::vec4(glm::vec3(v), d.baseColorFactor.a);
			if (ReadOverrideVec(entry, "emissive", 3, v)) d.emissiveFactor = glm::vec3(v);
			if (ReadOverrideVec(entry, "specular_color", 3, v)) d.specularColorFactor = glm::vec3(v);
			if (ReadOverrideVec(entry, "attenuation_color", 3, v)) d.attenuationColor = glm::vec3(v);
			d.metallicFactor = entry.value("metallic", d.metallicFactor);
			d.roughnessFactor = entry.value("roughness", d.roughnessFactor);
			d.specularFactor = entry.value("specular", d.specularFactor);
			d.ior = entry.value("ior", d.ior);
			d.clearcoatFactor = entry.value("clearcoat", d.clearcoatFactor);
			d.clearcoatRoughnessFactor = entry.value("clearcoat_roughness", d.clearcoatRoughnessFactor);
			d.emissiveStrength = entry.value("emissive_strength", d.emissiveStrength);
			d.transmissionFactor = entry.value("transmission", d.transmissionFactor);
			d.thicknessFactor = entry.value("thickness", d.thicknessFactor);
			d.normalScale = entry.value("normal_scale", d.normalScale);
			d.occlusionStrength = entry.value("occlusion_strength", d.occlusionStrength);
			d.wetnessResponse = entry.value("wetness_response", d.wetnessResponse);
			d.puddleResponse = entry.value("puddle_response", d.puddleResponse);
			d.rainBeadResponse = entry.value("rain_bead_response", d.rainBeadResponse);
			d.flakeIntensity = entry.value("flake_intensity", d.flakeIntensity);
			d.flakeDensity = entry.value("flake_density", d.flakeDensity);
			d.interiorMapping = entry.value("interior_mapping", d.interiorMapping);
			d.interiorDepth = entry.value("interior_depth", d.interiorDepth);
			d.interiorStrength = entry.value("interior_strength", d.interiorStrength);
			d.interiorSeed = entry.value("interior_seed", d.interiorSeed);
			d.interiorLighting = entry.value("interior_lighting", d.interiorLighting);
			d.interiorBlinds = entry.value("interior_blinds", d.interiorBlinds);
			d.interiorStyle = entry.value("interior_style", d.interiorStyle);
			d.doubleSided = entry.value("double_sided", d.doubleSided);
			d.alphaCutoff = std::clamp(OverrideFloat(entry, "alpha_cutoff", d.alphaCutoff), 0.0f, 1.0f);
			d.unlit = entry.value("unlit", d.unlit);
			if (ReadOverrideVec(entry, "sheen_color", 3, v)) d.sheenColorFactor = glm::vec3(v);
			d.sheenRoughnessFactor = OverrideFloat(entry, "sheen_roughness", d.sheenRoughnessFactor);
			d.anisotropyStrength = OverrideFloat(entry, "anisotropy_strength", d.anisotropyStrength);
			d.anisotropyRotation = OverrideFloat(entry, "anisotropy_rotation", d.anisotropyRotation);
			d.iridescenceFactor = OverrideFloat(entry, "iridescence", d.iridescenceFactor);
			d.iridescenceIor = OverrideFloat(entry, "iridescence_ior", d.iridescenceIor);
			d.iridescenceThicknessMin = OverrideFloat(entry, "iridescence_thickness_min", d.iridescenceThicknessMin);
			d.iridescenceThicknessMax = OverrideFloat(entry, "iridescence_thickness_max", d.iridescenceThicknessMax);
			d.dispersion = OverrideFloat(entry, "dispersion", d.dispersion);
			d.attenuationDistance = OverrideFloat(entry, "attenuation_distance", d.attenuationDistance);
			if (entry.contains("alpha_mode") && entry["alpha_mode"].is_string()) {
				const std::string mode = ToLowerCopy(entry["alpha_mode"].get<std::string>());
				d.alphaMode = mode == "blend" ? MaterialDesc::AlphaMode::Blend
					: (mode == "mask" ? MaterialDesc::AlphaMode::Mask : MaterialDesc::AlphaMode::Opaque);
				d.hasAlpha = d.alphaMode == MaterialDesc::AlphaMode::Blend || d.transmissionFactor > 0.0f;
			}
			// Drop mis-authored maps (e.g. a metallic channel baked into a dielectric part).
			if (entry.contains("disable_textures") && entry["disable_textures"].is_array()) {
				for (const json& tex : entry["disable_textures"]) {
					const std::string t = tex.is_string() ? ToLowerCopy(tex.get<std::string>()) : std::string();
					if (t == "base_color") mesh.diffuseTexture.reset();
					else if (t == "normal") mesh.normalTexture.reset();
					else if (t == "metallic_roughness") mesh.roughnessTexture.reset();
					else if (t == "emissive") mesh.emissiveTexture.reset();
					else if (t == "occlusion") mesh.occlusionTexture.reset();
					else if (t == "specular") mesh.specularTexture.reset();
					else if (t == "specular_color") mesh.specularColorTexture.reset();
					else if (t == "transmission") mesh.transmissionTexture.reset();
				}
			}
			ApplyTextureOverrides(entry, mesh, d, nodeName);
			mesh.SetMaterialDesc(d);
			++hits;
		}
		std::cout << "[SceneLoader] material_overrides '" << match << "' on node '" << nodeName
			<< "' matched " << hits << " mesh(es)" << std::endl;
		applied += hits;
	}
	return applied;
}
}

void SceneLoader::AddReflectionProbes(const json& probes)
{
	if (!m_currentSceneGraph || !probes.is_array()) {
		return;
	}
	auto vec3Of = [](const json& j, const char* key, const glm::vec3& fallback) {
		if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return fallback;
		return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
	};
	for (const json& p : probes) {
		if (!p.is_object()) continue;
		ReflectionProbeDesc desc;
		desc.position = vec3Of(p, "position", desc.position);
		desc.boxMin = vec3Of(p, "box_min", desc.position - glm::vec3(10.0f));
		desc.boxMax = vec3Of(p, "box_max", desc.position + glm::vec3(10.0f));
		desc.blendDistance = p.value("blend_distance", desc.blendDistance);
		desc.intensity = p.value("intensity", desc.intensity);
		m_currentSceneGraph->reflectionProbes.push_back(desc);
	}
}

SceneLoader::SceneLoader(std::shared_ptr<ModelManager> modelManager, std::shared_ptr<PhysicsEngine> physicsEngine, int screenW, int screenH) :
	m_modelManager(modelManager),
	m_physicsEngine(physicsEngine),
	m_screenH(screenH),
	m_screenW(screenW)
{
}

namespace {

// ---- Procedural nodes (C10): live generation or cooked results ------------------------------------

enum class ProceduralSource { Live, Cooked, GeneratorFailed, MissingCooked };

// Generator outputs are identified by generator name, version, params, the declared scene "game" keys and
// the input files (CookedResults). NOX_PROCEDURAL_LIVE=1 ignores cooked results and generates live.
ProceduralSource ResolveProceduralNode(const std::string& generator, const json& params, const json& sceneGame,
	ProceduralGeneratorResult& out, std::string& message)
{
	const ProceduralGenerator* generate = SceneExtensions::FindGenerator(generator);
	const ProceduralGeneratorInfo* info = SceneExtensions::FindGeneratorInfo(generator);
	const char* forceLive = std::getenv("NOX_PROCEDURAL_LIVE");
	const bool liveOnly = forceLive && std::string(forceLive) == "1";
	const json authoredParams = params.is_object() ? params : json::object();

	if (generate && info) {
		if (!liveOnly) {
			std::string error;
			if (auto identity = CookedResults::Identify(generator, *info, authoredParams, sceneGame, &error)) {
				CookedResults::Result cooked;
				std::string loadError;
				const CookedResults::Lookup status = CookedResults::LoadDir(identity->dir, cooked, &loadError);
				if (status == CookedResults::Lookup::Found && cooked.key == identity->key) {
					const std::string authoredOutput = authoredParams.value("output", cooked.defaultOutput);
					if (!CookedResults::MaterializeSideFiles(cooked, authoredOutput, &loadError)) {
						std::cerr << "[SceneLoader] WARNING: cannot restore side files of " << identity->dir << ": " << loadError << std::endl;
					}
					out = CookedResults::ToGenerated(cooked);
					std::cout << "[SceneLoader] Procedural '" << generator << "': cooked result " << identity->dir << std::endl;
					return ProceduralSource::Cooked;
				}
				if (status == CookedResults::Lookup::Invalid) {
					std::cerr << "[SceneLoader] WARNING: ignoring invalid cooked result: " << loadError << std::endl;
				}
			}
			else {
				std::cerr << "[SceneLoader] WARNING: cannot key procedural '" << generator << "': " << error << std::endl;
			}
		}
		out = (*generate)(ProceduralGeneratorInput{ authoredParams, sceneGame });
		return out.ok ? ProceduralSource::Live : ProceduralSource::GeneratorFailed;
	}

	// The generator's code is not compiled into this executable: its cooked result is required.
	CookedResults::Result cooked;
	std::string error;
	if (CookedResults::FindUnregistered(generator, authoredParams, sceneGame, cooked, &error)) {
		const std::string authoredOutput = authoredParams.value("output", cooked.defaultOutput);
		std::string restoreError;
		if (!CookedResults::MaterializeSideFiles(cooked, authoredOutput, &restoreError)) {
			std::cerr << "[SceneLoader] WARNING: cannot restore side files of " << cooked.dir << ": " << restoreError << std::endl;
		}
		out = CookedResults::ToGenerated(cooked);
		std::cout << "[SceneLoader] Procedural '" << generator << "' (not compiled in): cooked result " << cooked.dir << std::endl;
		return ProceduralSource::Cooked;
	}
	message = error;
	return ProceduralSource::MissingCooked;
}

// Generic scene key "interior_atlas" (also "window_room_root"): a directory with atlas_manifest.json.
bool LoadInteriorAtlasKey(SceneKeyInput& in)
{
	if (!in.value.is_string()) {
		std::cerr << "[SceneLoader] Scene key must be a directory path string" << std::endl;
		return false;
	}
	const std::string manifest = in.resolvePath(in.value.get<std::string>() + "/atlas_manifest.json");
	if (manifest.empty()) {
		std::cerr << "[SceneLoader] Interior atlas manifest not found under: " << in.value.get<std::string>() << std::endl;
		return false;
	}
	if (!InteriorAtlasManifest::Load(AssetIO::Parent(manifest), in.renderExtras.interiorAtlas)) return false;
	std::cout << "[SceneLoader] Interior atlas: " << in.renderExtras.interiorAtlas.RoomCount() << " rooms, "
		<< in.renderExtras.interiorAtlas.faceLayers.size() << " faces" << std::endl;
	return true;
}

}  // namespace

SceneLoader::~SceneLoader() {
}

// ---- Scene load job ----------------------------------------------------------------------------------
// A load is a sequence of small steps (Read, Setup, Nodes per root node, SceneKeys, Skybox, Finish). Each step
// runs on the thread that calls StepLoad. Heavy CPU work is moved off that thread: the scene file is read and
// parsed on a worker, and model files are decoded on the ModelManager prefetch worker. A root node is built only
// after its models are decoded, so the build step never waits on a decoder.

namespace {

using LoadClock = std::chrono::steady_clock;

struct ScenePayload {
	bool readOk = false;
	bool parsedOk = false;
	std::optional<std::string> text;
	json parsed;
	std::string parseError;
	// Per root node: the prefab expansion error, or empty. Expanded on the worker (pure JSON and file reads).
	std::vector<std::string> prefabErrors;
};

// Worker: file read and JSON parse. AssetIO is safe to call from any thread.
// JSON files the scene's "game" block names (for example a road network) are read ahead on the worker too:
// the game module reads them while the scene is committed, and a 17 MB file parsed there would stall the frame.
void PrefetchGameJsonFiles(const json& node, int depth)
{
	if (depth > 6) return;
	if (node.is_string()) {
		const std::string text = node.get<std::string>();
		if (text.size() > 5 && text.compare(text.size() - 5, 5, ".json") == 0) {
			AssetIO::PrefetchJson(text);
		}
	}
	else if (node.is_object()) {
		for (const auto& item : node.items()) PrefetchGameJsonFiles(item.value(), depth + 1);
	}
	else if (node.is_array()) {
		for (const json& item : node) PrefetchGameJsonFiles(item, depth + 1);
	}
}

ScenePayload ReadAndParseScene(const std::string& resolvedScenePath)
{
	ScenePayload payload;
	payload.text = AssetIO::ReadText(resolvedScenePath);
	if (!payload.text) return payload;
	payload.readOk = true;
	try {
		payload.parsed = json::parse(*payload.text);
		payload.parsedOk = true;
		if (payload.parsed.is_object() && payload.parsed.contains("game")) {
			PrefetchGameJsonFiles(payload.parsed["game"], 0);
		}
		if (payload.parsed.is_object() && payload.parsed.contains("nodes") && payload.parsed["nodes"].is_array()) {
			for (json& entry : payload.parsed["nodes"]) {
				std::string prefabError;
				if (!nox::prefab::ExpandReferences(entry, &prefabError)) {
					payload.prefabErrors.push_back(prefabError);
				}
				else {
					payload.prefabErrors.emplace_back();
				}
			}
		}
	}
	catch (const std::exception& e) {
		payload.parseError = e.what();
	}
	payload.text.reset();
	return payload;
}

// The model a procedural node will build, when its cooked result exists: the same lookup ResolveProceduralNode does
// first, without the side files it materializes. Empty when the node will run its generator live (no output known).
std::string PeekCookedGltfPath(const json& procedural, const json& sceneGame)
{
	const char* forceLive = std::getenv("NOX_PROCEDURAL_LIVE");
	if (forceLive && std::string(forceLive) == "1") return {};
	const std::string generator = procedural.value("generator", SceneExtensions::DefaultGenerator());
	const json params = procedural.value("params", json::object());
	const json authoredParams = params.is_object() ? params : json::object();
	const ProceduralGeneratorInfo* info = SceneExtensions::FindGeneratorInfo(generator);
	if (!info) {
		// Generator not compiled into this executable: its cooked result is the only source (see ResolveProceduralNode).
		CookedResults::Result cooked;
		std::string unregisteredError;
		if (CookedResults::FindUnregistered(generator, authoredParams, sceneGame, cooked, &unregisteredError)) {
			return CookedResults::ToGenerated(cooked).gltfPath;
		}
		return {};
	}
	std::string error;
	const auto identity = CookedResults::Identify(generator, *info, authoredParams, sceneGame, &error);
	if (!identity) return {};
	CookedResults::Result cooked;
	std::string loadError;
	if (CookedResults::LoadDir(identity->dir, cooked, &loadError) != CookedResults::Lookup::Found || cooked.key != identity->key) {
		return {};
	}
	return CookedResults::ToGenerated(cooked).gltfPath;
}

// Model files a node will load, in load order, resolved the way ProcessNode resolves them. Plain model nodes use
// their path; procedural nodes use the cooked output when one exists. Their decoded copies are the ones LoadModel
// takes. Other models (live generator output) decode on the main thread when they are built.
void CollectPrefetchModels(const json& node, const std::string& sceneDirectory, const json& sceneGame, std::vector<std::string>& out)
{
	if (!node.is_object()) return;
	const std::string typeStr = node.value("type", "model");
	const bool procedural = node.contains("procedural") && node["procedural"].is_object();
	if (SceneLoader::GetNodeType(typeStr) == SceneLoader::NodeType::MODEL) {
		std::string modelPath;
		if (procedural) {
			modelPath = PeekCookedGltfPath(node["procedural"], sceneGame);
		}
		else if (node.contains("path") && node["path"].is_string()) {
			modelPath = node["path"].get<std::string>();
		}
		if (!modelPath.empty()) {
			for (const std::string& candidate : BuildRelativeAssetCandidates(modelPath, sceneDirectory)) {
				if (ReadableFileExists(candidate)) {
					out.push_back(candidate);
					break;
				}
			}
		}
	}
	if (node.contains("children") && node["children"].is_array()) {
		for (const json& child : node["children"]) {
			CollectPrefetchModels(child, sceneDirectory, sceneGame, out);
		}
	}
}

}  // namespace

struct SceneLoader::LoadJob {
	enum class Phase { Read, Setup, Nodes, SceneKeys, Skybox, Finish, Done };
	Phase phase = Phase::Read;
	std::string requestedPath;
	std::string resolvedScenePath;
	std::string sceneName;
	std::string stage = "Reading scene file";
	float fraction = 0.0f;
	std::future<ScenePayload> payload;
	json sceneJson;
	std::shared_ptr<SceneGraph> sceneGraph;
	EntityID rootEntityID = INVALID_ENTITY;
	std::vector<std::vector<std::string>> rootModelPaths;   // per root node: models to have decoded first
	std::vector<std::string> prefabErrors;                  // per root node: prefab expansion error (see ScenePayload)
	std::unique_ptr<SliceTask> rootTask;                    // the root node being built, sliced across frames
	std::unique_ptr<SliceTask> skyboxTask;                  // the environment (skybox and IBL), sliced across frames
	size_t nodeIndex = 0;
	size_t nodeCount = 0;
	std::vector<std::pair<std::string, SceneKeyHandler>> sceneKeys;
	size_t keyIndex = 0;
	// Physics config before the load: the old scene keeps it when the load fails.
	SimulationConfig physicsConfigBeforeLoad;
	LoadClock::time_point started = LoadClock::now();
};

bool SceneLoader::BeginLoad(const std::string& sceneFilePath)
{
	if (m_loadStatus == LoadStatus::Running) {
		std::cerr << "[SceneLoader] A scene load is already running: " << sceneFilePath << std::endl;
		return false;
	}
	if (m_loadStatus == LoadStatus::Ready) {
		CancelLoad();  // an untaken result is dropped
	}
	m_loadError.clear();
	m_job = std::make_unique<LoadJob>();
	// Captured first: a failed load restores the physics config and removes the bodies it added.
	if (m_physicsEngine) {
		m_job->physicsConfigBeforeLoad = m_physicsEngine->GetConfig();
		m_bodiesBeforeLoad = m_physicsEngine->GetBodies();
	}
	m_job->requestedPath = sceneFilePath;
	m_job->resolvedScenePath = ResolveSceneJsonPath(sceneFilePath);
	if (m_job->resolvedScenePath.empty()) {
		FailLoad("Failed to load scene: empty scene path");
		return true;  // the failure is reported by GetLoadStatus / GetLoadError
	}

	m_currentSceneGraph = nullptr;
	m_currentSceneDirectory = fs::path(m_job->resolvedScenePath).parent_path().string();
	m_loadedModelCount = 0;
	m_missingCriticalAssetCount = 0;
	m_loadedSkyboxCount = 0;
	m_disabledSkyboxCount = 0;

	// Report loading started
	GuiEventBus::GetInstance().PublishLoadingStarted();
	GuiEventBus::GetInstance().PublishProgress(0.0f, "Assets");
	std::cout << "[SceneLoader] Loading scene: " << m_job->resolvedScenePath << std::endl;

	m_job->payload = std::async(std::launch::async, ReadAndParseScene, m_job->resolvedScenePath);
	m_loadStatus = LoadStatus::Running;
	return true;
}

SceneLoader::LoadStatus SceneLoader::StepLoad(double budgetMs)
{
	if (m_loadStatus != LoadStatus::Running || !m_job) return m_loadStatus;
	const bool unlimited = budgetMs < 0.0;
	const auto deadline = LoadClock::now() + std::chrono::duration_cast<LoadClock::duration>(
		std::chrono::duration<double, std::milli>(unlimited ? 0.0 : budgetMs));
	while (m_loadStatus == LoadStatus::Running) {
		const double remainingMs = unlimited ? -1.0 : std::max(0.0, std::chrono::duration<double, std::milli>(deadline - LoadClock::now()).count());
		const bool progressed = StepLoadOnce(*m_job, remainingMs);
		if (!progressed) {
			// Waiting on a worker. A budgeted caller goes back to its frame; a blocking caller sleeps briefly.
			if (!unlimited) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!unlimited && LoadClock::now() >= deadline) break;
	}
	return m_loadStatus;
}

SceneLoader::LoadStatus SceneLoader::GetLoadStatus() const
{
	return m_loadStatus;
}

SceneLoader::LoadProgress SceneLoader::GetLoadProgress() const
{
	LoadProgress progress;
	if (!m_job) return progress;
	progress.fraction = m_job->fraction;
	progress.stage = m_job->stage;
	progress.sceneName = m_job->sceneName;
	progress.sceneFile = m_job->requestedPath;
	return progress;
}

void SceneLoader::FailLoad(const std::string& message)
{
	m_loadError = message;
	std::cerr << "[SceneLoader] " << message << std::endl;
	GuiEventBus::GetInstance().PublishError(message);
	m_currentSceneGraph = nullptr;
	m_currentSceneDirectory.clear();
	if (m_job) {
		m_job->sceneGraph.reset();
		m_job->phase = LoadJob::Phase::Done;
		if (m_physicsEngine) m_physicsEngine->SetConfig(m_job->physicsConfigBeforeLoad);
	}
	RetirePhysicsBodies(false);
	if (m_modelManager) m_modelManager->CancelPrefetch();
	m_loadStatus = LoadStatus::Failed;
}

void SceneLoader::CancelLoad()
{
	if (m_loadStatus != LoadStatus::Running && m_loadStatus != LoadStatus::Ready) return;
	if (m_job) {
		if (m_job->payload.valid()) m_job->payload.wait();
		m_job->sceneGraph.reset();
		if (m_physicsEngine) m_physicsEngine->SetConfig(m_job->physicsConfigBeforeLoad);
	}
	m_currentSceneGraph = nullptr;
	m_currentSceneDirectory.clear();
	RetirePhysicsBodies(false);
	if (m_modelManager) m_modelManager->CancelPrefetch();
	m_job.reset();
	m_loadStatus = LoadStatus::Idle;
}

std::shared_ptr<SceneGraph> SceneLoader::TakeLoadedScene()
{
	if (m_loadStatus != LoadStatus::Ready || !m_job) return nullptr;
	std::shared_ptr<SceneGraph> graph = std::move(m_job->sceneGraph);
	// The scene being replaced is gone once its successor is taken: remove its physics bodies.
	RetirePhysicsBodies(true);
	m_job.reset();
	m_loadStatus = LoadStatus::Idle;
	return graph;
}

// Removes physics bodies from the engine: the replaced scene's (removeReplaced) or the bodies the load added.
void SceneLoader::RetirePhysicsBodies(bool removeReplaced)
{
	if (!m_physicsEngine) {
		m_bodiesBeforeLoad.clear();
		return;
	}
	std::unordered_set<const RigidBody*> before;
	for (const auto& body : m_bodiesBeforeLoad) before.insert(body.get());
	std::vector<std::shared_ptr<RigidBody>> toRemove;
	for (const auto& body : m_physicsEngine->GetBodies()) {
		const bool wasBefore = before.count(body.get()) != 0;
		if (wasBefore == removeReplaced) toRemove.push_back(body);
	}
	for (const auto& body : toRemove) {
		m_physicsEngine->RemoveBody(body);
	}
	m_bodiesBeforeLoad.clear();
}

std::shared_ptr<SceneGraph> SceneLoader::LoadScene(const std::string& sceneFilePath) {
	if (!BeginLoad(sceneFilePath)) return nullptr;
	StepLoad(-1.0);
	return TakeLoadedScene();
}

bool SceneLoader::StepLoadOnce(LoadJob& job, double budgetMs)
{
	switch (job.phase) {
	case LoadJob::Phase::Read: {
		if (job.payload.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			return false;
		}
		ScenePayload payload = job.payload.get();
		if (!payload.readOk) {
			std::cerr << "[SceneLoader] Failed to open scene file: " << job.resolvedScenePath << std::endl;
			FailLoad("Failed to open scene file: " + job.resolvedScenePath);
			return true;
		}
		if (!payload.parsedOk) {
			std::cerr << "[SceneLoader] Failed to parse scene file '" << job.resolvedScenePath
				<< "': " << payload.parseError << std::endl;
			FailLoad("Failed to parse scene file: " + job.resolvedScenePath);
			return true;
		}
		if (!payload.parsed.is_object()) {
			FailLoad("Invalid scene file: root JSON value must be an object: " + job.resolvedScenePath);
			return true;
		}
		job.sceneJson = std::move(payload.parsed);
		job.prefabErrors = std::move(payload.prefabErrors);
		job.phase = LoadJob::Phase::Setup;
		job.fraction = 0.05f;
		return true;
	}

	case LoadJob::Phase::Setup: {
		json& sceneJson = job.sceneJson;
		// Set the scene name if provided.
		job.sceneName = sceneJson.value("scene_name", "Unnamed Scene");
		job.sceneGraph = std::make_shared<SceneGraph>();
		job.sceneGraph->SetSceneName(job.sceneName);
		// Set the current scene graph BEFORE processing nodes
		// This is required for CreateECSEntity to work properly
		m_currentSceneGraph = job.sceneGraph.get();

		// Set the scenes exposure and gamma values if provided.
		job.sceneGraph->m_exposure = sceneJson.value("exposure", 1.0f);
		job.sceneGraph->m_gamma = sceneJson.value("gamma", 2.2f);
		m_sceneGameJson = sceneJson.value("game", nlohmann::json::object());

		// Physics settings: config.json defaults with the scene's "physics" block on top. Applied before the nodes
		// below create their bodies. A scene without a block gets the defaults, so the previous scene's values do not carry over.
		if (m_physicsEngine) {
			SimulationConfig physicsConfig = m_physicsEngine->GetDefaultConfig();
			if (sceneJson.contains("physics")) {
				try {
					physicsConfig.applyJson(sceneJson["physics"]);
				}
				catch (const std::exception& e) {
					std::cerr << "[SceneLoader] Ignoring invalid physics block in " << job.resolvedScenePath << ": " << e.what() << std::endl;
					physicsConfig = m_physicsEngine->GetDefaultConfig();
				}
			}
			m_physicsEngine->SetConfig(physicsConfig);
		}

		if (sceneJson.contains("reflection_probes")) {
			AddReflectionProbes(sceneJson["reflection_probes"]);
		}

		// Look for a hierarchical "nodes" array.
		if (!(sceneJson.contains("nodes") && sceneJson["nodes"].is_array())) {
			FailLoad("Invalid scene file format. 'nodes' array is missing: " + job.resolvedScenePath);
			return true;
		}
		const json& nodes = sceneJson["nodes"];
		job.nodeCount = nodes.size();
		job.rootModelPaths.assign(job.nodeCount, {});
		std::vector<std::string> allModels;
		for (size_t i = 0; i < job.nodeCount; ++i) {
			CollectPrefetchModels(nodes[i], m_currentSceneDirectory, m_sceneGameJson, job.rootModelPaths[i]);
			allModels.insert(allModels.end(), job.rootModelPaths[i].begin(), job.rootModelPaths[i].end());
		}
		if (m_modelManager) {
			m_modelManager->QueuePrefetch(allModels);
		}
		// Root entity ID for the hierarchy
		auto root = job.sceneGraph->GetRoot();
		job.rootEntityID = root ? root->GetEntityID() : INVALID_ENTITY;
		job.phase = LoadJob::Phase::Nodes;
		job.stage = "Building scene nodes";
		job.fraction = 0.05f;
		return true;
	}

	case LoadJob::Phase::Nodes: {
		if (job.nodeIndex >= job.nodeCount) {
			std::cout << "[SceneLoader] Loaded " << job.nodeCount << " root nodes with ECS entities" << std::endl;
			// Registered top-level keys (application content such as an authored interior atlas). They run
			// after the nodes so a procedural generator cannot overwrite what the scene states explicitly.
			// Built-in generic keys first; registered application keys follow.
			job.sceneKeys = {
				{ "interior_atlas", LoadInteriorAtlasKey },
				{ "window_room_root", LoadInteriorAtlasKey },   // compatibility spelling
			};
			for (const auto& entry : SceneExtensions::SceneKeys()) job.sceneKeys.push_back(entry);
			job.phase = LoadJob::Phase::SceneKeys;
			return true;
		}
		// The models of this root node must be decoded before it is built.
		const auto decoder = m_modelManager ? m_modelManager->GetPrefetchProgress() : ModelManager::PrefetchProgress{};
		std::vector<std::string> awaiting;
		if (m_modelManager) {
			for (const std::string& path : job.rootModelPaths[job.nodeIndex]) {
				if (m_modelManager->IsPrefetchAwaiting(path)) awaiting.push_back(path);
			}
		}
		if (!awaiting.empty()) {
			job.stage = "Decoding models (" + std::to_string(decoder.decoded) + "/" + std::to_string(decoder.queued) + ")";
			job.fraction = 0.05f + 0.8f * 0.5f * (decoder.queued > 0 ? static_cast<float>(decoder.decoded) / decoder.queued : 1.0f);
			// Wait for the decoders inside this frame's budget rather than returning early: an idle frame is wasted time.
			if (!m_modelManager->WaitWhileAwaiting(awaiting, budgetMs)) {
				return false;
			}
		}
		// Build the root node as a slice task: its model uploads yield to the frame when the budget is spent.
		if (!job.rootTask) {
			job.rootTask = std::make_unique<SliceTask>([this, &job]() {
				if (!job.prefabErrors[job.nodeIndex].empty()) {
					++m_missingCriticalAssetCount;
					std::cerr << "[SceneLoader] ERROR: prefab reference in '" << job.resolvedScenePath << "' failed: "
						<< job.prefabErrors[job.nodeIndex] << std::endl;
					return;
				}
				json& nodeEntry = job.sceneJson["nodes"][job.nodeIndex];
				auto node = ProcessNodeRecursive(nodeEntry, job.rootEntityID);
				if (node) {
					job.sceneGraph->GetRoot()->AddChild(node);
				}
			});
		}
		if (!job.rootTask->Resume(budgetMs)) {
			job.stage = "Building scene nodes (" + std::to_string(job.nodeIndex) + "/" + std::to_string(job.nodeCount) + ")";
			return true;
		}
		job.rootTask.reset();
		++job.nodeIndex;
		job.stage = "Building scene nodes (" + std::to_string(job.nodeIndex) + "/" + std::to_string(job.nodeCount) + ")";
		const float nodeFraction = job.nodeCount > 0 ? static_cast<float>(job.nodeIndex) / job.nodeCount : 1.0f;
		const float decodeFraction = decoder.queued > 0 ? static_cast<float>(decoder.decoded) / decoder.queued : 1.0f;
		job.fraction = 0.05f + 0.8f * (0.5f * decodeFraction + 0.5f * nodeFraction);
		return true;
	}

	case LoadJob::Phase::SceneKeys: {
		if (job.keyIndex < job.sceneKeys.size()) {
			const auto& [key, handler] = job.sceneKeys[job.keyIndex++];
			job.stage = "Loading scene content (" + key + ")";
			job.fraction = 0.85f;
			if (!job.sceneJson.contains(key)) return true;
			SceneKeyInput input{ job.sceneJson[key],
				[this](const std::string& path) {
					bool resolved = false;
					const std::string full = ResolveReadableAssetPath(TrimPathValue(path), m_currentSceneDirectory,
						"scene extension asset", false, &resolved);
					return resolved ? full : std::string();
				},
				job.sceneGraph->renderExtras };
			if (handler(input)) {
				job.sceneGraph->renderExtras.revision = NextRenderExtrasRevision();
			}
			else {
				++m_missingCriticalAssetCount;
				std::cerr << "[SceneLoader] Scene key '" << key << "' failed to load its content" << std::endl;
			}
			return true;
		}
		if (m_missingCriticalAssetCount > 0) {
			std::cerr << "[SceneLoader] Aborting scene load for '" << job.resolvedScenePath
				<< "' due to " << m_missingCriticalAssetCount
				<< " missing required asset(s)." << std::endl;
			FailLoad("Scene '" + job.resolvedScenePath + "' has " + std::to_string(m_missingCriticalAssetCount)
				+ " missing required asset(s)");
			return true;
		}
		job.phase = LoadJob::Phase::Skybox;
		return true;
	}

	case LoadJob::Phase::Skybox: {
		// The environment (HDR decode, cube conversion, IBL passes) runs as a slice so it spreads over frames.
		if (!job.skyboxTask) {
			job.stage = "Loading environment";
			job.fraction = 0.9f;
			job.skyboxTask = std::make_unique<SliceTask>([this, &job]() {
		json& sceneJson = job.sceneJson;
		const std::string& sceneName = job.sceneName;
		std::shared_ptr<SceneGraph>& sceneGraph = job.sceneGraph;
		// Initialize the skybox if provided. Either a path string, or an object:
		// { "path", "fallback", "rotation_deg", "intensity", "max_radiance", "ground": { "color", "blend_start_deg", "blend_end_deg" } }
	if (sceneJson.contains("skybox")) {
		const bool skyboxIsObject = sceneJson["skybox"].is_object();
		const nlohmann::json skyboxNode = skyboxIsObject ? sceneJson["skybox"] : nlohmann::json::object();
		EnvironmentMapOptions envOptions;
		if (skyboxIsObject) {
			std::string envWarnings;
			envOptions = ParseEnvironmentMapOptions(skyboxNode, &envWarnings);
			if (!envWarnings.empty()) {
				std::cout << "[SceneLoader] Skybox options adjusted for scene '" << sceneName << "':" << envWarnings << std::endl;
			}
		}
		const std::string hdrPath = TrimPathValue(skyboxIsObject
			? (skyboxNode.contains("path") && skyboxNode["path"].is_string() ? skyboxNode["path"].get<std::string>() : std::string{})
			: sceneJson.value("skybox", ""));
		const std::string fallbackPath = (skyboxIsObject && skyboxNode.contains("fallback") && skyboxNode["fallback"].is_string())
			? TrimPathValue(skyboxNode["fallback"].get<std::string>())
			: std::string{};

		if (hdrPath.empty() && fallbackPath.empty()) {
			++m_disabledSkyboxCount;
			std::cout << "[SceneLoader] Skybox disabled for scene '" << sceneName
				<< "' because the skybox path is empty." << std::endl;
		}
		else {
			bool skyboxResolved = false;
			const std::string resolvedHdrPath = ResolveReadableAssetPath(
				hdrPath,
				m_currentSceneDirectory,
				"skybox",
				false,
				&skyboxResolved);

			bool fallbackResolved = false;
			std::string resolvedFallbackPath;
			if (!skyboxResolved && !fallbackPath.empty()) {
				resolvedFallbackPath = ResolveReadableAssetPath(
					fallbackPath,
					m_currentSceneDirectory,
					"skybox fallback",
					false,
					&fallbackResolved);
				if (fallbackResolved) {
					std::cout << "[SceneLoader] Skybox '" << hdrPath << "' unavailable, using fallback '"
						<< resolvedFallbackPath << "'" << std::endl;
				}
			}

			if (!skyboxResolved && !fallbackResolved) {
				++m_disabledSkyboxCount;
				std::cout << "[SceneLoader] Skybox disabled for scene '" << sceneName
					<< "' because the HDR asset could not be resolved: " << hdrPath << std::endl;
			}
			else {
				const std::string primaryPath = skyboxResolved ? resolvedHdrPath : resolvedFallbackPath;
				auto initSkybox = [&](const std::string& path) -> std::shared_ptr<Skybox> {
					auto candidate = std::make_shared<Skybox>();
					if (!candidate->Init(path,
						"shaders/equirect2cube_vert.glsl",
						"shaders/equirect2cube_frag.glsl",
						"shaders/skybox_vert.glsl",
						"shaders/skybox_frag.glsl",
						m_screenW,
						m_screenH,
						envOptions)) {
						std::cerr << "[SceneLoader] Failed to load skybox: " << path << "\n";
						return nullptr;
					}
					return candidate;
				};

				std::shared_ptr<Skybox> sb = initSkybox(primaryPath);
				if (!sb && skyboxResolved && fallbackResolved && resolvedFallbackPath != resolvedHdrPath) {
					std::cout << "[SceneLoader] Retrying skybox with fallback '" << resolvedFallbackPath << "'" << std::endl;
					sb = initSkybox(resolvedFallbackPath);
				}
				if (!sb) {
					++m_disabledSkyboxCount;
				}
				else {
					++m_loadedSkyboxCount;
					std::cout << "[SceneLoader] Loading skybox: " << primaryPath << std::endl;
					sceneGraph->SetSkybox(sb);
				}
			}
		}
	}
			});
		}
		if (!job.skyboxTask->Resume(budgetMs)) {
			return true;
		}
		job.skyboxTask.reset();
		job.phase = LoadJob::Phase::Finish;
		return true;
	}

	case LoadJob::Phase::Finish: {
		json& sceneJson = job.sceneJson;
		const std::string& sceneName = job.sceneName;
		std::shared_ptr<SceneGraph>& sceneGraph = job.sceneGraph;
		// Per-scene physics flag (default true)
		bool physicsEnabled = sceneJson.value("physics_enabled", true);
		sceneGraph->SetPhysicsEnabled(physicsEnabled);

		// Publish authored scene transforms before the scene is handed back to callers.
		sceneGraph->UpdateAllTransforms();
		// Clear the scene graph reference
		m_currentSceneGraph = nullptr;
		m_currentSceneDirectory.clear();

		// Log ECS statistics
		auto* componentManager = sceneGraph->GetComponentManager();
		std::cout << "[SceneLoader] Scene load summary: scene='" << sceneName
			<< "', path='" << job.resolvedScenePath
			<< "', authoredRootNodes=" << sceneJson["nodes"].size()
			<< ", models=" << m_loadedModelCount
			<< ", transforms=" << componentManager->GetTransformPool().Size()
			<< ", maxTransformID=" << componentManager->GetMaxAllocatedTransformID()
			<< ", renderables=" << componentManager->GetRenderablePool().Size()
			<< ", lights=" << componentManager->GetLightPool().Size()
			<< ", cameras=" << componentManager->GetCameraPool().Size()
			<< ", skyboxesLoaded=" << m_loadedSkyboxCount
			<< ", skyboxesDisabled=" << m_disabledSkyboxCount
			<< ", loadMs=" << std::chrono::duration<double, std::milli>(LoadClock::now() - job.started).count()
			<< std::endl;

		if (m_modelManager) m_modelManager->CancelPrefetch();
		job.fraction = 1.0f;
		job.stage = "Done";
		job.phase = LoadJob::Phase::Done;
		m_loadStatus = LoadStatus::Ready;
		return true;
	}

	case LoadJob::Phase::Done:
		return false;
	}
	return false;
}


// Save scene state back to JSON file
bool SceneLoader::SaveScene(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sceneFilePath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot save: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Saving scene to: " << sceneFilePath << std::endl;
	std::string targetError;
	const auto target = ResolveAuthoringTarget(sceneFilePath, &targetError);
	if (!target) {
		std::cerr << "[SceneLoader] Cannot save scene: " << targetError << std::endl;
		return false;
	}

	try {
		nlohmann::json sceneJson = BuildSceneJson(sceneGraph, target->logical);

		// render_settings is written from the live renderer, not from the file copy BuildSceneJson preserves (that copy is
		// what the scene loaded with; edits made in the editor or by scripts would be lost on save).
		if (RenderContext* live = nox::reflect::Services().renderContext ? nox::reflect::Services().renderContext() : nullptr) {
			const json existingSettings = sceneJson.contains("render_settings") ? sceneJson["render_settings"] : json::object();
			json settings = SceneRenderSettings::ToJson(*live, existingSettings);
			if (!settings.empty() || sceneJson.contains("render_settings")) {
				sceneJson["render_settings"] = settings;
			}
		}

		// Write to the loose source tree with pretty formatting
		std::ofstream outFile(target->physical);
		if (!outFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open file for writing: " << target->physical.string() << std::endl;
			return false;
		}

		outFile << sceneJson.dump(2); // Indent with 2 spaces for readability
		outFile.close();

		std::cout << "[SceneLoader] Successfully saved scene with " << sceneJson["nodes"].size() << " nodes" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error saving scene: " << e.what() << std::endl;
		return false;
	}
}

bool SceneLoader::SaveSceneCopy(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sourceLogicalPath,
	const std::string& physicalOutPath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) return false;
	try {
		const nlohmann::json sceneJson = BuildSceneJson(sceneGraph, sourceLogicalPath);
		std::ofstream outFile(physicalOutPath, std::ios::binary);
		if (!outFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open copy for writing: " << physicalOutPath << std::endl;
			return false;
		}
		outFile << sceneJson.dump(2);
		return static_cast<bool>(outFile);
	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error writing scene copy: " << e.what() << std::endl;
		return false;
	}
}

nlohmann::json SceneLoader::BuildSceneJson(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& sourceLogicalPath) {
	nlohmann::json sceneJson;

	// The file being overwritten (or the source copy): top-level keys it did not have are not added while they hold
	// their loader defaults, and an unchanged authored value is written back as it was authored.
	json existing = json::object();
	if (!sourceLogicalPath.empty()) {
		if (auto existingText = AssetIO::ReadText(sourceLogicalPath)) {
			const json parsed = json::parse(*existingText, nullptr, false);
			if (parsed.is_object()) existing = parsed;
		}
	}
	// Serialize scene metadata
	sceneJson["scene_name"] = sceneGraph->GetSceneName();
	auto writeAuthoredNumber = [&](const char* key, float value, float defaultValue) {
		if (existing.contains(key) && existing[key].is_number()) {
			const float authored = existing[key].get<float>();
			sceneJson[key] = std::fabs(authored - value) <= 1e-6f * std::max(1.0f, std::fabs(value)) ? existing[key] : json(value);
		}
		else if (value != defaultValue) {
			sceneJson[key] = value;
		}
	};
	writeAuthoredNumber("exposure", sceneGraph->m_exposure, 1.0f);
	writeAuthoredNumber("gamma", sceneGraph->m_gamma, 2.2f);
	const bool physicsEnabled = sceneGraph->IsPhysicsEnabled();
	if (existing.contains("physics_enabled") || !physicsEnabled) {
		sceneJson["physics_enabled"] = physicsEnabled;
	}
	// Physics tuning: only the authored values that differ from config.json. Nothing is written when they all match
	// the defaults, so scenes without a block keep their bytes. Runtime changes made by scripts are not saved.
	if (m_physicsEngine) {
		const json physicsBlock = m_physicsEngine->GetAuthoredConfig().ToSceneJson(m_physicsEngine->GetDefaultConfig());
		if (!physicsBlock.empty()) sceneJson["physics"] = physicsBlock;
	}

	// Serialize skybox if present
	// The authored skybox object (rotation, intensity, fallback) is preserved below from the
	// file being overwritten; a scene without one records the path its skybox was loaded from.
	if (sceneGraph->GetSkybox() && !sceneGraph->GetSkybox()->GetHDRPath().empty()) {
		sceneJson["skybox"] = sceneGraph->GetSkybox()->GetHDRPath();
	}

	// Serialize all root-level nodes and their children. Prefab instances are written as references.
	nlohmann::json nodesArray = nlohmann::json::array();
	auto root = sceneGraph->GetRoot();
	for (const auto& child : root->children) {
		if (child) {
			nlohmann::json nodeJson = SerializeNodeTree(child, true, false);
			if (!nodeJson.is_null()) {
				nodesArray.push_back(nodeJson);
			}
		}
	}
	sceneJson["nodes"] = nodesArray;

	// Preserve authored top-level data the runtime does not model (render_settings, benchmark,
	// camera, reflection_probes, the real skybox path) from the file being overwritten.
	for (auto it = existing.begin(); it != existing.end(); ++it) {
		const std::string& key = it.key();
		if (key == "nodes" || key == "scene_name" || key == "exposure" || key == "gamma" ||
			key == "physics_enabled" || key == "physics") {
			continue;
		}
		sceneJson[key] = it.value();
	}
	return sceneJson;
}

nlohmann::json SceneLoader::SerializeNodeTree(const std::shared_ptr<SceneNode>& node, bool references, bool isRoot) {
	if (!node) {
		return nlohmann::json();
	}
	if (references && !isRoot && !node->GetPrefabPath().empty()) {
		return SerializePrefabReference(node);
	}

	// Serialize this node
	nlohmann::json nodeJson = SerializeNode(node);

	// Serialize children recursively
	if (!node->children.empty()) {
		nlohmann::json childrenArray = nlohmann::json::array();

		for (const auto& child : node->children) {
			// Generated nodes and nodes imported from the model file (glTF node index set) are
			// rebuilt on load, so writing them would duplicate the model's hierarchy.
			if (child && !child->IsGenerated() && child->nodeIndex < 0) {
				nlohmann::json childJson = SerializeNodeTree(child, references, false);
				if (!childJson.is_null()) {
					childrenArray.push_back(childJson);
				}
			}
		}

		if (!childrenArray.empty()) {
			nodeJson["children"] = childrenArray;
		}
	}

	return nodeJson;
}

nlohmann::json SceneLoader::SerializeNodeOwn(const std::shared_ptr<SceneNode>& node) {
	return node ? SerializeNode(node) : nlohmann::json();
}

nlohmann::json SceneLoader::SerializePrefabReference(const std::shared_ptr<SceneNode>& node) {
	const std::string path = node->GetPrefabPath();
	const nlohmann::json instanceFlat = SerializeNodeTree(node, false, true);
	const std::optional<nlohmann::json> baseline = PrefabBaseline(path);
	if (!baseline) {
		std::cerr << "[SceneLoader] Warning: prefab '" << path << "' cannot be read; node '" << node->GetName()
			<< "' is saved as plain nodes" << std::endl;
		return instanceFlat;
	}
	return nox::prefab::MakeReference(path, instanceFlat, *baseline);
}

std::optional<nlohmann::json> SceneLoader::PrefabBaseline(const std::string& logicalPath) {
	std::string error;
	std::optional<nlohmann::json> prefab = nox::prefab::ReadPrefabJson(logicalPath, &error);
	if (!prefab) return std::nullopt;
	nlohmann::json expanded = std::move(*prefab);
	if (!nox::prefab::ExpandReferences(expanded, &error)) return std::nullopt;

	// Instantiate in a scratch graph with a scratch physics world, so the live scene is not touched.
	SceneGraph* savedGraph = m_currentSceneGraph;
	const std::string savedDirectory = m_currentSceneDirectory;
	const size_t savedMissing = m_missingCriticalAssetCount;
	const bool savedScratch = m_scratchInstance;
	const std::shared_ptr<PhysicsEngine> savedPhysics = m_physicsEngine;
	auto scratch = std::make_shared<SceneGraph>();
	m_scratchInstance = true;
	m_physicsEngine = std::make_shared<PhysicsEngine>();
	auto node = InstantiateExpanded(*scratch, expanded, scratch->GetRoot(), logicalPath);
	std::optional<nlohmann::json> flat;
	if (node) flat = SerializeNodeTree(node, false, true);
	m_physicsEngine = savedPhysics;
	m_scratchInstance = savedScratch;
	m_missingCriticalAssetCount = savedMissing;
	m_currentSceneDirectory = savedDirectory;
	m_currentSceneGraph = savedGraph;
	return flat;
}

namespace {

// Save merge for one object of a node (SceneNode::SetAuthoredBaseline). authored: keys as the file wrote them. loaded:
// the same keys as the loader left the node. runtime: what the saver writes now.
json MergeAuthoredObject(const json& authored, const json& loaded, const json& runtime) {
	json out = json::object();
	for (auto it = runtime.begin(); it != runtime.end(); ++it) {
		const std::string& key = it.key();
		const json* av = (authored.is_object() && authored.contains(key)) ? &authored[key] : nullptr;
		const json* lv = (loaded.is_object() && loaded.contains(key)) ? &loaded[key] : nullptr;
		if (lv && *lv == it.value()) {
			// Unchanged: the authored value (exact), or nothing when the file did not have the key.
			if (av) out[key] = *av;
			continue;
		}
		if (av && av->is_object() && lv && lv->is_object() && it.value().is_object()) {
			out[key] = MergeAuthoredObject(*av, *lv, it.value());
		}
		else {
			out[key] = it.value();
		}
	}
	// Authored keys the saver never wrote are kept. Keys the loader wrote that the saver no longer writes are dropped.
	if (authored.is_object()) {
		for (auto it = authored.begin(); it != authored.end(); ++it) {
			const std::string& key = it.key();
			if (!loaded.contains(key) && !runtime.contains(key)) out[key] = it.value();
		}
	}
	return out;
}

} // namespace

nlohmann::json SceneLoader::SerializeNode(const std::shared_ptr<SceneNode>& node) {
	if (!node) {
		return nlohmann::json();
	}

	nlohmann::json nodeJson;

	// Determine and set node type
	SceneNode::NODE_TYPE nodeType = node->GetNodeType();
	switch (nodeType) {
	case SceneNode::MODEL:
		nodeJson["type"] = "model";
		break;
	case SceneNode::LIGHT:
		nodeJson["type"] = "light";
		break;
	case SceneNode::AUDIO:
		nodeJson["type"] = "audio";
		break;
	case SceneNode::CAMERA:
		nodeJson["type"] = "camera";
		break;
	case SceneNode::GUI:
		nodeJson["type"] = "gui";
		break;
	case SceneNode::LPV_VOLUME:
		nodeJson["type"] = "lpv_volume";
		break;
	case SceneNode::SKELETAL:
		nodeJson["type"] = "skeletal";
		break;
	case SceneNode::FOLDER:
		nodeJson["type"] = "folder";
		break;
	default:
		nodeJson["type"] = "node";
		break;
	}

	// A folder has no transform of its own (identity, not written); its children keep the transforms they have.
	if (nodeType != SceneNode::FOLDER) {
		glm::vec3 position = node->GetPosition();
		glm::vec3 rotation = node->GetRotation();
		glm::vec3 scale = node->GetScale();

		// Rounded to a fixed step, so float round-off (radians/degrees conversions, matrix decomposition of edited nodes)
		// does not change the file on every save/load cycle.
		nodeJson["position"] = { QuantizeTransform(position.x, 1e6), QuantizeTransform(position.y, 1e6), QuantizeTransform(position.z, 1e6) };

		// Convert rotation from radians to degrees for saving
		nodeJson["rotation"] = { QuantizeTransform(glm::degrees(rotation.x), 1e4), QuantizeTransform(glm::degrees(rotation.y), 1e4),
			QuantizeTransform(glm::degrees(rotation.z), 1e4) };

		nodeJson["scale"] = { QuantizeTransform(scale.x, 1e6), QuantizeTransform(scale.y, 1e6), QuantizeTransform(scale.z, 1e6) };
	}

	// Serialize node name
	std::string nodeName = node->GetName();
	if (!nodeName.empty()) {
		nodeJson["name"] = nodeName;
	}

	// Serialize type-specific properties
	switch (nodeType) {
	case SceneNode::MODEL: {
		// Serialize model path using ModelManager
		auto model = node->GetModel();
		if (model && m_modelManager) {
			std::string modelPath = m_modelManager->GetModelPath(model.get());
			if (!modelPath.empty()) {
				nodeJson["path"] = modelPath;
			}
			else {
				std::cout << "[SceneLoader] Warning: Could not find model path for node '"
					<< nodeName << "'" << std::endl;
			}
		}
		break;
	}

	case SceneNode::LIGHT: {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			nodeJson["light"] = SerializeLightProperties(lightNode);
		}
		break;
	}

	case SceneNode::AUDIO: {
		auto audioNode = std::dynamic_pointer_cast<AudioNode>(node);
		if (audioNode) {
			// Serialize audio properties
			nodeJson["pitch"] = audioNode->getPitch();
			nodeJson["volume"] = audioNode->getVolume();
			nodeJson["hearing_distance"] = audioNode->getHearingDistance();
			nodeJson["loop"] = audioNode->isLooping();
			nodeJson["is3d"] = audioNode->is3D();
			nodeJson["autoplay"] = audioNode->getAutoplay();
			// Distance curve and occlusion are written only when they differ from the defaults, so existing scenes
			// round-trip unchanged.
			if (audioNode->getAttenuation() != AudioAttenuation::Linear) {
				nodeJson["attenuation"] = AudioAttenuationName(audioNode->getAttenuation());
			}
			if (audioNode->getMinDistance() > 0.0f) nodeJson["min_distance"] = audioNode->getMinDistance();
			if (audioNode->getRolloff() != 1.0f) nodeJson["rolloff"] = audioNode->getRolloff();
			if (audioNode->getOcclusion()) nodeJson["occlusion"] = true;
			if (!audioNode->getSoundFilePath().empty()) {
				nodeJson["sound"] = audioNode->getSoundFilePath();
			}
		}
		break;
	}

	case SceneNode::GUI: {
		auto guiNode = std::dynamic_pointer_cast<GuiNode>(node);
		if (guiNode) {
			nodeJson["elements"] = SerializeGuiElements(guiNode);
			if (!guiNode->GetFontPath().empty()) {
				nodeJson["font_path"] = guiNode->GetFontPath();
				nodeJson["font_size"] = guiNode->GetFontSize();
			}
		}
		break;
	}

	case SceneNode::LPV_VOLUME: {
		// Serialize LPV volume data
		const auto& lpvData = node->GetLPVVolumeData();

		nlohmann::json lpvJson;
		lpvJson["center"] = { lpvData.center.x, lpvData.center.y, lpvData.center.z };
		lpvJson["extent"] = { lpvData.extent.x, lpvData.extent.y, lpvData.extent.z };
		lpvJson["voxel_size"] = lpvData.voxelSize;
		lpvJson["grid_resolution"] = lpvData.gridResolution;

		// Serialize quaternion orientation
		lpvJson["orientation"] = {
			lpvData.orientation.w,
			lpvData.orientation.x,
			lpvData.orientation.y,
			lpvData.orientation.z
		};

		nodeJson["lpv_data"] = lpvJson;
		break;
	}

	default:
		break;
	}

	// Serialize collider if present (for any node type, not just MODEL)
	// This allows nodes without models to still have physics colliders
	if (node->GetRigidBody()) {
		nodeJson["collider"] = SerializeCollider(node);
	}

	// Keys the file authored (SceneNode::SetAuthoredBaseline): an unchanged value is written back as authored, a key the
	// file did not have stays out while unchanged, and authored keys this saver does not model are kept.
	if (!node->GetAuthoredBaseline().empty()) {
		const json loaded = json::parse(node->GetAuthoredBaseline(), nullptr, false);
		json authored = node->GetAuthoredExtras().empty() ? json::object() : json::parse(node->GetAuthoredExtras(), nullptr, false);
		if (loaded.is_object() && authored.is_object()) {
			nodeJson = MergeAuthoredObject(authored, loaded, nodeJson);
		}
	}
	else if (!node->GetAuthoredExtras().empty()) {
		// Nodes made in the editor keep the keys a file gave them (material_overrides and the like).
		const json extras = json::parse(node->GetAuthoredExtras(), nullptr, false);
		if (extras.is_object()) {
			for (auto it = extras.begin(); it != extras.end(); ++it) {
				if (!nodeJson.contains(it.key())) nodeJson[it.key()] = it.value();
			}
		}
	}
	// Procedural nodes are regenerated from their params; the generated path is transient.
	if (nodeType == SceneNode::MODEL && nodeJson.contains("procedural")) {
		nodeJson.erase("path");
	}

	// Registry-described components (src/reflect). The loader applies them after the legacy keys
	// above, so both must read the same values; they do, since both use the same getters.
	nlohmann::json components = nox::reflect::SerializeComponents(*node);
	if (!components.empty()) {
		nodeJson["components"] = std::move(components);
	}

	return nodeJson;
}

nlohmann::json SceneLoader::SerializeCollider(const std::shared_ptr<SceneNode>& node) {
	nlohmann::json colliderJson;

	auto rb = node->GetRigidBody();
	if (!rb) {
		return colliderJson;
	}

	// Determine collider type
	RigidBody::ShapeType shapeType = rb->getShapeType();

	switch (shapeType) {
	case RigidBody::ShapeType::SPHERE: {
		colliderJson["type"] = "sphere";
		colliderJson["mass"] = rb->getMass();
		colliderJson["radius"] = rb->getBoundingRadius();
		colliderJson["linear_damping"] = rb->getLinearDamping();
		colliderJson["angular_damping"] = rb->getAngularDamping();
		colliderJson["friction"] = rb->getFriction();

		// Check if static body
		bool isStatic = rb->IsStatic();
		colliderJson["static"] = isStatic;

		if (!isStatic) {
			glm::vec3 velocity = rb->getVelocity();
			colliderJson["velocity_x"] = velocity.x;
			colliderJson["velocity_y"] = velocity.y;
			colliderJson["velocity_z"] = velocity.z;

			// Use gravity scale to determine if gravity is enabled
			bool hasGravity = (rb->getGravityScale() > 0.0f);
			colliderJson["gravity"] = hasGravity;
		}
		else {
			colliderJson["gravity"] = false;
		}
		break;
	}

	case RigidBody::ShapeType::BOX: {
		colliderJson["type"] = "box";
		colliderJson["mass"] = rb->getMass();

		glm::vec3 halfExtents = rb->getHalfExtents();
		colliderJson["size"] = { halfExtents.x, halfExtents.y, halfExtents.z };

		colliderJson["linear_damping"] = rb->getLinearDamping();
		colliderJson["angular_damping"] = rb->getAngularDamping();
		colliderJson["friction"] = rb->getFriction();

		// Check if static body
		bool isStatic = rb->IsStatic();
		colliderJson["static"] = isStatic;

		if (!isStatic) {
			glm::vec3 velocity = rb->getVelocity();
			colliderJson["velocity_x"] = velocity.x;
			colliderJson["velocity_y"] = velocity.y;
			colliderJson["velocity_z"] = velocity.z;

			// Use gravity scale to determine if gravity is enabled
			bool hasGravity = (rb->getGravityScale() > 0.0f);
			colliderJson["gravity"] = hasGravity;
		}
		else {
			colliderJson["gravity"] = false;
		}
		break;
	}

	case RigidBody::ShapeType::PLANE: {
		colliderJson["type"] = "plane";

		glm::vec3 normal = rb->getPlaneNormal();
		colliderJson["normal_x"] = normal.x;
		colliderJson["normal_y"] = normal.y;
		colliderJson["normal_z"] = normal.z;

		colliderJson["height"] = rb->getPlaneHeight();
		break;
	}

	case RigidBody::ShapeType::CAPSULE: {
		colliderJson["type"] = "capsule";
		colliderJson["mass"] = rb->getMass();
		colliderJson["radius"] = rb->getBoundingRadius();
		colliderJson["half_segment"] = rb->getHalfSegment();
		colliderJson["axis"] = rb->getCapsuleAxis();
		colliderJson["static"] = rb->IsStatic();
		colliderJson["friction"] = rb->getFriction();
		break;
	}

	case RigidBody::ShapeType::MESH: {
		colliderJson["type"] = "mesh";
		colliderJson["mesh_path"] = rb->getMeshPath();
		colliderJson["friction"] = rb->getFriction();
		break;
	}

	default:
		std::cerr << "[SceneLoader] Unknown collider shape type" << std::endl;
		break;
	}

	return colliderJson;
}

nlohmann::json SceneLoader::SerializeGuiElements(const std::shared_ptr<GuiNode>& guiNode) {
	nlohmann::json elementsArray = nlohmann::json::array();
	if (!guiNode) {
		return elementsArray;
	}
	for (const auto& element : guiNode->GetElements()) {
		elementsArray.push_back(GuiElementToJson(element));
	}
	return elementsArray;
}

nlohmann::json SceneLoader::SerializeLightProperties(const std::shared_ptr<LightNode>& lightNode) {
	nlohmann::json lightJson;

	if (!lightNode || !lightNode->GetLight()) {
		return lightJson;
	}

	auto light = lightNode->GetLight();

	// Determine light type
	BaseLight::LightType lightType = light->GetLightType();
	switch (lightType) {
	case BaseLight::LightType::DIRECTIONAL:
		lightJson["type"] = "directional";
		break;
	case BaseLight::LightType::POINT:
		lightJson["type"] = "point";
		break;
	case BaseLight::LightType::SPOT:
		lightJson["type"] = "spot";
		break;
	default:
		lightJson["type"] = "unknown";
		break;
	}

	// Serialize common light properties
	glm::vec3 color = light->GetColor();
	lightJson["color"] = { color.r, color.g, color.b };

	// The authored photometric unit (LightNode::GetPhotometricUnit) is written back in place of intensity, converted
	// from the current intensity, so an edit made in the editor keeps the file in physical units.
	switch (lightNode->GetPhotometricUnit()) {
	case LightNode::PhotometricUnit::Lumens:
		lightJson["luminous_power"] = LightNode::LuminousPowerFromIntensity(lightType, light->GetIntensity());
		break;
	case LightNode::PhotometricUnit::Lux:
		lightJson["illuminance"] = light->GetIntensity();
		break;
	default:
		lightJson["intensity"] = light->GetIntensity();
		break;
	}
	if (light->GetSourceRadius() > 0.0f) {
		lightJson["source_radius"] = light->GetSourceRadius();
	}
	lightJson["enabled"] = light->IsEnabled();
	lightJson["castsShadows"] = light->CastsShadows();

	// Serialize type-specific properties
	if (lightType == BaseLight::LightType::DIRECTIONAL) {
		auto dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
		if (dirLight) {
			lightJson["shadowSize"] = dirLight->GetShadowSize();
			lightJson["splitLambda"] = dirLight->GetSplitLambda();
		}
	}
	else if (lightType == BaseLight::LightType::POINT || lightType == BaseLight::LightType::SPOT) {
		lightJson["range"] = light->GetRange();

		// A lumen-authored light without an authored "attenuation" implies LightNode::ImplicitLumenAttenuation; the
		// key is written only when the scene authored it or the value was changed since.
		glm::vec3 attenuation = light->GetAttenuation();
		const glm::vec3 implicitFalloff = LightNode::ImplicitLumenAttenuation();
		const bool implicit = lightNode->GetPhotometricUnit() == LightNode::PhotometricUnit::Lumens &&
			!lightNode->IsAttenuationAuthored() && glm::all(glm::equal(attenuation, implicitFalloff));
		if (!implicit) {
			lightJson["attenuation"] = { attenuation.x, attenuation.y, attenuation.z };
		}

		if (lightType == BaseLight::LightType::SPOT) {
			auto spotLight = std::dynamic_pointer_cast<SpotLight>(light);
			if (spotLight) {
				lightJson["cutOff"] = spotLight->GetCutOff();
				lightJson["outerCutOff"] = spotLight->GetOuterCutOff();
				lightJson["shadowResolution"] = spotLight->GetShadowResolution();
			}
		}
		else {
			auto pointLight = std::dynamic_pointer_cast<PointLight>(light);
			lightJson["shadowResolution"] = pointLight ? pointLight->GetShadowResolution() : 1024;
		}
	}

	return lightJson;
}

void SceneLoader::ProcessCollider(const nlohmann::json& colliderJson, std::shared_ptr<SceneNode> node) {
	if (!colliderJson.is_object() || !node) return;

	std::string type = colliderJson.value("type", "sphere");
	if (type == "sphere") {
		float mass = colliderJson.value("mass", 1.0f);
		float radius = colliderJson.value("radius", 1.0f);
		bool useGravity = colliderJson.value("gravity", true);
		float linearDamping = colliderJson.value("linear_damping", 0.1f);
		float angularDamping = colliderJson.value("angular_damping", 0.1f);
		float friction = colliderJson.value("friction", m_physicsEngine->GetConfig().defaultFriction);
		bool isStatic = colliderJson.value("static", false);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::SPHERE);
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setLinearDamping(linearDamping);
		rb->setAngularDamping(angularDamping);
		rb->setFriction(friction);
		rb->setRestitution(m_physicsEngine->GetConfig().defaultRestitution);
		rb->setBoundingRadius(radius);

		// Determine if this should be static
		// Static if explicitly marked OR if gravity is disabled (treat as immovable obstacle)
		if (isStatic || !useGravity) {
			rb->setMass(0.0f);  // Zero mass = infinite mass = immovable
			rb->SetBodyType(RigidBody::BodyType::STATIC);
			rb->setGravityScale(0.0f);
			rb->setVelocity(glm::vec3(0.0f));
			rb->setAcceleration(glm::vec3(0.0f));
		}
		else {
			rb->setMass(mass);
			rb->setGravityScale(1.0f);

			glm::vec3 initVel{
				colliderJson.value("velocity_x", 0.0f),
				colliderJson.value("velocity_y", 0.0f),
				colliderJson.value("velocity_z", 0.0f)
			};
			rb->setVelocity(initVel);
			rb->setAcceleration(glm::vec3(0.0f));
		}

		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "plane") {
		// Plane is static, infinite, and only needs a normal + height
		glm::vec3 normal = {
			colliderJson.value("normal_x", 0.0f),
			colliderJson.value("normal_y", 1.0f),
			colliderJson.value("normal_z", 0.0f)
		};
		float height = colliderJson.value("height", 0.0f);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::PLANE);
		rb->AttachNode(node);

		// Treat plane as static: zero velocity, zero mass
		rb->setMass(0.0f);
		rb->setVelocity(glm::vec3(0.0f));
		rb->setAcceleration(glm::vec3(0.0f));
		rb->setPosition(node->GetPosition());
		rb->setPlane(normal, height);

		// Planes are always static - no gravity
		rb->setGravityScale(0.0f);
		rb->SetBodyType(RigidBody::BodyType::STATIC);

		rb->setBoundingRadius(0.0f); // not used for collision
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "box") {
		glm::vec3 size = glm::vec3(1.0f);
		if (colliderJson.contains("size") && colliderJson["size"].is_array() && colliderJson["size"].size() == 3) {
			size = glm::vec3(colliderJson["size"][0], colliderJson["size"][1], colliderJson["size"][2]);
		}

		float mass = colliderJson.value("mass", 1.0f);
		bool useGravity = colliderJson.value("gravity", true);
		float linearDamping = colliderJson.value("linear_damping", 0.0f);
		float angularDamping = colliderJson.value("angular_damping", 0.0f);
		float friction = colliderJson.value("friction", m_physicsEngine->GetConfig().defaultFriction);
		bool isStatic = colliderJson.value("static", false);

		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::BOX);
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setLinearDamping(linearDamping);
		rb->setAngularDamping(angularDamping);
		rb->setFriction(friction);
		rb->setRestitution(m_physicsEngine->GetConfig().defaultRestitution);
		rb->setBox(size);

		// Determine if this should be static
		// Static if explicitly marked OR if gravity is disabled (treat as immovable obstacle)
		if (isStatic || !useGravity) {
			rb->setMass(0.0f);  // Zero mass = infinite mass = immovable
			rb->SetBodyType(RigidBody::BodyType::STATIC);
			rb->setGravityScale(0.0f);
			rb->setVelocity(glm::vec3(0.0f));
			rb->setAcceleration(glm::vec3(0.0f));
		}
		else {
			rb->setMass(mass);
			rb->setGravityScale(1.0f);

			glm::vec3 initVel{
				colliderJson.value("velocity_x", 0.0f),
				colliderJson.value("velocity_y", 0.0f),
				colliderJson.value("velocity_z", 0.0f)
			};
			rb->setVelocity(initVel);
			rb->setAcceleration(glm::vec3(0.0f));
		}

		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "capsule") {
		auto rb = std::make_shared<RigidBody>();
		rb->setShape(RigidBody::ShapeType::CAPSULE);
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setFriction(colliderJson.value("friction", m_physicsEngine->GetConfig().defaultFriction));
		rb->setRestitution(m_physicsEngine->GetConfig().defaultRestitution);
		rb->setCapsule(colliderJson.value("radius", 0.5f), colliderJson.value("half_segment", 0.5f),
			colliderJson.value("axis", 1));
		if (colliderJson.value("static", false)) {
			rb->setMass(0.0f);
			rb->SetBodyType(RigidBody::BodyType::STATIC);
			rb->setGravityScale(0.0f);
		}
		else {
			rb->setMass(colliderJson.value("mass", 1.0f));
			rb->setGravityScale(1.0f);
		}
		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
	else if (type == "mesh") {
		// Static geometry from a glTF/GLB file; the node's transform places it. Missing files leave an empty mesh.
		const std::string path = colliderJson.value("mesh_path", std::string());
		auto mesh = MeshCollider::LoadShared(path);
		glm::vec3 mn(0.0f), mx(0.0f);
		if (mesh) MeshCollider::LocalBounds(*mesh, mn, mx);
		auto rb = std::make_shared<RigidBody>();
		rb->AttachNode(node);
		rb->setPosition(node->GetPosition());
		rb->setOrientation(node->GetOrientation());
		rb->setFriction(colliderJson.value("friction", m_physicsEngine->GetConfig().defaultFriction));
		rb->setRestitution(m_physicsEngine->GetConfig().defaultRestitution);
		rb->setMesh(mesh, mn, mx, path);
		rb->computeInertiaTensor();
		rb->computeAABB();
		node->AttachRigidBody(rb);
		m_physicsEngine->AddBody(rb);
	}
}



// Applies the layout keys of a parsed GUI element (resizable, anchor, relative position/size).
namespace {
void ApplyGuiElementLayout(GuiNode& guiNode, int elementId, const GuiNode::GuiElement& el) {
	if (elementId < 0) return;
	if (el.isResizable) {
		guiNode.SetElementResizable(elementId, true, el.minWidth, el.minHeight, el.maxWidth, el.maxHeight);
	}
	if (el.anchor != GuiNode::GuiElement::Anchor::TOP_LEFT) {
		guiNode.SetElementAnchor(elementId, el.anchor);
	}
	if (el.useRelativePositioning) {
		guiNode.SetElementRelativePosition(elementId, el.relativePosition.x, el.relativePosition.y);
	}
	if (el.useRelativeSize) {
		guiNode.SetElementRelativeSize(elementId, el.relativeSize.x, el.relativeSize.y);
	}
}
} // namespace

void SceneLoader::StoreAuthoredState(const std::shared_ptr<SceneNode>& node, const json& nodeJson) {
	// The loaded state is serialized with no authored keys in place (SerializeNode merges them only when set).
	node->SetAuthoredExtras(std::string());
	node->SetAuthoredBaseline(std::string());
	json loaded = SerializeNode(node);
	loaded.erase("components");
	// Children are separate nodes and the prefab link is kept by SetPrefabPath, so neither is part of the own keys.
	json authored = nodeJson;
	authored.erase("children");
	authored.erase("components");
	authored.erase("prefab");
	// A folder has no transform of its own: a transform the file wrote for it is dropped, not kept as authored.
	if (node->IsFolder()) {
		authored.erase("position");
		authored.erase("rotation");
		authored.erase("scale");
	}
	node->SetAuthoredExtras(authored.dump());
	node->SetAuthoredBaseline(loaded.dump());
}

std::shared_ptr<SceneNode> SceneLoader::ProcessNodeRecursive(const json& nodeJson, EntityID parentEntityID) {
	m_pendingComponentNames.clear();
	auto node = ProcessNode(nodeJson);
	std::vector<std::string> deferredComponents = std::move(m_pendingComponentNames);
	m_pendingComponentNames.clear();
	json generatedChildren = std::move(m_pendingGeneratedChildren);
	m_pendingGeneratedChildren = json();
	if (!node) return nullptr;

	// Create ECS entity for this node
	CreateECSEntity(node, parentEntityID);

	// Components that needed the entity (Animator) are applied now, with the same warnings as the first pass.
	if (!deferredComponents.empty()) {
		std::vector<std::string> warnings;
		nox::reflect::DeserializeComponentsNamed(*node, nodeJson["components"], deferredComponents, &warnings);
		for (const std::string& warning : warnings) {
			std::cerr << "[SceneLoader] Warning: node '" << node->GetName() << "': " << warning << std::endl;
		}
	}
	// Authored keys and the loaded state, after the entity exists (save compares against them).
	StoreAuthoredState(node, nodeJson);

	EntityID myEntityID = node->GetEntityID();
	const size_t importedChildCount = node->children.size();
	for (size_t i = 0; i < importedChildCount; ++i) {
		CreateExistingChildEntitiesRecursive(node->children[i], myEntityID);
	}

	// If the node has children, process them recursively with this node as parent
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		for (auto& childJson : nodeJson["children"]) {
			SliceTask::YieldIfOverBudget();  // slice point: one child at a time (generated children can number in the hundreds)
			auto childNode = ProcessNodeRecursive(childJson, myEntityID);
			if (childNode) {
				node->AddChild(childNode);
			}
		}
	}
	if (generatedChildren.is_array()) {
		for (const json& childJson : generatedChildren) {
			SliceTask::YieldIfOverBudget();  // slice point: one child at a time (generated children can number in the hundreds)
			auto childNode = ProcessNodeRecursive(childJson, myEntityID);
			if (childNode) {
				childNode->SetGenerated(true);
				node->AddChild(childNode);
			}
		}
	}
	return node;
}

std::shared_ptr<SceneNode> SceneLoader::ProcessNode(const json& nodeJson) {
	ComponentManager* componentManager = m_currentSceneGraph ? m_currentSceneGraph->GetComponentManager() : nullptr;
	TransformSystem* transformSystem = m_currentSceneGraph ? m_currentSceneGraph->GetTransformSystem() : nullptr;

	// Determine node type.
	std::string typeStr = nodeJson.value("type", "model");
	NodeType type = GetNodeType(typeStr);

	// Create the appropriate node type.
	std::shared_ptr<SceneNode> node;
	// set node its type
	if (node == nullptr)
		switch (type) {
		case NodeType::AUDIO:
			node = std::make_shared<AudioNode>();
			node->SetECSContext(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		case NodeType::LIGHT:
			node = std::make_shared<LightNode>(nullptr); // Will be set based on light properties
			node->SetECSContext(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		case NodeType::CAMERA:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::CAMERA);
			break;
		case NodeType::FOLDER:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::FOLDER);
			break;
		case NodeType::LPV_VOLUME:  // Handle LPV volume nodes
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::LPV_VOLUME);
			break;
		case NodeType::SKELETAL:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(SceneNode::SKELETAL);
			break;
		case NodeType::MODEL:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			node->SetNodeType(static_cast<SceneNode::NODE_TYPE>(type));
			break;
		default:
			node = std::make_shared<SceneNode>(componentManager, transformSystem);
			break;
		}

	// Process common transform data. A folder is always at identity whatever the file says.
	const std::string nodeDebugName = nodeJson.value("name", typeStr);
	glm::vec3 pos = ReadSceneVec3(nodeJson, "position", glm::vec3(0.0f), nodeDebugName);
	glm::vec3 rot = ReadSceneVec3(nodeJson, "rotation", glm::vec3(0.0f), nodeDebugName);
	glm::vec3 scl = SanitizeSceneScale(
		ReadSceneVec3(nodeJson, "scale", glm::vec3(1.0f), nodeDebugName),
		nodeDebugName);
	if (type == NodeType::FOLDER) {
		pos = glm::vec3(0.0f);
		rot = glm::vec3(0.0f);
		scl = glm::vec3(1.0f);
	}

	// Apply rotation correctly using combined Euler angles -> quaternion conversion
	// Previous code called SetRotation three times, but each call replaced the rotation instead of accumulating
	glm::vec3 radians = glm::radians(rot);
	glm::quat rotationQuat = glm::quat(radians); // glm::quat from Euler angles (pitch, yaw, roll)

	// Apply transform using SetLocalTRS which correctly combines translation, rotation, scale
	node->SetLocalTRS(pos, rotationQuat, scl);
	// Keep the authored angles and scale (SetLocalTRS clamps the scale the same way).
	node->SetAuthoredRotationScale(radians, glm::clamp(scl, glm::vec3(1e-6f), glm::vec3(1000.0f)));

	// Set node name if provided
	const std::string nodeName = nodeJson.value("name", "");
	if (!nodeName.empty()) {
		node->SetName(nodeName);
	}

	// Process type-specific properties.
	switch (type) {
	case NodeType::MODEL: { // ModelNode
		std::string modelPath = nodeJson.value("path", "");
		bool proceduralSkipped = false;
		if (nodeJson.contains("procedural") && nodeJson["procedural"].is_object()) {
			const json& procedural = nodeJson["procedural"];
			const std::string generator = procedural.value("generator", SceneExtensions::DefaultGenerator());
			const json generatorParams = procedural.value("params", json::object());
			ProceduralGeneratorResult generated;
			std::string message;
			const ProceduralSource source = ResolveProceduralNode(generator, generatorParams, m_sceneGameJson, generated, message);
			if (source == ProceduralSource::MissingCooked) {
				// Not a load failure: the node is left without geometry and the scene loads (dev loose runs
				// without nox_cook output). Packaging cooks every procedural node it ships.
				proceduralSkipped = true;
				std::cerr << "[SceneLoader] ERROR: procedural generator '" << generator << "' is not compiled into this executable and "
					<< "node '" << nodeDebugName << "' has no cooked result, node skipped: " << message << std::endl;
			}
			else if (source == ProceduralSource::GeneratorFailed) {
				std::cerr << "[SceneLoader] ERROR: procedural generator failed for node '" << nodeDebugName << "'" << std::endl;
			}
			if (generated.ok) {
				modelPath = generated.gltfPath;
				m_pendingGeneratedChildren = std::move(generated.childNodes);
				AddReflectionProbes(generated.reflectionProbes);
				if (m_currentSceneGraph && generated.hasRenderExtras) {
					// Unique per load so the passes re-upload their instance buffers.
					m_currentSceneGraph->renderExtras = std::move(generated.renderExtras);
					m_currentSceneGraph->renderExtras.revision = NextRenderExtrasRevision();
				}
			}
		}
		bool modelResolved = false;
		modelPath = ResolveReadableAssetPath(
			modelPath,
			m_currentSceneDirectory,
			"model",
			true,
			&modelResolved);
		if (modelResolved) {
			if (!m_modelManager) {
				++m_missingCriticalAssetCount;
				std::cerr << "[SceneLoader] ERROR: ModelManager unavailable while loading node '"
					<< nodeDebugName << "': " << modelPath << std::endl;
				break;
			}
			auto model = m_modelManager->LoadModel(modelPath); // Load the model using the ModelManager
			if (model) {
				++m_loadedModelCount;
				if (nodeJson.value("exclude_from_reflection_capture", false)) {
					model->excludeFromReflectionCapture = true;
				}
				if (nodeJson.contains("material_overrides")) {
					ApplyMaterialOverrides(*model, nodeJson["material_overrides"], nodeDebugName);
				}
				node->SetModel(model); // Set the model to the node
				if (model->hasSkin) {
					node->BuildSkeleton(*model);
				}
			}
			else {
				++m_missingCriticalAssetCount;
				std::cerr << "[SceneLoader] ERROR: Model import failed for node '"
					<< nodeDebugName << "': " << modelPath << std::endl;
			}
		}
		else if (!proceduralSkipped) {
			++m_missingCriticalAssetCount;
		}
		// Check for custom shader properties
		if (nodeJson.contains("vertex_shader") && nodeJson.contains("fragment_shader")) {
			std::string vsPath = nodeJson.value("vertex_shader", "");
			std::string fsPath = nodeJson.value("fragment_shader", "");
			if (!vsPath.empty() && !fsPath.empty()) {
				rhi::ProgramHandle customShader = CreateShaderProgram(vsPath.c_str(), fsPath.c_str());
				if (customShader.IsValid())
					node->SetShader(customShader);
				else
					std::cerr << "[ERROR] Custom shader compilation failed for node ("
					<< node->GetName() << "). Falling back to default shader." << std::endl;
			}
		}
		if (auto model = node->GetModel(); model && !model->hasSkin) {
			BuildImportedMeshNodeChildren(node, model);
		}
		break;
	}
	case NodeType::AUDIO: { // AudioNode
		AudioNode* audioNode = dynamic_cast<AudioNode*>(node.get()); // Ensure the node is of type AudioNode
		if (audioNode) {
			std::string soundFile = nodeJson.value("sound", "");
			float pitch = nodeJson.value("pitch", 1.0f);
			float volume = nodeJson.value("volume", 1.0f);
			float hearingDistance = nodeJson.value("hearing_distance", 10.0f);
			bool loop = nodeJson.value("loop", false);
			bool is3d = nodeJson.value("is3d", true);
			bool autoplay = nodeJson.value("autoplay", true);
			audioNode->setPitch(pitch);
			audioNode->setVolume(volume);
			audioNode->setHearingDistance(hearingDistance);
			audioNode->setLooping(loop);
			audioNode->setIs3D(is3d);
			audioNode->setAutoplay(autoplay);
			AudioAttenuation curve = AudioAttenuation::Linear;
			const std::string curveName = nodeJson.value("attenuation", std::string("linear"));
			if (!ParseAudioAttenuation(curveName, curve)) {
				std::cerr << "[SceneLoader] unknown attenuation '" << curveName << "', using linear\n";
			}
			audioNode->setAttenuation(curve);
			audioNode->setMinDistance(nodeJson.value("min_distance", 0.0f));
			audioNode->setRolloff(nodeJson.value("rolloff", 1.0f));
			audioNode->setOcclusion(nodeJson.value("occlusion", false));
			if (!soundFile.empty()) {
				if (!audioNode->initAudio(soundFile))
					std::cerr << "[ERROR] Failed to load sound file: " << soundFile << std::endl;
			}
			if (autoplay && !m_scratchInstance) {
				audioNode->play(loop, is3d);
			}
		}
		break;
	}
	case NodeType::LIGHT: {
		LightNode* lightNode = dynamic_cast<LightNode*>(node.get());
		if (lightNode && nodeJson.contains("light")) {
			const auto& lightJson = nodeJson["light"];

			// Create appropriate light type
			std::string lightType = lightJson.value("type", "point");
			std::shared_ptr<BaseLight> light;

			if (lightType == "directional") {
				auto dirLight = std::make_shared<DirectionalLight>();
				if (lightJson.contains("shadowSize")) {
					dirLight->InitializeCascades(lightJson["shadowSize"], lightJson.value("splitLambda", 0.8f));
				}
				light = dirLight;
			}
			else if (lightType == "point") {
				auto pointLight = std::make_shared<PointLight>();
				if (lightJson.contains("shadowResolution")) {
					pointLight->InitializeShadowMap(lightJson["shadowResolution"]);
				}
				light = pointLight;
			}
			else if (lightType == "spot") {
				auto spotLight = std::make_shared<SpotLight>();
				if (lightJson.contains("shadowResolution")) {
					spotLight->InitializeShadowMap(lightJson["shadowResolution"]);
				}
				if (lightJson.contains("cutOff")) {
					spotLight->SetCutOff(lightJson["cutOff"]);
				}
				if (lightJson.contains("outerCutOff")) {
					spotLight->SetOuterCutOff(lightJson["outerCutOff"]);
				}
				light = spotLight;
			}

			if (light) {
				// Set common light properties
				if (lightJson.contains("color") && lightJson["color"].is_array() && lightJson["color"].size() == 3) {
					light->SetColor(glm::vec3(lightJson["color"][0], lightJson["color"][1], lightJson["color"][2]));
				}
				if (lightJson.contains("intensity")) {
					light->SetIntensity(lightJson["intensity"]);
				}
				if (lightJson.contains("enabled")) {
					light->SetEnabled(lightJson["enabled"]);
				}
				else {
					// Enable lights by default when loaded from scene files
					light->SetEnabled(true);
				}
				if (lightJson.contains("castsShadows")) {
					light->SetCastsShadows(lightJson["castsShadows"]);
				}
				if (lightJson.contains("range")) {
					light->SetRange(lightJson["range"]);
				}
				const bool attenuationAuthored = lightJson.contains("attenuation") && lightJson["attenuation"].is_array() &&
					lightJson["attenuation"].size() == 3;
				if (attenuationAuthored) {
					light->SetAttenuation(lightJson["attenuation"][0], lightJson["attenuation"][1], lightJson["attenuation"][2]);
				}
				// Photometric units (Frostbite PBR 3.0, docs/nfs2015/configuration.md): punctual lights
				// specify luminous power in lumens and are stored as luminous intensity in candela
				// (LightNode::IntensityFromLuminousPower), with inverse-square falloff (min distance 1 cm) and the
				// existing smooth range window unless the scene also authored "attenuation".
				// Directional lights specify illuminance in lux. Rendering is pre-exposed, see
				// RenderContext::physicalCamera. The unit is kept on the node so save writes the same key back.
				lightNode->SetAttenuationAuthored(attenuationAuthored);
				if (lightJson.contains("luminous_power") && lightJson["luminous_power"].is_number()) {
					const float lumens = std::max(lightJson["luminous_power"].get<float>(), 0.0f);
					light->SetIntensity(LightNode::IntensityFromLuminousPower(light->GetLightType(), lumens));
					if (!attenuationAuthored) {
						const glm::vec3 falloff = LightNode::ImplicitLumenAttenuation();
						light->SetAttenuation(falloff.x, falloff.y, falloff.z);
					}
					lightNode->SetPhotometricUnit(LightNode::PhotometricUnit::Lumens);
				}
				else if (lightJson.contains("illuminance") && lightJson["illuminance"].is_number()) {
					light->SetIntensity(std::max(lightJson["illuminance"].get<float>(), 0.0f));
					lightNode->SetPhotometricUnit(LightNode::PhotometricUnit::Lux);
				}
				if (lightJson.contains("source_radius") && lightJson["source_radius"].is_number()) {
					light->SetSourceRadius(lightJson["source_radius"].get<float>());
				}

				// Set the light and force transform update
				lightNode->SetLight(light);

				// Update light properties from node transform
				lightNode->UpdateLightFromTransform();

				// Enable debug visualization if specified
				if (lightJson.contains("showDebugVisualization")) {
					lightNode->SetDebugVisualization(lightJson["showDebugVisualization"]);
				}

				// Store node name for light registration (don't call SetName if it doesn't exist)
				std::string lightName = nodeJson.value("name", "Light_" + lightType);
				// node->SetName(lightName); // Comment out if SetName doesn't exist

				std::cout << "[SceneLoader] Successfully loaded " << lightType << " light node '"
					<< lightName << "' at (" << pos.x << "," << pos.y << "," << pos.z
					<< ") with intensity " << light->GetIntensity()
					<< ", enabled: " << light->IsEnabled() << std::endl;
			}
			else {
				std::cerr << "[SceneLoader] Failed to create light of type: " << lightType << std::endl;
			}
		}
		else {
			std::cerr << "[SceneLoader] Light node missing light properties" << std::endl;
		}
		break;
	}
	case NodeType::GUI: {
		auto guiNode = std::make_shared<GuiNode>(m_screenW, m_screenH); // Create a GUI node,this acts as a container for GUI elements in the scene
		guiNode->SetECSContext(componentManager, transformSystem);

		// Load font if specified
		std::string fontPath = nodeJson.value("font_path", "fonts\\arial.ttf");
		int fontSize = nodeJson.value("font_size", 24);
		guiNode->LoadFont(fontPath, fontSize);

		if (nodeJson.contains("elements") && nodeJson["elements"].is_array()) {
			for (const auto& elementJson : nodeJson["elements"]) {
				GuiNode::GuiElement el;
				std::string kind;
				if (!GuiElementFromJson(elementJson, el, &kind)) {
					std::cerr << "[SceneLoader] Unknown GUI element kind: " << kind << std::endl;
					continue;
				}
				const int countBefore = guiNode->GetElementCount();
				int elementId = -1;
				switch (el.type) {
				case GuiNode::GuiType::TEXT:
					guiNode->AddText(el.content, el.x, el.y, el.color);
					elementId = guiNode->GetElementCount() - 1;
					break;
				case GuiNode::GuiType::IMAGE:
					guiNode->AddImage(el.content, el.x, el.y, el.width, el.height);
					elementId = guiNode->GetElementCount() > countBefore ? guiNode->GetElementCount() - 1 : -1;
					break;
				case GuiNode::GuiType::RECT_SOLID:
					elementId = guiNode->AddSolidRect(el.x, el.y, el.width, el.height, el.color);
					break;
				case GuiNode::GuiType::RECT_GRADIENT:
					elementId = guiNode->AddGradientRect(el.x, el.y, el.width, el.height, el.gradient);
					break;
				case GuiNode::GuiType::RECT_BEVEL:
					elementId = guiNode->AddBevelRect(el.x, el.y, el.width, el.height, el.color, el.bevel);
					break;
				case GuiNode::GuiType::PROGRESS_BAR:
					elementId = guiNode->AddProgressBar(el.x, el.y, el.width, el.height, el.progressStyle);
					if (elementId >= 0) {
						guiNode->SetProgressValue(elementId, el.progressValue);
						if (!el.progressLabel.empty()) {
							guiNode->SetProgressLabel(elementId, el.progressLabel);
						}
					}
					break;
				}
				ApplyGuiElementLayout(*guiNode, elementId, el);
			}
		}
		node = guiNode;
		node->SetNodeType(SceneNode::GUI); // Set the node type to GUI
		node->SetLocalTRS(pos, rotationQuat, scl);
		if (!nodeName.empty()) {
			node->SetName(nodeName);
		}
		break;
	}

	case NodeType::NODE:        // plain transform node (written as "node"): created by the first switch
	case NodeType::CAMERA:      // camera node: created by the first switch, camera data is read elsewhere
	case NodeType::LPV_VOLUME:  // LPV bounds and grid are read below from "lpv_data"
	case NodeType::SKELETAL:    // skeletal node: created by the first switch
	case NodeType::FOLDER:   // no type-specific data: the transform was reset above
		break;

	default:
		std::cerr << "Unknown node type: " << typeStr << std::endl;
		break;
	}

	// LPV volume bounds and grid (written by SerializeNode as "lpv_data").
	if (type == NodeType::LPV_VOLUME && nodeJson.contains("lpv_data") && nodeJson["lpv_data"].is_object()) {
		const json& lpvJson = nodeJson["lpv_data"];
		SceneNode::LPVVolumeData data = node->GetLPVVolumeData();
		data.center = ReadSceneVec3(lpvJson, "center", data.center, nodeDebugName);
		data.extent = glm::max(ReadSceneVec3(lpvJson, "extent", data.extent, nodeDebugName), glm::vec3(0.01f));
		const float voxelSize = lpvJson.value("voxel_size", data.voxelSize);
		if (std::isfinite(voxelSize) && voxelSize > 0.0f) {
			data.voxelSize = voxelSize;
		}
		data.gridResolution = std::max(lpvJson.value("grid_resolution", data.gridResolution), 1);
		if (lpvJson.contains("orientation") && lpvJson["orientation"].is_array() && lpvJson["orientation"].size() == 4) {
			const glm::quat q(lpvJson["orientation"][0].get<float>(), lpvJson["orientation"][1].get<float>(),
				lpvJson["orientation"][2].get<float>(), lpvJson["orientation"][3].get<float>());
			if (std::isfinite(glm::length(q)) && glm::length(q) > 0.0f) {
				data.orientation = glm::normalize(q);
			}
		}
		node->SetLPVVolumeData(data);
	}

	// Process collider for any node type (not just MODEL nodes)
	// This allows nodes without models to still have physics colliders
	if (nodeJson.contains("collider")) {
		ProcessCollider(nodeJson["collider"], node);
	}

	// Prefab instance root: the expanded reference keeps the source path (see src/scene/Prefab.h).
	if (nodeJson.contains("prefab") && nodeJson["prefab"].is_string()) {
		node->SetPrefabPath(nodeJson["prefab"].get<std::string>());
	}

	// Registry components win over the legacy keys above (see SerializeNode).
	if (nodeJson.contains("components")) {
		std::vector<std::string> warnings;
		nox::reflect::DeserializeComponents(*node, nodeJson["components"], &warnings, &m_pendingComponentNames);
		for (const std::string& warning : warnings) {
			std::cerr << "[SceneLoader] Warning: node '" << nodeDebugName << "': " << warning << std::endl;
		}
	}

	return node;
}

// ============== ECS ENTITY AND COMPONENT CREATION ==============

void SceneLoader::CreateECSEntity(std::shared_ptr<SceneNode> node, EntityID parentID) {
	if (!node || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	TransformSystem* transformSystem = m_currentSceneGraph->GetTransformSystem();
	if (!componentManager) return;

	const glm::mat4 authoredLocalTransform = node->GetTransform();
	const glm::mat4 authoredAnimatedTransform = node->GetAnimatedTransform();

	// Skip if entity already exists
	if (node->GetEntityID() != INVALID_ENTITY) {
		// Keep ECS adjacency authoritative whenever an existing node is reattached.
		componentManager->SetParent(node->GetEntityID(), parentID);
		if (TransformComponent* transform = node->GetTransformComponent()) {
			transform->isDirty = true;
		}
		return;
	}

	// Determine ECS node type
	::NodeType ecsType = ::NodeType::NODE;
	switch (node->GetNodeType()) {
	case SceneNode::NODE:       ecsType = ::NodeType::NODE; break;
	case SceneNode::MODEL:      ecsType = ::NodeType::MODEL; break;
	case SceneNode::LIGHT:      ecsType = ::NodeType::LIGHT; break;
	case SceneNode::CAMERA:     ecsType = ::NodeType::CAMERA; break;
	case SceneNode::AUDIO:      ecsType = ::NodeType::AUDIO; break;
	case SceneNode::GUI:        ecsType = ::NodeType::GUI; break;
	case SceneNode::LPV_VOLUME: ecsType = ::NodeType::LPV_VOLUME; break;
	case SceneNode::SKELETAL:   ecsType = ::NodeType::SKELETAL; break;
	case SceneNode::FOLDER:     ecsType = ::NodeType::FOLDER; break;
	}

	// Create entity
	std::string nodeName = node->GetName();
	EntityID entityID = componentManager->CreateEntity(nodeName, ecsType);
	node->SetEntityID(entityID);

	// CreateEntity already allocates a transform component. Populate it in place so the
	// loader authors the final local transform instead of layering a second AddTransform call.
	TransformComponent* transformComp = componentManager->GetTransform(entityID);
	if (!transformComp) {
		return;
	}
	transformComp->localTransform = authoredLocalTransform;
	transformComp->animatedTransform = authoredAnimatedTransform;
	transformComp->worldTransform = glm::mat4(1.0f);
	transformComp->prevWorldTransform = glm::mat4(1.0f);
	transformComp->hasAnimation = (authoredAnimatedTransform != glm::mat4(1.0f));
	transformComp->isDirty = true;
	transformComp->parentID = INVALID_ENTITY;

	componentManager->SetParent(entityID, parentID);
	componentManager->QueueTransformUpdate(entityID);
	if (transformSystem) {
		transformSystem->SetLocalTransform(entityID, authoredLocalTransform);
		if (authoredAnimatedTransform != glm::mat4(1.0f)) {
			transformSystem->SetAnimatedTransform(entityID, authoredAnimatedTransform);
		}
		else {
			transformSystem->ClearAnimatedTransform(entityID);
		}
	}
	// Create RenderableComponent if node has a model
	if (node->GetModel() && (node->renderWholeModel || !node->renderMeshIndices.empty())) {
		CreateRenderableComponent(node, node->GetModel());
	}

	if (node->GetNodeType() == SceneNode::LIGHT) {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			LightComponent lightComp;
			lightComp.light = lightNode->GetLight();
			lightComp.enabled = lightNode->GetLight()->IsEnabled();
			lightComp.castsShadows = lightNode->GetLight()->CastsShadows();
			componentManager->AddLight(entityID, lightComp);
		}
	}
	else if (node->GetNodeType() == SceneNode::CAMERA) {
		CameraComponent cameraComp;
		if (auto cameraNode = std::dynamic_pointer_cast<Camera>(node)) {
			cameraComp.up = cameraNode->GetCameraUpVector();
			cameraComp.front = cameraNode->GetCameraFrontVector();
			cameraComp.right = cameraNode->GetCameraRightVector();
			cameraComp.yaw = cameraNode->GetCameraFacingAngle();
			cameraComp.pitch = 0.0f;
			cameraComp.fov = cameraNode->GetCameraFov();
			cameraComp.aspectRatio = (m_screenH != 0) ? static_cast<float>(m_screenW) / static_cast<float>(m_screenH) : cameraComp.aspectRatio;
			cameraComp.nearPlane = cameraNode->GetCameraNearPlane();
			cameraComp.farPlane = cameraNode->GetCameraFarPlane();
			cameraComp.movementSpeed = cameraNode->GetCameraMovementSpeed();
			cameraComp.mouseSensitivity = cameraNode->GetCameraMouseSensitivity();
		}
		else {
			const glm::mat4 cameraTransform = node->GetTransform();
			cameraComp.right = glm::normalize(glm::vec3(cameraTransform[0]));
			cameraComp.up = glm::normalize(glm::vec3(cameraTransform[1]));
			cameraComp.front = glm::normalize(-glm::vec3(cameraTransform[2]));
			cameraComp.aspectRatio = (m_screenH != 0) ? static_cast<float>(m_screenW) / static_cast<float>(m_screenH) : cameraComp.aspectRatio;
		}
		componentManager->AddCamera(entityID, cameraComp);
	}

	// Create AnimationComponent if model has animations
	if (node->GetModel() && !node->GetModel()->animations.empty()) {
		CreateAnimationComponent(node, node->GetModel());
	}

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created ECS entity " << entityID << " for node: " << nodeName
			<< " (parent=" << parentID << ")" << std::endl;
	}
}

void SceneLoader::CreateRenderableComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model) {
	if (!node || !model || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	EntityID entityID = node->GetEntityID();
	if (!componentManager || entityID == INVALID_ENTITY) return;

	RenderableComponent renderComp;
	renderComp.model = model;
	renderComp.shaderID = node->GetShader();
	renderComp.boundingRadius = node->boundingRadius;
	renderComp.isSkinned = node->isSkinned;
	renderComp.hasAlpha = false;
	renderComp.renderWholeModel = node->renderWholeModel;
	renderComp.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(node->GetCullingOverride()));
	renderComp.meshIndices = node->renderMeshIndices;
	renderComp.boneNodes = node->boneNodes;
	renderComp.boneInverseBindMatrices = node->boneInverseBindMatrices;
	renderComp.nodeIndex = node->nodeIndex;

	auto markAlpha = [&](const MeshComponent& mesh) {
		if (mesh.RequiresAlphaBlending()) {
			renderComp.hasAlpha = true;
		}
	};

	if (renderComp.renderWholeModel) {
		for (const auto& mesh : model->meshes) {
			markAlpha(mesh);
		}
	}
	else {
		for (uint32_t meshIndex : renderComp.meshIndices) {
			if (meshIndex < model->meshes.size()) {
				markAlpha(model->meshes[meshIndex]);
			}
		}
	}

	componentManager->AddRenderable(entityID, renderComp);

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created RenderableComponent for entity " << entityID
			<< " with " << model->meshes.size() << " meshes" << std::endl;
	}
}

void SceneLoader::CreateExistingChildEntitiesRecursive(const std::shared_ptr<SceneNode>& node, EntityID parentID)
{
	if (!node) {
		return;
	}

	CreateECSEntity(node, parentID);
	const EntityID entityID = node->GetEntityID();
	const size_t childCount = node->children.size();
	for (size_t i = 0; i < childCount; ++i) {
		CreateExistingChildEntitiesRecursive(node->children[i], entityID);
	}
}

void SceneLoader::BuildImportedMeshNodeChildren(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& model)
{
	if (!node || !model || model->nodes.empty()) {
		return;
	}

	const std::vector<glm::mat4>& cachedSourceWorldTransforms = model->GetNodeWorldTransforms();
	const bool hasCachedSourceWorldTransforms = (cachedSourceWorldTransforms.size() == model->nodes.size());
	std::vector<glm::mat4> fallbackSourceWorldTransforms;
	if (!hasCachedSourceWorldTransforms) {
		fallbackSourceWorldTransforms = ComputeModelNodeWorldTransforms(*model);
	}
	const std::vector<glm::mat4>& sourceWorldTransforms = hasCachedSourceWorldTransforms
		? cachedSourceWorldTransforms
		: fallbackSourceWorldTransforms;

	std::vector<uint32_t> rootMeshIndices;
	std::vector<bool> meshAttached(model->meshes.size(), false);
	for (const Scene::NodeInfo& nodeInfo : model->nodes) {
		for (uint32_t meshIndex : nodeInfo.meshIndices) {
			if (meshIndex < meshAttached.size()) {
				meshAttached[meshIndex] = true;
			}
		}
	}
	for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(meshAttached.size()); ++meshIndex) {
		if (!meshAttached[meshIndex]) {
			rootMeshIndices.push_back(meshIndex);
		}
	}

	node->renderWholeModel = false;
	node->renderMeshIndices = rootMeshIndices;
	node->boundingRadius = ComputeRenderableBoundingRadius(*model, rootMeshIndices, -1);

	ComponentManager* componentManager = m_currentSceneGraph ? m_currentSceneGraph->GetComponentManager() : nullptr;
	TransformSystem* transformSystem = m_currentSceneGraph ? m_currentSceneGraph->GetTransformSystem() : nullptr;

	size_t invalidLocalTransformCount = 0;
	size_t reparentedToRootCount = 0;
	size_t correctedLocalTransformCount = 0;

	std::vector<std::shared_ptr<SceneNode>> importedNodes(model->nodes.size());
	for (size_t nodeIndex = 0; nodeIndex < model->nodes.size(); ++nodeIndex) {
		const Scene::NodeInfo& nodeInfo = model->nodes[nodeIndex];
		const bool hasRenderableMeshes = !nodeInfo.meshIndices.empty();
		glm::mat4 localTransform = nodeInfo.localTransform;
		if (!IsFiniteMatrix(localTransform)) {
			localTransform = glm::mat4(1.0f);
			++invalidLocalTransformCount;
		}

		auto importedNode = std::make_shared<SceneNode>(componentManager, transformSystem);
		importedNode->SetNodeType(hasRenderableMeshes ? SceneNode::MODEL : SceneNode::NODE);
		importedNode->SetShader(node->GetShader());
		importedNode->SetCullingOverride(node->GetCullingOverride());
		importedNode->SetTransform(localTransform);
		importedNode->SetName(!nodeInfo.name.empty()
			? nodeInfo.name
			: node->GetName() + "_node_" + std::to_string(nodeIndex));
		importedNode->nodeIndex = static_cast<int>(nodeIndex);

		if (hasRenderableMeshes) {
			importedNode->SetModel(model);
			importedNode->renderWholeModel = false;
			importedNode->renderMeshIndices = nodeInfo.meshIndices;
			importedNode->boundingRadius = ComputeRenderableBoundingRadius(*model, nodeInfo.meshIndices, static_cast<int>(nodeIndex));
		}

		importedNodes[nodeIndex] = importedNode;
	}

	for (size_t nodeIndex = 0; nodeIndex < model->nodes.size(); ++nodeIndex) {
		auto& importedNode = importedNodes[nodeIndex];
		if (!importedNode) {
			continue;
		}

		const int parentIndex = model->nodes[nodeIndex].parent;
		if (parentIndex >= 0 && parentIndex < static_cast<int>(importedNodes.size()) && importedNodes[parentIndex]) {
			importedNodes[parentIndex]->AddChild(importedNode);
		}
		else {
			if (parentIndex >= 0 && nodeIndex < sourceWorldTransforms.size()) {
				// Preserve authored world placement if malformed parent links force a root fallback.
				importedNode->SetTransform(sourceWorldTransforms[nodeIndex]);
				++reparentedToRootCount;
			}
			node->AddChild(importedNode);
		}
	}

	std::unordered_map<int, glm::mat4> importedWorldCache;
	std::function<glm::mat4(int)> computeImportedWorld = [&](int nodeIndex) -> glm::mat4 {
		auto cached = importedWorldCache.find(nodeIndex);
		if (cached != importedWorldCache.end()) {
			return cached->second;
		}

		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model->nodes.size()) || !importedNodes[nodeIndex]) {
			return glm::mat4(1.0f);
		}

		glm::mat4 localTransform = importedNodes[nodeIndex]->GetTransform();
		if (!IsFiniteMatrix(localTransform)) {
			localTransform = glm::mat4(1.0f);
		}

		const int parentIndex = model->nodes[nodeIndex].parent;
		glm::mat4 worldTransform = (parentIndex >= 0)
			? computeImportedWorld(parentIndex) * localTransform
			: localTransform;
		importedWorldCache[nodeIndex] = worldTransform;
		return worldTransform;
	};

	for (size_t nodeIndex = 0; nodeIndex < importedNodes.size(); ++nodeIndex) {
		if (!importedNodes[nodeIndex] || nodeIndex >= sourceWorldTransforms.size()) {
			continue;
		}

		glm::mat4 importedWorld = computeImportedWorld(static_cast<int>(nodeIndex));
		const glm::mat4& sourceWorld = sourceWorldTransforms[nodeIndex];
		if (!IsFiniteMatrix(importedWorld) || !IsFiniteMatrix(sourceWorld)) {
			continue;
		}

		if (MaxMatrixAbsDelta(importedWorld, sourceWorld) <= kImportedTransformTolerance) {
			continue;
		}

		glm::mat4 correctedLocal = sourceWorld;
		const int parentIndex = model->nodes[nodeIndex].parent;
		if (parentIndex >= 0 && parentIndex < static_cast<int>(sourceWorldTransforms.size())) {
			glm::mat4 parentInverse = glm::inverse(sourceWorldTransforms[parentIndex]);
			if (IsFiniteMatrix(parentInverse)) {
				correctedLocal = parentInverse * sourceWorld;
			}
		}

		if (IsFiniteMatrix(correctedLocal)) {
			importedNodes[nodeIndex]->SetTransform(correctedLocal);
			importedWorldCache.clear();
			++correctedLocalTransformCount;
		}
	}

	if (invalidLocalTransformCount > 0 || reparentedToRootCount > 0 || correctedLocalTransformCount > 0) {
		std::cout << "[SceneLoader] Imported transform diagnostics for model node '" << node->GetName()
			<< "': invalidLocal=" << invalidLocalTransformCount
			<< ", reparentedToRoot=" << reparentedToRootCount
			<< ", correctedLocal=" << correctedLocalTransformCount
			<< std::endl;
	}
}

void SceneLoader::CreateAnimationComponent(std::shared_ptr<SceneNode> node, const std::shared_ptr<Scene>& model) {
	if (!node || !model || !m_currentSceneGraph) return;

	ComponentManager* componentManager = m_currentSceneGraph->GetComponentManager();
	EntityID entityID = node->GetEntityID();
	if (!componentManager || entityID == INVALID_ENTITY) return;

	// Only create if model has animations
	if (model->animations.empty()) return;

	AnimationComponent animComp;
	animComp.isPlaying = false;
	animComp.isPaused = false;
	animComp.animationTime = 0.0f;
	animComp.currentAnimationIndex = -1;
	// Controller will be created when animation is played

	componentManager->AddAnimation(entityID, animComp);

	if constexpr (kVerboseEntityCreationLogs) {
		std::cout << "[SceneLoader] Created AnimationComponent for entity " << entityID
			<< " with " << model->animations.size() << " animations available" << std::endl;
	}
}

void SceneLoader::ReportProgress(float progress, const std::string& stage) {
	progress = std::min(std::max(progress, 0.0f), 1.0f);

	// Publish via event bus for GUI systems
	GuiEventBus::GetInstance().PublishProgress(progress, stage);

	// Also call callback if registered
	if (m_progressCallback) {
		m_progressCallback(progress, stage);
	}
}

void SceneLoader::ReportAssetLoadingProgress(float t, int current, int total) {
	if (total <= 0) return;

	float assetProgress = (float)current / (float)total;
	float overallProgress = assetProgress * 0.3f;  // Assets are first 30% of load
	ReportProgress(overallProgress, "Loading Assets (" + std::to_string(current) + "/" + std::to_string(total) + ")");
}

void SceneLoader::ReportECSCreationProgress(float t, int current, int total) {
	if (total <= 0) return;

	float ecsProgress = (float)current / (float)total;
	float overallProgress = 0.3f + ecsProgress * 0.35f;  // ECS creation is 35% of load
	ReportProgress(overallProgress, "Creating ECS Entities (" + std::to_string(current) + "/" + std::to_string(total) + ")");
}


// Runtime State Management


bool SceneLoader::SaveRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot save runtime state: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Saving runtime state to: " << filepath << std::endl;

	try {
		nlohmann::json stateJson;

		// Version for forward/backward compatibility
		stateJson["version"] = "1.0";
		stateJson["format"] = "runtime_state";
		stateJson["timestamp"] = std::time(nullptr);

		// Serialize camera state
		if (camera) {
			nlohmann::json cameraJson;

			glm::vec3 pos = camera->GetCameraPosition();
			cameraJson["position"] = { pos.x, pos.y, pos.z };

			glm::vec3 front = camera->GetCameraFrontVector();
			cameraJson["front"] = { front.x, front.y, front.z };

			glm::vec3 up = camera->GetCameraUpVector();
			cameraJson["up"] = { up.x, up.y, up.z };

			cameraJson["fov"] = camera->GetCameraFov();
			cameraJson["near_plane"] = camera->GetCameraNearPlane();
			cameraJson["far_plane"] = camera->GetCameraFarPlane();
			cameraJson["movement_speed"] = camera->GetCameraMovementSpeed();
			cameraJson["mouse_sensitivity"] = camera->GetCameraMouseSensitivity();

			stateJson["camera"] = cameraJson;
		}

		// Serialize scene metadata
		stateJson["scene_name"] = sceneGraph->GetSceneName();
		stateJson["exposure"] = sceneGraph->m_exposure;
		stateJson["gamma"] = sceneGraph->m_gamma;
		stateJson["physics_enabled"] = sceneGraph->IsPhysicsEnabled();

		// Serialize all nodes with runtime state
		nlohmann::json nodesArray = nlohmann::json::array();
		auto root = sceneGraph->GetRoot();

		for (const auto& child : root->children) {
			if (child) {
				nlohmann::json nodeJson = SerializeNodeTree(child, false, false);
				if (!nodeJson.is_null()) {
					// Add runtime-specific data
					SerializeRuntimeNodeState(child, nodeJson, sceneGraph);
					nodesArray.push_back(nodeJson);
				}
			}
		}

		stateJson["nodes"] = nodesArray;

		// Runtime state is per-user output
		std::string pathError;
		const auto userPath = ResolveUserPath(filepath, &pathError);
		if (!userPath) {
			std::cerr << "[SceneLoader] Cannot save runtime state: " << pathError << std::endl;
			return false;
		}
		std::ofstream outFile(*userPath);
		if (!outFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open file for writing: " << userPath->string() << std::endl;
			return false;
		}

		outFile << stateJson.dump(2);
		outFile.close();

		std::cout << "[SceneLoader] Successfully saved runtime state with "
			<< nodesArray.size() << " nodes" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error saving runtime state: " << e.what() << std::endl;
		return false;
	}
}

bool SceneLoader::LoadRuntimeState(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::string& filepath) {
	if (!sceneGraph || !sceneGraph->GetRoot()) {
		std::cerr << "[SceneLoader] Cannot load runtime state: invalid scene graph" << std::endl;
		return false;
	}

	std::cout << "[SceneLoader] Loading runtime state from: " << filepath << std::endl;

	try {
		std::string pathError;
		const auto userPath = ResolveUserPath(filepath, &pathError);
		std::ifstream inFile(userPath ? *userPath : fs::path(filepath));
		if (!userPath || !inFile.is_open()) {
			std::cerr << "[SceneLoader] Failed to open runtime state file: " << filepath << std::endl;
			return false;
		}

		nlohmann::json stateJson;
		inFile >> stateJson;
		inFile.close();

		// Version check
		std::string version = stateJson.value("version", "unknown");
		std::string format = stateJson.value("format", "unknown");

		if (format != "runtime_state") {
			std::cerr << "[SceneLoader] Invalid runtime state format: " << format << std::endl;
			return false;
		}

		std::cout << "[SceneLoader] Loading runtime state version: " << version << std::endl;

		// Restore camera state
		if (camera && stateJson.contains("camera")) {
			const auto& cameraJson = stateJson["camera"];

			if (cameraJson.contains("position") && cameraJson["position"].is_array()) {
				glm::vec3 pos(
					cameraJson["position"][0],
					cameraJson["position"][1],
					cameraJson["position"][2]
				);
				camera->SetPosition(pos);
			}

			// Note: Camera doesn't expose setters for front/up vectors directly
			// They are derived from yaw/pitch which we'd need to calculate
			// For now, position restore is most critical
		}

		// Restore scene metadata
		if (stateJson.contains("exposure")) {
			sceneGraph->m_exposure = stateJson["exposure"];
		}
		if (stateJson.contains("gamma")) {
			sceneGraph->m_gamma = stateJson["gamma"];
		}

		// Restore node states
		if (stateJson.contains("nodes") && stateJson["nodes"].is_array()) {
			auto root = sceneGraph->GetRoot();
			RestoreNodeStates(stateJson["nodes"], root->children, sceneGraph);
		}

		std::cout << "[SceneLoader] Successfully loaded runtime state" << std::endl;
		return true;

	}
	catch (const std::exception& e) {
		std::cerr << "[SceneLoader] Error loading runtime state: " << e.what() << std::endl;
		return false;
	}
}

void SceneLoader::SerializeRuntimeNodeState(const std::shared_ptr<SceneNode>& node,
	nlohmann::json& nodeJson,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node || !sceneGraph) return;

	EntityID entityID = node->GetEntityID();
	if (entityID == INVALID_ENTITY) return;

	ComponentManager* cm = sceneGraph->GetComponentManager();
	if (!cm) return;

	// Add runtime section
	nlohmann::json runtimeJson;

	// Animation state
	if (cm->HasAnimation(entityID)) {
		auto* animComp = cm->GetAnimation(entityID);
		if (animComp) {
			nlohmann::json animJson;
			animJson["current_animation_index"] = animComp->currentAnimationIndex;
			animJson["animation_time"] = animComp->animationTime;
			animJson["is_playing"] = animComp->isPlaying;
			animJson["is_paused"] = animComp->isPaused;

			if (!animComp->morphWeights.empty()) {
				animJson["morph_weights"] = animComp->morphWeights;
			}

			runtimeJson["animation"] = animJson;
		}
	}

	// Physics state (velocity, angular velocity, etc.)
	auto rb = node->GetRigidBody();
	if (rb) {
		nlohmann::json physicsJson;

		glm::vec3 velocity = rb->getVelocity();
		physicsJson["velocity"] = { velocity.x, velocity.y, velocity.z };

		glm::vec3 angularVel = rb->getAngularVelocity();
		physicsJson["angular_velocity"] = { angularVel.x, angularVel.y, angularVel.z };

		glm::vec3 acceleration = rb->getAcceleration();
		physicsJson["acceleration"] = { acceleration.x, acceleration.y, acceleration.z };

		runtimeJson["physics"] = physicsJson;
	}

	// Light state (if it's a light node with runtime modifications)
	if (node->GetNodeType() == SceneNode::LIGHT) {
		auto lightNode = std::dynamic_pointer_cast<LightNode>(node);
		if (lightNode && lightNode->GetLight()) {
			// Light properties already serialized in SerializeLightProperties
			// But add runtime-specific state here if needed
		}
	}

	// Store runtime data if any was added
	if (!runtimeJson.empty()) {
		nodeJson["runtime_state"] = runtimeJson;
	}

	// Recursively serialize children's runtime state
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		size_t childIndex = 0;
		for (const auto& child : node->children) {
			if (child && childIndex < nodeJson["children"].size()) {
				SerializeRuntimeNodeState(child, nodeJson["children"][childIndex], sceneGraph);
				childIndex++;
			}
		}
	}
}

void SceneLoader::RestoreNodeStates(const nlohmann::json& nodesArray,
	const std::vector<std::shared_ptr<SceneNode>>& nodes,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!nodesArray.is_array() || nodes.empty()) return;

	// The saved state preserves the exact order of nodes, so index matching is most accurate
	size_t nodeCount = std::min(nodesArray.size(), nodes.size());

	for (size_t i = 0; i < nodeCount; ++i) {
		const auto& nodeJson = nodesArray[i];
		const auto& node = nodes[i];

		if (!node) continue;

		// Verify type matches as a sanity check
		std::string savedType = nodeJson.value("type", "node");
		std::string actualType;
		switch (node->GetNodeType()) {
		case SceneNode::MODEL:     actualType = "model"; break;
		case SceneNode::LIGHT:     actualType = "light"; break;
		case SceneNode::AUDIO:     actualType = "audio"; break;
		case SceneNode::CAMERA:    actualType = "camera"; break;
		case SceneNode::GUI:       actualType = "gui"; break;
		case SceneNode::LPV_VOLUME: actualType = "lpv_volume"; break;
		case SceneNode::SKELETAL:  actualType = "skeletal"; break;
		case SceneNode::FOLDER:    actualType = "folder"; break;
		default:                   actualType = "node"; break;
		}

		// Log if there's a type mismatch (shouldn't happen if scene hasn't changed)
		if (savedType != actualType) {
			std::cerr << "[SceneLoader] Warning: Type mismatch at index " << i
				<< " - saved: " << savedType << ", actual: " << actualType << std::endl;
		}

		// Restore the node state
		RestoreNodeState(nodeJson, node, sceneGraph);
	}

	// Log if counts don't match
	if (nodesArray.size() != nodes.size()) {
		std::cerr << "[SceneLoader] Warning: Node count mismatch - saved: "
			<< nodesArray.size() << ", actual: " << nodes.size() << std::endl;
	}
}

void SceneLoader::RestoreNodeState(const nlohmann::json& nodeJson,
	const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneGraph>& sceneGraph) {
	if (!node || !sceneGraph) return;

	// Restore transform - use SetLocalTRS for proper rotation handling
	glm::vec3 pos = node->GetPosition();
	glm::vec3 scale = node->GetScale();
	glm::quat rotation = node->GetOrientation();

	// Extract position if present
	if (nodeJson.contains("position") && nodeJson["position"].is_array() && nodeJson["position"].size() == 3) {
		pos = glm::vec3(
			nodeJson["position"][0],
			nodeJson["position"][1],
			nodeJson["position"][2]
		);
	}

	// Extract rotation if present - stored in degrees, convert to quaternion
	if (nodeJson.contains("rotation") && nodeJson["rotation"].is_array() && nodeJson["rotation"].size() == 3) {
		// Rotation is stored in degrees, convert to radians then to quaternion
		glm::vec3 rotDegrees(
			nodeJson["rotation"][0],
			nodeJson["rotation"][1],
			nodeJson["rotation"][2]
		);
		glm::vec3 rotRadians = glm::radians(rotDegrees);
		// Use glm::quat from Euler angles - this correctly combines all three rotations
		rotation = glm::quat(rotRadians);
	}

	// Extract scale if present
	if (nodeJson.contains("scale") && nodeJson["scale"].is_array() && nodeJson["scale"].size() == 3) {
		scale = glm::vec3(
			nodeJson["scale"][0],
			nodeJson["scale"][1],
			nodeJson["scale"][2]
		);
	}

	// Apply transform using SetLocalTRS which correctly combines TRS
	node->SetLocalTRS(pos, rotation, scale);

	// Sync transform to ECS if entity exists
	if (node->GetEntityID() != INVALID_ENTITY) {
		node->SyncToECS();
	}

	// Always sync physics body to match restored transform (even without runtime_state)
	auto rb = node->GetRigidBody();
	if (rb) {
		rb->setPosition(node->GetWorldPosition());
		rb->setOrientation(node->GetOrientation());
		rb->computeAABB();
		rb->storePreviousState();
	}

	// Restore runtime state if present
	if (nodeJson.contains("runtime_state")) {
		const auto& runtimeJson = nodeJson["runtime_state"];

		EntityID entityID = node->GetEntityID();
		ComponentManager* cm = sceneGraph->GetComponentManager();

		if (entityID != INVALID_ENTITY && cm) {
			// Restore animation state
			if (runtimeJson.contains("animation") && cm->HasAnimation(entityID)) {
				const auto& animJson = runtimeJson["animation"];
				auto* animComp = cm->GetAnimation(entityID);

				if (animComp) {
					animComp->currentAnimationIndex = animJson.value("current_animation_index", -1);
					animComp->animationTime = animJson.value("animation_time", 0.0f);
					animComp->isPlaying = animJson.value("is_playing", false);
					animComp->isPaused = animJson.value("is_paused", false);

					if (animJson.contains("morph_weights") && animJson["morph_weights"].is_array()) {
						animComp->morphWeights.clear();
						for (const auto& weight : animJson["morph_weights"]) {
							animComp->morphWeights.push_back(weight);
						}
					}
				}
			}

			// Restore physics runtime state (velocity, angular velocity, etc.)
			if (runtimeJson.contains("physics") && rb) {
				const auto& physicsJson = runtimeJson["physics"];

				if (physicsJson.contains("velocity") && physicsJson["velocity"].is_array()) {
					glm::vec3 velocity(
						physicsJson["velocity"][0],
						physicsJson["velocity"][1],
						physicsJson["velocity"][2]
					);
					rb->setVelocity(velocity);
				}

				if (physicsJson.contains("angular_velocity") && physicsJson["angular_velocity"].is_array()) {
					glm::vec3 angularVel(
						physicsJson["angular_velocity"][0],
						physicsJson["angular_velocity"][1],
						physicsJson["angular_velocity"][2]
					);
					rb->setAngularVelocity(angularVel);
				}

				if (physicsJson.contains("acceleration") && physicsJson["acceleration"].is_array()) {
					glm::vec3 acceleration(
						physicsJson["acceleration"][0],
						physicsJson["acceleration"][1],
						physicsJson["acceleration"][2]
					);
					rb->setAcceleration(acceleration);
				}
			}
		}
	}

	// Recursively restore children
	if (nodeJson.contains("children") && nodeJson["children"].is_array()) {
		RestoreNodeStates(nodeJson["children"], node->children, sceneGraph);
	}
}

// ---------------------------------------------------------------------------------------------
// Editor helpers (docs/nfs2015/game.md): duplicate / add nodes at runtime.
// ---------------------------------------------------------------------------------------------
nlohmann::json SceneLoader::SerializeNodeForEditor(const std::shared_ptr<SceneNode>& node)
{
	return node ? SerializeNodeTree(node, true, false) : nlohmann::json();
}

std::shared_ptr<SceneNode> SceneLoader::InstantiateNode(SceneGraph& graph, const nlohmann::json& nodeJson,
	const std::shared_ptr<SceneNode>& parent, const std::string& sceneFilePath)
{
	if (!nodeJson.is_object() || !parent) {
		return nullptr;
	}
	nlohmann::json expanded = nodeJson;
	std::string prefabError;
	if (!nox::prefab::ExpandReferences(expanded, &prefabError)) {
		std::cerr << "[SceneLoader] ERROR: prefab reference failed: " << prefabError << std::endl;
		return nullptr;
	}
	return InstantiateExpanded(graph, expanded, parent, sceneFilePath);
}

std::shared_ptr<SceneNode> SceneLoader::InstantiateExpanded(SceneGraph& graph, const nlohmann::json& expanded,
	const std::shared_ptr<SceneNode>& parent, const std::string& sceneFilePath)
{
	if (!expanded.is_object() || !parent) {
		return nullptr;
	}
	m_currentSceneGraph = &graph;
	m_currentSceneDirectory = std::filesystem::path(sceneFilePath).parent_path().string();
	m_missingCriticalAssetCount = 0;
	auto node = ProcessNodeRecursive(expanded, parent->GetEntityID());
	if (node) {
		parent->AddChild(node);
		graph.UpdateAllTransforms();
	}
	m_currentSceneGraph = nullptr;
	return node;
}

// ===== Model hot reload (see SceneLoader::RetargetModelNodes) =====

namespace {

// Mesh indices no node claims: the list a model root draws when the model is imported as child nodes.
std::vector<uint32_t> UnclaimedMeshIndices(const Scene& model) {
	std::vector<bool> claimed(model.meshes.size(), false);
	for (const Scene::NodeInfo& info : model.nodes) {
		for (uint32_t meshIndex : info.meshIndices) {
			if (meshIndex < claimed.size()) claimed[meshIndex] = true;
		}
	}
	std::vector<uint32_t> unclaimed;
	for (uint32_t meshIndex = 0; meshIndex < claimed.size(); ++meshIndex) {
		if (!claimed[meshIndex]) unclaimed.push_back(meshIndex);
	}
	return unclaimed;
}

// Sorted names of the nodes a clip animates: the joints it needs from the model.
std::vector<std::string> ClipJointNames(const Scene& model, int clip) {
	std::set<std::string> names;
	if (clip < 0 || clip >= static_cast<int>(model.animations.size())) return {};
	for (const Animation::Channel& channel : model.animations[clip].channels) {
		if (channel.targetNode >= 0 && channel.targetNode < static_cast<int>(model.nodes.size())) {
			names.insert(model.nodes[channel.targetNode].name);
		}
	}
	return std::vector<std::string>(names.begin(), names.end());
}

// The clip of `model` that replaces clip `clip` of `previous`: the same index when its joints still match, else a clip
// with the same name and joints. -1 when none matches.
int MatchingClip(const Scene& previous, const Scene& model, int clip) {
	if (clip < 0 || clip >= static_cast<int>(previous.animations.size())) return -1;
	const std::vector<std::string> joints = ClipJointNames(previous, clip);
	if (clip < static_cast<int>(model.animations.size()) && ClipJointNames(model, clip) == joints) return clip;
	for (int index = 0; index < static_cast<int>(model.animations.size()); ++index) {
		if (model.animations[index].name == previous.animations[clip].name && ClipJointNames(model, index) == joints) {
			return index;
		}
	}
	return -1;
}

void RetargetAnimation(AnimationComponent& anim, const Scene& previous, const Scene& model) {
	const bool fromAnimator = anim.animator && anim.animator->IsBound();
	const int oldClip = fromAnimator ? anim.animator->PrimaryClip() : anim.currentAnimationIndex;
	const float oldTime = fromAnimator ? anim.animator->PrimaryClipTime() : anim.animationTime;
	const bool wasPlaying = fromAnimator ? anim.animator->IsRunning() : anim.isPlaying;
	const int clip = MatchingClip(previous, model, oldClip);

	if (anim.animator) {
		// The scene changed, so the animator builds its bind pose and graph again (Bind resets its state).
		anim.animator->Bind(model);
		if (clip >= 0 && wasPlaying) {
			if (anim.animator->GraphValid()) {
				anim.animator->StartGraph();
			}
			else {
				anim.animator->PlayClip(model, clip, model.animations[clip].isLooping, 0.0f);
				anim.animator->Update(oldTime, model);   // seek to the kept time; the events it fires are dropped
				anim.animator->TakeEvents();
			}
		}
		else if (clip < 0) {
			anim.animator->Reset();
		}
	}
	anim.currentAnimationIndex = clip;
	anim.animationTime = clip >= 0 ? std::min(oldTime, model.animations[clip].duration) : 0.0f;
	anim.isPlaying = clip >= 0 && wasPlaying;
}

} // namespace

int SceneLoader::RetargetModelNodes(ComponentManager* components, const std::vector<std::shared_ptr<SceneNode>>& nodes,
	const Scene& previous, const std::shared_ptr<Scene>& model, const std::string& path)
{
	if (!model || model->nodes.size() != previous.nodes.size()) return -1;

	const bool sameJoints = model->hasSkin == previous.hasSkin && model->skin.joints == previous.skin.joints;
	const std::vector<uint32_t> rootMeshIndices = UnclaimedMeshIndices(*model);
	// The mesh collider is read again once; each node below asks the cache for it.
	MeshCollider::Invalidate(path);

	int retargeted = 0;
	for (const std::shared_ptr<SceneNode>& node : nodes) {
		if (!node || node->GetModel().get() != &previous) continue;
		node->SetModel(model);

		// Skeleton. Joint nodes are children of the skinned node: same joints keep them, new joints replace them.
		if (!sameJoints) {
			const std::vector<std::shared_ptr<SceneNode>> oldBones = node->boneNodes;
			node->children.erase(std::remove_if(node->children.begin(), node->children.end(),
				[&oldBones](const std::shared_ptr<SceneNode>& child) {
					return std::find(oldBones.begin(), oldBones.end(), child) != oldBones.end();
				}), node->children.end());
			node->boneNodes.clear();
			node->boneInverseBindMatrices.clear();
			node->isSkinned = false;
			node->BuildSkeleton(*model);
		}
		else if (node->isSkinned) {
			node->boneInverseBindMatrices = model->skin.inverseBindMatrices;
		}

		// Meshes drawn by this node and its bounds, as the loader sets them.
		if (node->renderWholeModel) {
			std::vector<uint32_t> all(model->meshes.size());
			for (uint32_t meshIndex = 0; meshIndex < all.size(); ++meshIndex) all[meshIndex] = meshIndex;
			node->boundingRadius = ComputeRenderableBoundingRadius(*model, all, -1);
		}
		else {
			const bool imported = node->nodeIndex >= 0 && node->nodeIndex < static_cast<int>(model->nodes.size());
			node->renderMeshIndices = imported ? model->nodes[node->nodeIndex].meshIndices : rootMeshIndices;
			node->boundingRadius = ComputeRenderableBoundingRadius(*model, node->renderMeshIndices,
				imported ? node->nodeIndex : -1);
			// A child node named after the glTF node keeps following it when the file renames it.
			if (imported && !model->nodes[node->nodeIndex].name.empty()
				&& node->GetName() == previous.nodes[node->nodeIndex].name) {
				node->SetName(model->nodes[node->nodeIndex].name);
			}
		}

		// Material overrides from the scene file, applied to the new copy as the loader applied them.
		if (!node->GetAuthoredExtras().empty()) {
			const json extras = json::parse(node->GetAuthoredExtras(), nullptr, false);
			if (extras.is_object() && extras.contains("material_overrides")) {
				ApplyMaterialOverrides(*model, extras["material_overrides"], node->GetName());
			}
		}

		// Mesh collider: the body keeps its transform and state; its mesh is read from the file again.
		if (auto body = node->GetRigidBody(); body && body->getMeshPath() == path) {
			if (auto mesh = MeshCollider::LoadShared(path)) {
				glm::vec3 mn(0.0f), mx(0.0f);
				MeshCollider::LocalBounds(*mesh, mn, mx);
				body->setMesh(mesh, mn, mx, path);
				body->computeInertiaTensor();
				body->computeAABB();
			}
		}

		// Renderable: removed and added again, which bumps the renderable revision the renderer caches by.
		if (components && node->HasECSEntity()) {
			const EntityID entity = node->GetEntityID();
			if (components->HasRenderable(entity)) components->RemoveRenderable(entity);
			if (node->renderWholeModel || !node->renderMeshIndices.empty()) {
				RenderableComponent renderable;
				renderable.model = model;
				renderable.shaderID = node->GetShader();
				renderable.boundingRadius = node->boundingRadius;
				renderable.isSkinned = node->isSkinned;
				renderable.renderWholeModel = node->renderWholeModel;
				renderable.cullingOverride = static_cast<::CullingOverride>(static_cast<uint8_t>(node->GetCullingOverride()));
				renderable.meshIndices = node->renderMeshIndices;
				renderable.boneNodes = node->boneNodes;
				renderable.boneInverseBindMatrices = node->boneInverseBindMatrices;
				renderable.nodeIndex = node->nodeIndex;
				for (size_t meshIndex = 0; meshIndex < model->meshes.size(); ++meshIndex) {
					const bool drawn = node->renderWholeModel || std::find(node->renderMeshIndices.begin(),
						node->renderMeshIndices.end(), static_cast<uint32_t>(meshIndex)) != node->renderMeshIndices.end();
					if (drawn && model->meshes[meshIndex].RequiresAlphaBlending()) renderable.hasAlpha = true;
				}
				components->AddRenderable(entity, renderable);
			}
		}

		if (AnimationComponent* anim = node->GetAnimationComponent()) {
			RetargetAnimation(*anim, previous, *model);
		}
		++retargeted;
	}
	return retargeted;
}
