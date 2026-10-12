#pragma once
// Immutable defaults from assets plus per-user overrides (docs/asset_io.md, "Writable data").
//
// Defaults are read through AssetIO (loose source tree or packs). The user's changes live only in
// AssetIO::UserDir(), stored as an RFC 7386 JSON merge patch or as a whole user-owned file. The user
// directory is never mounted as an asset source, so it cannot override shaders or content.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include "json.hpp"

namespace UserSettings {
	// Physical path of a user file (relative names only; nullopt for escaping/absolute names).
	std::optional<std::filesystem::path> UserPath(std::string_view relativeName);

	// Defaults from logicalDefault (may be empty) merged with the patch in <userDir>/<userFile>.
	// Missing or invalid inputs are skipped with a log line; the result is an object or null.
	nlohmann::json LoadJsonWithUserOverride(std::string_view logicalDefault, std::string_view userFile);

	// Writes the merge patch that turns the defaults into value (only changed keys are stored).
	bool SaveUserOverride(std::string_view logicalDefault, std::string_view userFile, const nlohmann::json& value);

	// Whole-file user data (save games, best times): read/written only in the user directory.
	std::optional<nlohmann::json> LoadUserJson(std::string_view userFile);
	bool SaveUserJson(std::string_view userFile, const nlohmann::json& value);

	// Text/binary user files (ImGui ini, layouts). Writes are atomic (temp file + rename).
	std::optional<std::string> LoadUserText(std::string_view userFile);
	bool SaveUserText(std::string_view userFile, std::string_view text);
}
