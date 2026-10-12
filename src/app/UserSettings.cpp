#include "app/UserSettings.h"
#include "assets/AssetIO.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
	bool WriteAtomic(const fs::path& path, std::string_view bytes)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		fs::path temp = path;
		temp += ".tmp";
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (!out) return false;
			out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!out) return false;
		}
		fs::rename(temp, path, ec);
		if (ec) {
			fs::remove(temp, ec);
			return false;
		}
		return true;
	}

	// RFC 7386: objects merge recursively, null removes a key, everything else replaces.
	nlohmann::json MergePatch(nlohmann::json target, const nlohmann::json& patch)
	{
		if (!patch.is_object()) return patch;
		if (!target.is_object()) target = nlohmann::json::object();
		for (auto it = patch.begin(); it != patch.end(); ++it) {
			if (it.value().is_null()) target.erase(it.key());
			else target[it.key()] = MergePatch(target.contains(it.key()) ? target[it.key()] : nlohmann::json(), it.value());
		}
		return target;
	}

	// Smallest patch p with MergePatch(from, p) == to.
	nlohmann::json DiffPatch(const nlohmann::json& from, const nlohmann::json& to)
	{
		if (!from.is_object() || !to.is_object()) return to;
		nlohmann::json patch = nlohmann::json::object();
		for (auto it = from.begin(); it != from.end(); ++it) {
			if (!to.contains(it.key())) patch[it.key()] = nullptr;
		}
		for (auto it = to.begin(); it != to.end(); ++it) {
			if (!from.contains(it.key())) patch[it.key()] = it.value();
			else if (from[it.key()] != it.value()) {
				if (from[it.key()].is_object() && it.value().is_object()) patch[it.key()] = DiffPatch(from[it.key()], it.value());
				else patch[it.key()] = it.value();
			}
		}
		return patch;
	}
}

namespace UserSettings {
	std::optional<fs::path> UserPath(std::string_view relativeName)
	{
		const fs::path rel{ std::string(relativeName) };
		if (relativeName.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return std::nullopt;
		for (const fs::path& part : rel) {
			if (part == ".." || part == ".") return std::nullopt;
		}
		const fs::path& root = AssetIO::UserDir();
		if (root.empty()) return std::nullopt;
		return root / rel;
	}

	std::optional<std::string> LoadUserText(std::string_view userFile)
	{
		const auto path = UserPath(userFile);
		if (!path) return std::nullopt;
		std::ifstream in(*path, std::ios::binary);
		if (!in) return std::nullopt;
		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	bool SaveUserText(std::string_view userFile, std::string_view text)
	{
		const auto path = UserPath(userFile);
		if (!path) return false;
		if (!WriteAtomic(*path, text)) {
			std::cerr << "[UserSettings] Could not write " << path->string() << std::endl;
			return false;
		}
		return true;
	}

	std::optional<nlohmann::json> LoadUserJson(std::string_view userFile)
	{
		const auto text = LoadUserText(userFile);
		if (!text) return std::nullopt;
		nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
		if (doc.is_discarded()) {
			std::cerr << "[UserSettings] Ignoring invalid JSON in user file " << userFile << std::endl;
			return std::nullopt;
		}
		return doc;
	}

	bool SaveUserJson(std::string_view userFile, const nlohmann::json& value)
	{
		return SaveUserText(userFile, value.dump(2) + "\n");
	}

	nlohmann::json LoadJsonWithUserOverride(std::string_view logicalDefault, std::string_view userFile)
	{
		nlohmann::json defaults;
		if (!logicalDefault.empty()) {
			std::string error;
			if (const auto text = AssetIO::ReadText(logicalDefault, &error)) {
				defaults = nlohmann::json::parse(*text, nullptr, false);
				if (defaults.is_discarded()) {
					std::cerr << "[UserSettings] Invalid JSON defaults: " << logicalDefault << std::endl;
					defaults = nlohmann::json();
				}
			}
		}
		if (const auto patch = LoadUserJson(userFile)) return MergePatch(defaults, *patch);
		return defaults;
	}

	bool SaveUserOverride(std::string_view logicalDefault, std::string_view userFile, const nlohmann::json& value)
	{
		nlohmann::json defaults;
		if (!logicalDefault.empty()) {
			if (const auto text = AssetIO::ReadText(logicalDefault)) {
				defaults = nlohmann::json::parse(*text, nullptr, false);
				if (defaults.is_discarded()) defaults = nlohmann::json();
			}
		}
		return SaveUserJson(userFile, DiffPatch(defaults, value));
	}
}
