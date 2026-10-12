#include "cook/CookedResults.h"

#include "assets/AssetIO.h"
#include "cook/Sha256.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <set>

using json = nlohmann::json;

namespace CookedResults {

namespace {

	bool Fail(std::string* error, const std::string& message)
	{
		if (error) *error = message;
		return false;
	}

	bool IsSafeFileName(const std::string& name)
	{
		return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
			name.find('\\') == std::string::npos && name.find(':') == std::string::npos;
	}

	std::string LastComponent(const std::string& path)
	{
		const size_t slash = path.find_last_of('/');
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	bool IsHexDigest(const std::string& text)
	{
		if (text.size() != 64) return false;
		return std::all_of(text.begin(), text.end(), [](char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		});
	}

	// ---- JSON helpers with explicit errors. Floats are stored as doubles, which hold every float exactly.
	json Floats(const float* values, int count)
	{
		json array = json::array();
		for (int i = 0; i < count; ++i) array.push_back(static_cast<double>(values[i]));
		return array;
	}

	bool ReadFloats(const json& object, const char* key, float* out, int count, std::string* error)
	{
		if (!object.is_object() || !object.contains(key)) return Fail(error, std::string("missing '") + key + "'");
		const json& array = object[key];
		if (!array.is_array() || static_cast<int>(array.size()) != count) {
			return Fail(error, std::string("'") + key + "' must hold " + std::to_string(count) + " numbers");
		}
		for (int i = 0; i < count; ++i) {
			if (!array[i].is_number()) return Fail(error, std::string("'") + key + "' holds a non-number");
			const double value = array[i].get<double>();
			if (!std::isfinite(value)) return Fail(error, std::string("'") + key + "' is not finite");
			out[i] = static_cast<float>(value);
		}
		return true;
	}

	bool ReadFloat(const json& object, const char* key, float& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_number()) {
			return Fail(error, std::string("missing number '") + key + "'");
		}
		const double value = object[key].get<double>();
		if (!std::isfinite(value)) return Fail(error, std::string("'") + key + "' is not finite");
		out = static_cast<float>(value);
		return true;
	}

	bool ReadInt(const json& object, const char* key, int& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer()) {
			return Fail(error, std::string("missing integer '") + key + "'");
		}
		out = object[key].get<int>();
		return true;
	}

	bool ReadBool(const json& object, const char* key, bool& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_boolean()) {
			return Fail(error, std::string("missing boolean '") + key + "'");
		}
		out = object[key].get<bool>();
		return true;
	}

	bool ReadString(const json& object, const char* key, std::string& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
			return Fail(error, std::string("missing string '") + key + "'");
		}
		out = object[key].get<std::string>();
		return true;
	}

	bool ReadArray(const json& object, const char* key, const json*& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
			return Fail(error, std::string("missing array '") + key + "'");
		}
		out = &object[key];
		return true;
	}

	bool ReadObject(const json& object, const char* key, const json*& out, std::string* error)
	{
		if (!object.is_object() || !object.contains(key) || !object[key].is_object()) {
			return Fail(error, std::string("missing object '") + key + "'");
		}
		out = &object[key];
		return true;
	}

	json Vec3ListToJson(const std::vector<glm::vec3>& values)
	{
		json array = json::array();
		for (const glm::vec3& v : values) array.push_back(Floats(&v.x, 3));
		return array;
	}

	bool Vec3ListFromJson(const json& object, const char* key, std::vector<glm::vec3>& out, std::string* error)
	{
		const json* array = nullptr;
		if (!ReadArray(object, key, array, error)) return false;
		out.clear();
		for (const json& entry : *array) {
			glm::vec3 v;
			if (!entry.is_array() || entry.size() != 3) return Fail(error, std::string("'") + key + "' entry must hold 3 numbers");
			for (int i = 0; i < 3; ++i) {
				if (!entry[i].is_number()) return Fail(error, std::string("'") + key + "' holds a non-number");
				v[i] = static_cast<float>(entry[i].get<double>());
			}
			out.push_back(v);
		}
		return true;
	}

	// ---- Texture arrays (TextureArrayBuilder) -------------------------------------------------------
	json LayersToJson(const std::vector<TextureArrayBuilder::Layer>& layers)
	{
		json array = json::array();
		for (const auto& layer : layers) {
			array.push_back({ { "file", layer.file }, { "rect", Floats(&layer.rect.x, 4) } });
		}
		return array;
	}

	bool LayersFromJson(const json& object, const char* key, std::vector<TextureArrayBuilder::Layer>& out, std::string* error)
	{
		const json* array = nullptr;
		if (!ReadArray(object, key, array, error)) return false;
		out.clear();
		for (const json& entry : *array) {
			TextureArrayBuilder::Layer layer;
			if (!ReadString(entry, "file", layer.file, error) || !ReadFloats(entry, "rect", &layer.rect.x, 4, error)) return false;
			out.push_back(std::move(layer));
		}
		return true;
	}

	json OptionsToJson(const TextureArrayBuilder::Options& options)
	{
		return {
			{ "size", options.size },
			{ "premultiply", options.premultiply },
			{ "alpha_floor", static_cast<double>(options.alphaFloor) },
			{ "anisotropic", options.anisotropic },
			{ "debug_name", options.debugName },
		};
	}

	bool OptionsFromJson(const json& object, const char* key, TextureArrayBuilder::Options& out, std::string* error)
	{
		const json* o = nullptr;
		if (!ReadObject(object, key, o, error)) return false;
		return ReadInt(*o, "size", out.size, error) && ReadBool(*o, "premultiply", out.premultiply, error) &&
			ReadFloat(*o, "alpha_floor", out.alphaFloor, error) && ReadBool(*o, "anisotropic", out.anisotropic, error) &&
			ReadString(*o, "debug_name", out.debugName, error);
	}

	const char* const kFaceRoleNames[] = { "back", "left", "right", "ceiling", "floor" };
	static_assert(sizeof(kFaceRoleNames) / sizeof(kFaceRoleNames[0]) == static_cast<size_t>(InteriorAtlas::FaceRole::Count));

	json FloatListToJson(const std::vector<float>& values)
	{
		json array = json::array();
		for (float v : values) array.push_back(static_cast<double>(v));
		return array;
	}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Identity and key
// ---------------------------------------------------------------------------------------------------

std::string CanonicalJson(const json& value)
{
	return value.dump();   // nlohmann objects are sorted maps; numbers use shortest round-trip text
}

json WithoutOutput(const json& params)
{
	json copy = params;
	if (copy.is_object()) copy.erase("output");
	return copy;
}

std::string ParamsHash(const json& params)
{
	return Sha256::HexOf(CanonicalJson(WithoutOutput(params)));
}

json SceneGameSubset(const json& sceneGame, const std::vector<std::string>& keys)
{
	json subset = json::object();
	if (!sceneGame.is_object()) return subset;
	for (const std::string& key : keys) {
		if (sceneGame.contains(key)) subset[key] = sceneGame[key];
	}
	return subset;
}

namespace {
	// Mirrors tools/AssetPackaging.ps1 (NoxRuntimeExtensions and the directory child filter): only files the
	// packer would ship count as inputs, so loose and packed runs compute the same key.
	bool IsPackagedInput(const std::string& path)
	{
		static const char* const kExtensions[] = { ".glb", ".gltf", ".bin", ".fbx", ".obj", ".mtl", ".hdr", ".exr",
			".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".ktx", ".ktx2", ".ttf", ".otf", ".wav", ".ogg", ".mp3",
			".json", ".ini", ".glsl", ".vert", ".frag", ".geom", ".comp", ".lua" };
		std::string lower = path;
		for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const std::string name = LastComponent(lower);
		if (name == "generation_prompts.json") return false;
		for (const char* segment : { "/src/", "/source/", "/sources/", "/raw/", "/originals/", "/__pycache__/" }) {
			if (("/" + lower).find(segment) != std::string::npos) return false;
		}
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos) return false;
		const std::string extension = name.substr(dot);
		for (const char* allowed : kExtensions) if (extension == allowed) return true;
		return false;
	}
}

bool HashInputs(const ProceduralGeneratorInfo& info, const json& params, std::vector<InputHash>& out, std::string* error)
{
	std::vector<std::string> directories = info.inputDirs;
	for (const std::string& name : info.inputParams) {
		if (params.is_object() && params.contains(name) && params[name].is_string()) {
			directories.push_back(params[name].get<std::string>());
		}
	}
	std::set<std::string> files;   // sorted, de-duplicated by logical path
	for (const std::string& directory : directories) {
		if (directory.empty()) continue;
		AssetIO::ListOptions options;
		options.recursive = true;
		for (const std::string& file : AssetIO::List(directory, options)) {
			if (IsPackagedInput(file)) files.insert(file);
		}
	}
	for (const std::string& file : info.inputFiles) {
		if (AssetIO::Exists(file)) files.insert(file);
	}
	out.clear();
	for (const std::string& file : files) {
		std::string readError;
		const AssetIO::Buffer bytes = AssetIO::ReadAll(file, &readError);
		if (!bytes) return Fail(error, "cannot read generator input '" + file + "': " + readError);
		Sha256::Hasher hasher;
		hasher.Update(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
		out.push_back({ file, hasher.FinishHex() });
	}
	return true;
}

std::string ComputeKey(const std::string& generator, int generatorVersion, const json& params,
	const json& sceneGameSubset, const std::vector<InputHash>& inputs)
{
	json inputArray = json::array();
	for (const InputHash& input : inputs) inputArray.push_back({ { "path", input.path }, { "sha256", input.sha256 } });
	const json document = {
		{ "format", kFormatVersion },
		{ "generator", generator },
		{ "generator_version", generatorVersion },
		{ "params", WithoutOutput(params) },
		{ "scene_game", sceneGameSubset },
		{ "inputs", inputArray },
	};
	return Sha256::HexOf(CanonicalJson(document));
}

std::optional<NodeIdentity> Identify(const std::string& generator, const ProceduralGeneratorInfo& info,
	const json& rawParams, const json& sceneGame, std::string* error)
{
	if (!rawParams.is_object() && !rawParams.is_null()) {
		Fail(error, "procedural params of '" + generator + "' must be an object");
		return std::nullopt;
	}
	const json params = rawParams.is_null() ? json::object() : rawParams;
	NodeIdentity identity;
	identity.generator = generator;
	identity.generatorVersion = info.version;
	identity.defaultOutput = info.defaultOutput;
	identity.sceneGameKeys = info.sceneGameKeys;
	identity.params = WithoutOutput(params);
	identity.sceneGame = SceneGameSubset(sceneGame, info.sceneGameKeys);
	if (!HashInputs(info, params, identity.inputs, error)) return std::nullopt;
	identity.key = ComputeKey(generator, info.version, identity.params, identity.sceneGame, identity.inputs);
	identity.dir = std::string(kRootDir) + "/" + generator + "-" + identity.key;
	return identity;
}

// ---------------------------------------------------------------------------------------------------
// Render extras
// ---------------------------------------------------------------------------------------------------

json RenderExtrasToJson(const SceneRenderExtras& extras)
{
	json decalSets = json::array();
	for (const DecalSet& set : extras.decalSets) {
		json instances = json::array();
		for (const DecalInstance& d : set.instances) {
			instances.push_back({
				{ "center", Floats(&d.center.x, 3) },
				{ "axis_u", Floats(&d.axisU.x, 3) },
				{ "axis_v", Floats(&d.axisV.x, 3) },
				{ "uv_rect", Floats(&d.uvRect.x, 4) },
				{ "layer", d.layer },
				{ "tint", Floats(&d.tint.x, 4) },
			});
		}
		decalSets.push_back({
			{ "name", set.name },
			{ "layers", LayersToJson(set.layers) },
			{ "texture_options", OptionsToJson(set.textureOptions) },
			{ "material", {
				{ "albedo_scale", static_cast<double>(set.material.albedoScale) },
				{ "roughness", static_cast<double>(set.material.roughness) },
				{ "wetness_response", static_cast<double>(set.material.wetnessResponse) },
				{ "puddle_response", static_cast<double>(set.material.puddleResponse) },
			} },
			{ "instances", instances },
		});
	}

	json styles = json::array();
	for (const LensFlareStyle& style : extras.lensFlares.styles) {
		json layers = json::array();
		for (const LensFlareLayer& l : style.layers) {
			layers.push_back({
				{ "texture_layer", l.textureLayer },
				{ "gain", static_cast<double>(l.gain) },
				{ "width", static_cast<double>(l.width) },
				{ "tint", Floats(&l.tint.x, 3) },
				{ "use_source_tint", l.useSourceTint },
				{ "glare_min", static_cast<double>(l.glareMin) },
				{ "glare_weight", static_cast<double>(l.glareWeight) },
			});
		}
		styles.push_back({ { "layers", layers } });
	}
	json sources = json::array();
	for (const LensFlareSource& s : extras.lensFlares.sources) {
		sources.push_back({
			{ "position", Floats(&s.position.x, 3) },
			{ "style", s.style },
			{ "facing", Floats(&s.facing.x, 3) },
			{ "luminance", static_cast<double>(s.luminance) },
			{ "tint", Floats(&s.tint.x, 3) },
			{ "size", static_cast<double>(s.size) },
		});
	}
	const LensFlareSet& flares = extras.lensFlares;
	json flareSet = {
		{ "layers", LayersToJson(flares.layers) },
		{ "texture_options", OptionsToJson(flares.textureOptions) },
		{ "styles", styles },
		{ "sources", sources },
		{ "luminance_to_glare", static_cast<double>(flares.luminanceToGlare) },
		{ "gain", static_cast<double>(flares.gain) },
		{ "min_pixels", static_cast<double>(flares.minPixels) },
		{ "max_pixels", static_cast<double>(flares.maxPixels) },
		{ "reference_height", static_cast<double>(flares.referenceHeight) },
		{ "fade_start", static_cast<double>(flares.fadeStart) },
		{ "fade_end", static_cast<double>(flares.fadeEnd) },
		{ "visibility_rate", static_cast<double>(flares.visibilityRate) },
	};

	const InteriorAtlas& atlas = extras.interiorAtlas;
	json faceRoles = json::array();
	for (InteriorAtlas::FaceRole role : atlas.faceRoles) faceRoles.push_back(kFaceRoleNames[static_cast<int>(role)]);
	json faceLayers = json::array();
	for (const auto& layer : atlas.faceLayers) faceLayers.push_back({ { "file", layer.file }, { "rect", Floats(&layer.rect.x, 4) } });
	json styleWeights = json::array();
	for (const auto& row : atlas.styleWeights) styleWeights.push_back(FloatListToJson(row));
	json interior = {
		{ "face_roles", faceRoles },
		{ "face_layers", faceLayers },
		{ "texture_options", OptionsToJson(atlas.textureOptions) },
		{ "face_mean", Vec3ListToJson(atlas.faceMean) },
		{ "room_light", Vec3ListToJson(atlas.roomLight) },
		{ "style_weights", styleWeights },
		{ "room_shape", {
			{ "side_margin", Floats(&atlas.roomShape.sideMargin.x, 2) },
			{ "height", Floats(&atlas.roomShape.height.x, 2) },
			{ "depth_jitter", static_cast<double>(atlas.roomShape.depthJitter) },
			{ "sill_scale", Floats(&atlas.roomShape.sillScale.x, 2) },
		} },
		{ "pane_uv", {
			{ "grid", static_cast<double>(atlas.paneUV.grid) },
			{ "inset", static_cast<double>(atlas.paneUV.inset) },
			{ "extent", static_cast<double>(atlas.paneUV.extent) },
		} },
		{ "far_distance", static_cast<double>(atlas.farDistance) },
	};

	return {
		{ "decal_sets", decalSets },
		{ "lens_flares", flareSet },
		{ "interior_atlas", interior },
	};
}

bool RenderExtrasFromJson(const json& object, SceneRenderExtras& out, std::string* error)
{
	SceneRenderExtras extras;
	const json* decalArray = nullptr;
	if (!ReadArray(object, "decal_sets", decalArray, error)) return false;
	for (const json& setJson : *decalArray) {
		DecalSet set;
		const json* instances = nullptr;
		const json* material = nullptr;
		if (!ReadString(setJson, "name", set.name, error) || !LayersFromJson(setJson, "layers", set.layers, error) ||
			!OptionsFromJson(setJson, "texture_options", set.textureOptions, error) ||
			!ReadObject(setJson, "material", material, error)) return false;
		if (!ReadFloat(*material, "albedo_scale", set.material.albedoScale, error) ||
			!ReadFloat(*material, "roughness", set.material.roughness, error) ||
			!ReadFloat(*material, "wetness_response", set.material.wetnessResponse, error) ||
			!ReadFloat(*material, "puddle_response", set.material.puddleResponse, error)) return false;
		if (!ReadArray(setJson, "instances", instances, error)) return false;
		for (const json& d : *instances) {
			DecalInstance inst;
			if (!ReadFloats(d, "center", &inst.center.x, 3, error) || !ReadFloats(d, "axis_u", &inst.axisU.x, 3, error) ||
				!ReadFloats(d, "axis_v", &inst.axisV.x, 3, error) || !ReadFloats(d, "uv_rect", &inst.uvRect.x, 4, error) ||
				!ReadInt(d, "layer", inst.layer, error) || !ReadFloats(d, "tint", &inst.tint.x, 4, error)) return false;
			set.instances.push_back(inst);
		}
		extras.decalSets.push_back(std::move(set));
	}

	const json* flares = nullptr;
	if (!ReadObject(object, "lens_flares", flares, error)) return false;
	LensFlareSet& flareSet = extras.lensFlares;
	const json* styleArray = nullptr;
	const json* sourceArray = nullptr;
	if (!LayersFromJson(*flares, "layers", flareSet.layers, error) ||
		!OptionsFromJson(*flares, "texture_options", flareSet.textureOptions, error) ||
		!ReadArray(*flares, "styles", styleArray, error) || !ReadArray(*flares, "sources", sourceArray, error)) return false;
	for (const json& styleJson : *styleArray) {
		LensFlareStyle style;
		const json* layerArray = nullptr;
		if (!ReadArray(styleJson, "layers", layerArray, error)) return false;
		for (const json& l : *layerArray) {
			LensFlareLayer layer;
			if (!ReadInt(l, "texture_layer", layer.textureLayer, error) || !ReadFloat(l, "gain", layer.gain, error) ||
				!ReadFloat(l, "width", layer.width, error) || !ReadFloats(l, "tint", &layer.tint.x, 3, error) ||
				!ReadBool(l, "use_source_tint", layer.useSourceTint, error) || !ReadFloat(l, "glare_min", layer.glareMin, error) ||
				!ReadFloat(l, "glare_weight", layer.glareWeight, error)) return false;
			style.layers.push_back(layer);
		}
		flareSet.styles.push_back(std::move(style));
	}
	for (const json& s : *sourceArray) {
		LensFlareSource source;
		if (!ReadFloats(s, "position", &source.position.x, 3, error) || !ReadInt(s, "style", source.style, error) ||
			!ReadFloats(s, "facing", &source.facing.x, 3, error) || !ReadFloat(s, "luminance", source.luminance, error) ||
			!ReadFloats(s, "tint", &source.tint.x, 3, error) || !ReadFloat(s, "size", source.size, error)) return false;
		flareSet.sources.push_back(source);
	}
	if (!ReadFloat(*flares, "luminance_to_glare", flareSet.luminanceToGlare, error) ||
		!ReadFloat(*flares, "gain", flareSet.gain, error) || !ReadFloat(*flares, "min_pixels", flareSet.minPixels, error) ||
		!ReadFloat(*flares, "max_pixels", flareSet.maxPixels, error) ||
		!ReadFloat(*flares, "reference_height", flareSet.referenceHeight, error) ||
		!ReadFloat(*flares, "fade_start", flareSet.fadeStart, error) || !ReadFloat(*flares, "fade_end", flareSet.fadeEnd, error) ||
		!ReadFloat(*flares, "visibility_rate", flareSet.visibilityRate, error)) return false;

	const json* atlasJson = nullptr;
	if (!ReadObject(object, "interior_atlas", atlasJson, error)) return false;
	InteriorAtlas& atlas = extras.interiorAtlas;
	const json* roles = nullptr;
	const json* faceLayers = nullptr;
	const json* weights = nullptr;
	const json* shape = nullptr;
	const json* pane = nullptr;
	if (!ReadArray(*atlasJson, "face_roles", roles, error) || !ReadArray(*atlasJson, "face_layers", faceLayers, error) ||
		!ReadArray(*atlasJson, "style_weights", weights, error) || !ReadObject(*atlasJson, "room_shape", shape, error) ||
		!ReadObject(*atlasJson, "pane_uv", pane, error)) return false;
	for (const json& r : *roles) {
		InteriorAtlas::FaceRole role;
		if (!r.is_string() || !InteriorAtlas::FaceRoleFromName(r.get<std::string>(), role)) return Fail(error, "unknown face role");
		atlas.faceRoles.push_back(role);
	}
	for (const json& l : *faceLayers) {
		TextureArrayBuilder::Layer layer;
		if (!ReadString(l, "file", layer.file, error) || !ReadFloats(l, "rect", &layer.rect.x, 4, error)) return false;
		atlas.faceLayers.push_back(std::move(layer));
	}
	if (!OptionsFromJson(*atlasJson, "texture_options", atlas.textureOptions, error) ||
		!Vec3ListFromJson(*atlasJson, "face_mean", atlas.faceMean, error) ||
		!Vec3ListFromJson(*atlasJson, "room_light", atlas.roomLight, error)) return false;
	for (const json& row : *weights) {
		if (!row.is_array()) return Fail(error, "style weight row is not an array");
		std::vector<float> values;
		for (const json& v : row) {
			if (!v.is_number()) return Fail(error, "style weight is not a number");
			values.push_back(static_cast<float>(v.get<double>()));
		}
		atlas.styleWeights.push_back(std::move(values));
	}
	if (!ReadFloats(*shape, "side_margin", &atlas.roomShape.sideMargin.x, 2, error) ||
		!ReadFloats(*shape, "height", &atlas.roomShape.height.x, 2, error) ||
		!ReadFloat(*shape, "depth_jitter", atlas.roomShape.depthJitter, error) ||
		!ReadFloats(*shape, "sill_scale", &atlas.roomShape.sillScale.x, 2, error)) return false;
	if (!ReadFloat(*pane, "grid", atlas.paneUV.grid, error) || !ReadFloat(*pane, "inset", atlas.paneUV.inset, error) ||
		!ReadFloat(*pane, "extent", atlas.paneUV.extent, error)) return false;
	if (!ReadFloat(*atlasJson, "far_distance", atlas.farDistance, error)) return false;

	out = std::move(extras);   // revision is runtime state, not stored
	return true;
}

// ---------------------------------------------------------------------------------------------------
// result.json
// ---------------------------------------------------------------------------------------------------

json ToJson(const Result& result)
{
	json keys = json::array();
	for (const std::string& key : result.sceneGameKeys) keys.push_back(key);
	json inputs = json::array();
	for (const InputHash& input : result.inputs) inputs.push_back({ { "path", input.path }, { "sha256", input.sha256 } });
	json sides = json::array();
	for (const SideFile& side : result.sideFiles) sides.push_back({ { "suffix", side.suffix }, { "file", side.file } });
	return {
		{ "format", result.format },
		{ "generator", result.generator },
		{ "generator_version", result.generatorVersion },
		{ "key", result.key },
		{ "default_output", result.defaultOutput },
		{ "params", result.params },
		{ "scene_game_keys", keys },
		{ "scene_game", result.sceneGame },
		{ "inputs", inputs },
		{ "model", result.model },
		{ "side_files", sides },
		{ "child_nodes", result.childNodes },
		{ "reflection_probes", result.reflectionProbes },
		{ "has_render_extras", result.hasRenderExtras },
		{ "render_extras", RenderExtrasToJson(result.renderExtras) },
	};
}

bool FromJson(const json& object, Result& out, std::string* error)
{
	if (!object.is_object()) return Fail(error, "result is not a JSON object");
	int format = 0;
	if (!ReadInt(object, "format", format, error)) return false;
	if (format != kFormatVersion) return Fail(error, "unsupported cooked result format " + std::to_string(format));

	Result r;
	r.format = format;
	if (!ReadString(object, "generator", r.generator, error) || !ReadInt(object, "generator_version", r.generatorVersion, error) ||
		!ReadString(object, "key", r.key, error) || !ReadString(object, "default_output", r.defaultOutput, error) ||
		!ReadString(object, "model", r.model, error)) return false;
	if (r.generator.empty() || !IsHexDigest(r.key) || !IsSafeFileName(r.model)) return Fail(error, "invalid generator, key or model name");
	const json* params = nullptr;
	const json* sceneGame = nullptr;
	if (!ReadObject(object, "params", params, error) || !ReadObject(object, "scene_game", sceneGame, error)) return false;
	r.params = *params;
	r.sceneGame = *sceneGame;

	const json* keys = nullptr;
	if (!ReadArray(object, "scene_game_keys", keys, error)) return false;
	for (const json& k : *keys) {
		if (!k.is_string()) return Fail(error, "scene_game_keys holds a non-string");
		r.sceneGameKeys.push_back(k.get<std::string>());
	}
	const json* inputs = nullptr;
	if (!ReadArray(object, "inputs", inputs, error)) return false;
	for (const json& input : *inputs) {
		InputHash hash;
		if (!ReadString(input, "path", hash.path, error) || !ReadString(input, "sha256", hash.sha256, error)) return false;
		if (!IsHexDigest(hash.sha256)) return Fail(error, "input hash is not a SHA-256 digest");
		r.inputs.push_back(std::move(hash));
	}
	const json* sides = nullptr;
	if (!ReadArray(object, "side_files", sides, error)) return false;
	for (const json& side : *sides) {
		SideFile file;
		if (!ReadString(side, "suffix", file.suffix, error) || !ReadString(side, "file", file.file, error)) return false;
		if (!IsSafeFileName(file.file)) return Fail(error, "unsafe side file name");
		r.sideFiles.push_back(std::move(file));
	}
	const json* children = nullptr;
	const json* probes = nullptr;
	const json* extras = nullptr;
	if (!ReadArray(object, "child_nodes", children, error) || !ReadArray(object, "reflection_probes", probes, error) ||
		!ReadBool(object, "has_render_extras", r.hasRenderExtras, error) || !ReadObject(object, "render_extras", extras, error)) return false;
	r.childNodes = *children;
	r.reflectionProbes = *probes;
	if (!RenderExtrasFromJson(*extras, r.renderExtras, error)) return false;

	if (ComputeKey(r.generator, r.generatorVersion, r.params, r.sceneGame, r.inputs) != r.key) {
		return Fail(error, "key does not match the stored identity (stale or edited result)");
	}
	out = std::move(r);
	return true;
}

bool FromGenerated(const NodeIdentity& identity, const ProceduralGeneratorResult& generated, Result& out, std::string* error)
{
	if (!generated.ok) return Fail(error, "generator reported failure");
	const std::string stemPath = identity.dir + "/" + kModelStem;
	if (generated.gltfPath != stemPath + ".gltf") {
		return Fail(error, "generator wrote '" + generated.gltfPath + "', expected '" + stemPath + ".gltf'");
	}
	Result r;
	r.generator = identity.generator;
	r.generatorVersion = identity.generatorVersion;
	r.key = identity.key;
	r.defaultOutput = identity.defaultOutput;
	r.params = identity.params;
	r.sceneGameKeys = identity.sceneGameKeys;
	r.sceneGame = identity.sceneGame;
	r.inputs = identity.inputs;
	r.model = std::string(kModelStem) + ".gltf";
	for (const std::string& aux : generated.auxiliaryPaths) {
		if (aux.rfind(stemPath, 0) != 0) return Fail(error, "generator side file '" + aux + "' is outside the cooked model stem");
		const std::string suffix = aux.substr(stemPath.size());
		if (suffix.empty() || suffix.find('/') != std::string::npos) return Fail(error, "unsupported side file name '" + aux + "'");
		r.sideFiles.push_back({ suffix, std::string(kModelStem) + suffix });
	}
	r.childNodes = generated.childNodes;
	r.reflectionProbes = generated.reflectionProbes;
	r.hasRenderExtras = generated.hasRenderExtras;
	r.renderExtras = generated.renderExtras;
	r.renderExtras.revision = 0;
	r.dir = identity.dir;
	out = std::move(r);
	return true;
}

ProceduralGeneratorResult ToGenerated(const Result& result)
{
	ProceduralGeneratorResult out;
	out.ok = true;
	out.gltfPath = result.dir + "/" + result.model;
	for (const SideFile& side : result.sideFiles) out.auxiliaryPaths.push_back(result.dir + "/" + side.file);
	out.childNodes = result.childNodes;
	out.reflectionProbes = result.reflectionProbes;
	out.hasRenderExtras = result.hasRenderExtras;
	out.renderExtras = result.renderExtras;
	return out;
}

Lookup LoadDir(const std::string& logicalDir, Result& out, std::string* error)
{
	const std::string path = logicalDir + "/" + kResultFile;
	if (!AssetIO::Exists(path)) return Lookup::Missing;
	std::string readError;
	const std::optional<std::string> text = AssetIO::ReadText(path, &readError);
	if (!text) {
		Fail(error, "cannot read " + path + ": " + readError);
		return Lookup::Invalid;
	}
	const json document = json::parse(*text, nullptr, false);
	if (document.is_discarded()) {
		Fail(error, "malformed JSON in " + path);
		return Lookup::Invalid;
	}
	Result r;
	if (!FromJson(document, r, error)) return Lookup::Invalid;
	if (logicalDir != std::string(kRootDir) + "/" + r.generator + "-" + r.key) {
		Fail(error, "cooked result is stored under the wrong directory: " + logicalDir);
		return Lookup::Invalid;
	}
	if (!AssetIO::Exists(logicalDir + "/" + r.model)) {
		Fail(error, "cooked model is missing: " + logicalDir + "/" + r.model);
		return Lookup::Invalid;
	}
	for (const SideFile& side : r.sideFiles) {
		if (!AssetIO::Exists(logicalDir + "/" + side.file)) {
			Fail(error, "cooked side file is missing: " + logicalDir + "/" + side.file);
			return Lookup::Invalid;
		}
	}
	r.dir = logicalDir;
	out = std::move(r);
	return Lookup::Found;
}

namespace {
	bool WriteBytes(const std::filesystem::path& path, const char* data, size_t size, std::string* error)
	{
		std::filesystem::path temp = path;
		temp += ".tmp";
		{
			std::ofstream file(temp, std::ios::binary | std::ios::trunc);
			if (!file) return Fail(error, "cannot write " + temp.string());
			file.write(data, static_cast<std::streamsize>(size));
			if (!file) return Fail(error, "write failed: " + temp.string());
		}
		std::error_code ec;
		std::filesystem::rename(temp, path, ec);
		if (ec) {
			std::filesystem::remove(path, ec);
			ec.clear();
			std::filesystem::rename(temp, path, ec);
			if (ec) return Fail(error, "cannot replace " + path.string() + ": " + ec.message());
		}
		return true;
	}
}

bool Write(const Result& result, std::string* error)
{
	if (result.dir.empty()) return Fail(error, "result has no directory");
	const std::optional<std::filesystem::path> physical = AssetIO::GeneratedOutputPath(result.dir + "/" + kResultFile);
	if (!physical) return Fail(error, "cooked results must live under generated/: " + result.dir);
	const std::string text = ToJson(result).dump(1) + "\n";
	return WriteBytes(*physical, text.data(), text.size(), error);
}

bool MaterializeSideFiles(const Result& result, const std::string& authoredOutput, std::string* error)
{
	for (const SideFile& side : result.sideFiles) {
		const std::string target = authoredOutput + side.suffix;
		const std::string source = result.dir + "/" + side.file;
		std::string readError;
		const AssetIO::Buffer bytes = AssetIO::ReadAll(source, &readError);
		if (!bytes) return Fail(error, "cannot read cooked side file " + source + ": " + readError);
		if (AssetIO::Exists(target)) {
			const AssetIO::Buffer current = AssetIO::ReadAll(target);
			if (current && *current == *bytes) continue;
		}
		const std::optional<std::filesystem::path> physical = AssetIO::GeneratedOutputPath(target);
		if (!physical) return Fail(error, "side file target must be under generated/: " + target);
		if (!WriteBytes(*physical, reinterpret_cast<const char*>(bytes->data()), bytes->size(), error)) return false;
	}
	return true;
}

bool FindUnregistered(const std::string& generator, const json& rawParams, const json& sceneGame, Result& out, std::string* error)
{
	const std::string wantedParams = ParamsHash(rawParams);
	AssetIO::ListOptions options;
	options.includeDirectories = true;
	const std::string prefix = generator + "-";
	std::string lastProblem;
	for (const std::string& entry : AssetIO::List(kRootDir, options)) {
		const std::string name = LastComponent(entry);
		if (name.rfind(prefix, 0) != 0) continue;
		Result candidate;
		std::string problem;
		const Lookup status = LoadDir(std::string(kRootDir) + "/" + name, candidate, &problem);
		if (status != Lookup::Found) {
			if (status == Lookup::Invalid) lastProblem = problem;
			continue;
		}
		if (candidate.generator != generator || ParamsHash(candidate.params) != wantedParams) continue;
		if (CanonicalJson(SceneGameSubset(sceneGame, candidate.sceneGameKeys)) != CanonicalJson(candidate.sceneGame)) {
			lastProblem = "scene game data differs from " + name;
			continue;
		}
		out = std::move(candidate);
		return true;
	}
	std::string message = "no cooked result for generator '" + generator + "' with params hash " + wantedParams +
		" (expected under " + kRootDir + "/" + prefix + "<key>); run nox_cook on this scene";
	if (!lastProblem.empty()) message += " (" + lastProblem + ")";
	return Fail(error, message);
}

} // namespace CookedResults
