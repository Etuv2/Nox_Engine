#pragma once
// JSON readers over AssetIO. Same contract as the former "ifstream + json::parse(stream, nullptr, false)"
// pattern: a missing or unreadable file yields a null json value; a syntax error yields a discarded value.
#include "AssetIO.h"
#include "json.hpp"

#include <string_view>

namespace AssetIO {
	inline nlohmann::json ReadJsonOrNull(std::string_view path)
	{
		const std::optional<std::string> text = ReadText(path);
		return text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
	}
}
