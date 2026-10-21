#pragma once
// JSON readers over AssetIO. Same contract as the former "ifstream + json::parse(stream, nullptr, false)"
// pattern: a missing or unreadable file yields a null json value; a syntax error yields a discarded value.
#include "AssetIO.h"
#include "json.hpp"

#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AssetIO {
	namespace detail {
		inline nlohmann::json ReadJsonOrNullUncached(const std::string& path)
		{
			const std::optional<std::string> text = ReadText(path);
			return text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
		}

		struct JsonReadAhead {
			std::mutex mutex;
			std::unordered_map<std::string, std::future<nlohmann::json>> pending;
		};
		inline JsonReadAhead& ReadAheadStore()
		{
			static JsonReadAhead store;
			return store;
		}
	}

	// Read-ahead: parses the file on a worker thread now, so a later ReadJsonOrNull of the same path takes the
	// parsed value instead of reading and parsing on the caller's thread. Thread-safe. The result is the same as
	// an uncached read made at the time of the call.
	inline void PrefetchJson(const std::string& path)
	{
		detail::JsonReadAhead& store = detail::ReadAheadStore();
		std::lock_guard<std::mutex> lock(store.mutex);
		if (store.pending.count(path)) return;
		store.pending.emplace(path, std::async(std::launch::async, [path] { return detail::ReadJsonOrNullUncached(path); }));
	}

	inline nlohmann::json ReadJsonOrNull(std::string_view path)
	{
		detail::JsonReadAhead& store = detail::ReadAheadStore();
		std::future<nlohmann::json> ahead;
		{
			std::lock_guard<std::mutex> lock(store.mutex);
			auto node = store.pending.extract(std::string(path));
			if (!node.empty()) ahead = std::move(node.mapped());
		}
		if (ahead.valid()) return ahead.get();
		return detail::ReadJsonOrNullUncached(std::string(path));
	}
}
