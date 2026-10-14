#include "LuaProject.h"

#include "../assets/AssetIO.h"

namespace NoxLua {

	bool LoadProjectDescriptor(const std::string& descriptorPath, const nlohmann::json& descriptor,
		ProjectDescriptor& out, std::string& error) {
		if (!descriptor.is_object()) {
			error = descriptorPath + ": descriptor must be a JSON object";
			return false;
		}
		if (descriptor.value("format", 0) != 1) {
			error = descriptorPath + ": unsupported descriptor format (expected 1)";
			return false;
		}
		if (descriptor.value("runtime", std::string()) != "lua") {
			error = descriptorPath + ": runtime must be \"lua\"";
			return false;
		}
		if (descriptor.value("api_version", 0) != 1) {
			error = descriptorPath + ": unsupported api_version (this engine provides 1)";
			return false;
		}
		const std::string entry = descriptor.value("entry", std::string());
		if (entry.empty()) {
			error = descriptorPath + ": missing \"entry\"";
			return false;
		}

		out = ProjectDescriptor();
		out.descriptorPath = descriptorPath;
		out.content.name = descriptor.value("name", std::string());
		out.content.descriptorPath = descriptorPath;
		out.content.projectDir = AssetIO::Parent(descriptorPath);
		out.content.descriptor = descriptor;

		std::string resolveError;
		const auto resolvedEntry = AssetIO::ResolveRelative(descriptorPath, entry, &resolveError);
		if (!resolvedEntry) {
			error = descriptorPath + ": entry '" + entry + "': " + resolveError;
			return false;
		}
		out.content.entry = *resolvedEntry;

		std::vector<std::string> roots;
		if (descriptor.contains("script_roots")) {
			if (!descriptor["script_roots"].is_array()) {
				error = descriptorPath + ": script_roots must be an array";
				return false;
			}
			for (const auto& root : descriptor["script_roots"]) {
				if (!root.is_string()) {
					error = descriptorPath + ": script_roots entries must be strings";
					return false;
				}
				roots.push_back(root.get<std::string>());
			}
		}
		if (roots.empty()) roots.push_back("scripts");
		for (const std::string& root : roots) {
			const auto resolved = AssetIO::ResolveRelative(descriptorPath, root, &resolveError);
			if (!resolved) {
				error = descriptorPath + ": script root '" + root + "': " + resolveError;
				return false;
			}
			out.content.scriptRoots.push_back(*resolved);
		}

		if (descriptor.contains("fonts")) {
			if (!descriptor["fonts"].is_array()) {
				error = descriptorPath + ": fonts must be an array";
				return false;
			}
			if (descriptor["fonts"].size() > 64) {
				error = descriptorPath + ": more than 64 fonts";
				return false;
			}
			for (const auto& entry : descriptor["fonts"]) {
				if (!entry.is_object()) {
					error = descriptorPath + ": each font needs {\"id\", \"path\", \"size\"}";
					return false;
				}
				UiFontDecl decl;
				decl.id = entry.value("id", std::string());
				const std::string path = entry.value("path", std::string());
				decl.size = entry.value("size", 16.0f);
				if (decl.id.empty() || path.empty()) {
					error = descriptorPath + ": font entries need a non-empty id and path";
					return false;
				}
				if (!(decl.size >= 4.0f && decl.size <= 256.0f)) {
					error = descriptorPath + ": font '" + decl.id + "' size must be 4..256";
					return false;
				}
				for (const UiFontDecl& other : out.fonts) {
					if (other.id == decl.id) {
						error = descriptorPath + ": duplicate font id '" + decl.id + "'";
						return false;
					}
				}
				const auto resolvedFont = AssetIO::ResolveRelative(descriptorPath, path, &resolveError);
				if (!resolvedFont) {
					error = descriptorPath + ": font '" + decl.id + "': " + resolveError;
					return false;
				}
				decl.path = *resolvedFont;
				out.fonts.push_back(decl);
			}
		}

		if (descriptor.contains("icons")) {
			const auto& icons = descriptor["icons"];
			if (!icons.is_object()) {
				error = descriptorPath + ": icons must be an object {\"manifest\", \"size\"}";
				return false;
			}
			const std::string manifest = icons.value("manifest", std::string());
			out.iconSize = icons.value("size", 128);
			if (manifest.empty()) {
				error = descriptorPath + ": icons.manifest is required";
				return false;
			}
			if (out.iconSize < 16 || out.iconSize > 512) {
				error = descriptorPath + ": icons.size must be 16..512";
				return false;
			}
			const auto resolvedManifest = AssetIO::ResolveRelative(descriptorPath, manifest, &resolveError);
			if (!resolvedManifest) {
				error = descriptorPath + ": icons.manifest: " + resolveError;
				return false;
			}
			out.iconManifest = *resolvedManifest;
		}

		// Action map: "input" names the file; the default is input.json beside the descriptor (optional).
		{
			const std::string inputRef = descriptor.contains("input") && descriptor["input"].is_string()
				? descriptor["input"].get<std::string>() : std::string("input.json");
			const auto resolvedInput = AssetIO::ResolveRelative(descriptorPath, inputRef, &resolveError);
			if (!resolvedInput) {
				error = descriptorPath + ": input: " + resolveError;
				return false;
			}
			out.inputPath = *resolvedInput;
		}

		// Extra audio buses beyond the engine's (master, music, sfx, ui, voice). Names are checked when declared.
		if (descriptor.contains("audio")) {
			const auto& audio = descriptor["audio"];
			if (!audio.is_object() || (audio.contains("buses") && !audio["buses"].is_array())) {
				error = descriptorPath + ": audio must be an object with an optional buses array of names";
				return false;
			}
			if (audio.contains("buses")) {
				for (const auto& entry : audio["buses"]) {
					if (!entry.is_string()) {
						error = descriptorPath + ": audio.buses entries must be strings";
						return false;
					}
					out.audioBuses.push_back(entry.get<std::string>());
				}
			}
		}

		// Declared data files that scripts may name (nox.roads, nox.collision). Only these can be loaded.
		for (const char* field : { "roads", "collision" }) {
			if (!descriptor.contains(field)) continue;
			if (!descriptor[field].is_array()) {
				error = descriptorPath + ": " + field + " must be an array of paths";
				return false;
			}
			std::vector<DeclaredFile>& target = std::string(field) == "roads" ? out.content.roads : out.content.collision;
			for (const auto& entry : descriptor[field]) {
				if (!entry.is_string()) {
					error = descriptorPath + ": " + field + " entries must be strings";
					return false;
				}
				const std::string name = entry.get<std::string>();
				const auto resolved = AssetIO::ResolveRelative(descriptorPath, name, &resolveError);
				if (!resolved) {
					error = descriptorPath + ": " + field + " '" + name + "': " + resolveError;
					return false;
				}
				target.push_back(DeclaredFile{ name, *resolved });
			}
		}

		// Authoring roots: repo-logical paths (not descriptor-relative), e.g. "config/hud.json" or "data". Only
		// nox.save_authoring_json may write under them, from editor contexts, in loose distributions.
		if (descriptor.contains("authoring_roots")) {
			if (!descriptor["authoring_roots"].is_array()) {
				error = descriptorPath + ": authoring_roots must be an array of logical paths";
				return false;
			}
			for (const auto& entry : descriptor["authoring_roots"]) {
				if (!entry.is_string()) {
					error = descriptorPath + ": authoring_roots entries must be strings";
					return false;
				}
				const std::string name = entry.get<std::string>();
				const auto normalized = AssetIO::Normalize(name, &resolveError);
				if (!normalized || normalized->empty()) {
					error = descriptorPath + ": authoring_roots '" + name + "': " + (normalized ? "empty path" : resolveError);
					return false;
				}
				if (*normalized == "generated" || normalized->rfind("generated/", 0) == 0) {
					error = descriptorPath + ": authoring_roots '" + name + "' names the generated cache, which is never authored";
					return false;
				}
				out.content.authoringRoots.push_back(*normalized);
			}
		}

		if (descriptor.contains("data_roots") && descriptor["data_roots"].is_array()) {
			for (const auto& root : descriptor["data_roots"]) {
				if (!root.is_string()) continue;
				const auto resolved = AssetIO::ResolveRelative(descriptorPath, root.get<std::string>(), &resolveError);
				if (resolved) out.dataRoots.push_back(*resolved);
			}
		}
		return true;
	}

	bool ReadProjectDescriptor(const std::string& descriptorPath, ProjectDescriptor& out, std::string& error) {
		const auto text = AssetIO::ReadText(descriptorPath, &error);
		if (!text) return false;
		const nlohmann::json descriptor = nlohmann::json::parse(*text, nullptr, false);
		if (descriptor.is_discarded()) {
			error = descriptorPath + ": invalid JSON";
			return false;
		}
		return LoadProjectDescriptor(descriptorPath, descriptor, out, error);
	}

	bool AssetScriptSource::ReadText(const std::string& logicalPath, std::string& text, std::string& error) {
		const auto contents = AssetIO::ReadText(logicalPath, &error);
		if (!contents) return false;
		text = *contents;
		return true;
	}
}
