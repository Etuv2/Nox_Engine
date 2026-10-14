#pragma once
// GUI element <-> scene JSON. The scene loader parses each entry of a GuiNode "elements" array with
// GuiElementFromJson and the saver writes the same schema with GuiElementToJson, so one definition
// of the keys and defaults covers both directions.

#include "GuiNode.h"
#include "json.hpp"

#include <string>

// Parses one "elements" entry. Returns false (kindOut still set) for an unknown kind.
// Fields missing from the JSON take the defaults the loader has always used per kind.
bool GuiElementFromJson(const nlohmann::json& el, GuiNode::GuiElement& out, std::string* kindOut = nullptr);

// Writes the scene-file form of an element. Layout keys (resizable, anchor, relative_*) are only
// written when they differ from the defaults, matching what the loader applies.
nlohmann::json GuiElementToJson(const GuiNode::GuiElement& element);
