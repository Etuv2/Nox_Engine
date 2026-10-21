#pragma once
// Editor clipboard documents: serialised node subtrees (SceneLoader node JSON, prefab instances as references)
// wrapped with a format marker, so the same text can sit on the OS clipboard and paste into another editor
// session. Headless: the workspace does the Core work (serialise, instantiate) and the clipboard I/O.

#include <string>
#include <vector>
#include "json.hpp"

namespace nox::editor {

inline constexpr const char* kClipboardFormat = "nox.scene_nodes";
inline constexpr int kClipboardVersion = 1;

// {"format": "nox.scene_nodes", "version": 1, "nodes": [...]}
nlohmann::json MakeClipboardDocument(const std::vector<nlohmann::json>& nodes);

// Reads a clipboard text. False when the text is not one of our documents (other text, other formats).
bool ParseClipboardDocument(const std::string& text, std::vector<nlohmann::json>& nodes);

// Name for a pasted copy: the name itself when no sibling has it, else "Name (n)" with the smallest n >= 1
// that is free. A trailing " (n)" on name is kept as the stem, so pasting "Crate (1)" gives "Crate (2)".
std::string UniqueSiblingName(const std::string& name, const std::vector<std::string>& siblingNames);

} // namespace nox::editor
