#include "Clipboard.h"

#include <algorithm>
#include <cctype>

namespace nox::editor {

using nlohmann::json;

json MakeClipboardDocument(const std::vector<json>& nodes)
{
	json doc = json::object();
	doc["format"] = kClipboardFormat;
	doc["version"] = kClipboardVersion;
	doc["nodes"] = json::array();
	for (const json& n : nodes) doc["nodes"].push_back(n);
	return doc;
}

bool ParseClipboardDocument(const std::string& text, std::vector<json>& nodes)
{
	nodes.clear();
	if (text.empty() || text.front() != '{') return false;   // cheap reject before parsing
	const json doc = json::parse(text, nullptr, false);
	if (!doc.is_object() || doc.value("format", std::string()) != kClipboardFormat) return false;
	if (doc.value("version", 0) != kClipboardVersion) return false;
	const auto it = doc.find("nodes");
	if (it == doc.end() || !it->is_array() || it->empty()) return false;
	for (const json& n : *it) {
		if (!n.is_object()) {
			nodes.clear();
			return false;
		}
		nodes.push_back(n);
	}
	return true;
}

std::string UniqueSiblingName(const std::string& name, const std::vector<std::string>& siblingNames)
{
	auto taken = [&](const std::string& candidate) {
		return std::find(siblingNames.begin(), siblingNames.end(), candidate) != siblingNames.end();
	};
	if (!taken(name)) return name;

	// Strip a trailing " (digits)" so that copies of copies count up rather than nest.
	std::string stem = name;
	if (!stem.empty() && stem.back() == ')') {
		const size_t open = stem.rfind(" (");
		if (open != std::string::npos && open + 2 < stem.size() - 1) {
			const bool digits = std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(open + 2), stem.end() - 1,
				[](unsigned char c) { return std::isdigit(c) != 0; });
			if (digits) stem = stem.substr(0, open);
		}
	}
	for (int n = 1;; ++n) {
		const std::string candidate = stem + " (" + std::to_string(n) + ")";
		if (!taken(candidate)) return candidate;
	}
}

} // namespace nox::editor
