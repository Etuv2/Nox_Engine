#pragma once
// Runtime outputs (performance data, benchmark captures, state snapshots, window layouts) are per-user
// files. Relative names resolve inside AssetIO::UserDir() through UserSettings; absolute paths are an
// explicit choice and are used as given.
#include "app/UserSettings.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace AssetIO {
	// Returns the physical path for a runtime output, or "" when the name is invalid or the user
	// directory is not configured. createParents creates the folders for a file about to be written.
	inline std::string UserOutputPath(const std::string& name, bool createParents = false)
	{
		const std::filesystem::path p(name);
		if (p.is_absolute()) return name;
		const auto target = UserSettings::UserPath(name);
		if (!target) return std::string();
		if (createParents && target->has_parent_path()) {
			std::error_code ec;
			std::filesystem::create_directories(target->parent_path(), ec);
		}
		return target->string();
	}
}
