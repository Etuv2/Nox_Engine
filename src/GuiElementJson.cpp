#include "GuiElementJson.h"

namespace {

using json = nlohmann::json;

// Reads an [r, g, b] or [r, g, b, a] array into color; alphaWhenMissing applies to the 3-entry form.
void ReadColor(const json& el, const char* key, SDL_Color& color, Uint8 alphaWhenMissing) {
	const auto it = el.find(key);
	if (it == el.end() || !it->is_array() || it->size() < 3) return;
	color.r = static_cast<Uint8>((*it)[0].get<int>());
	color.g = static_cast<Uint8>((*it)[1].get<int>());
	color.b = static_cast<Uint8>((*it)[2].get<int>());
	color.a = it->size() > 3 ? static_cast<Uint8>((*it)[3].get<int>()) : alphaWhenMissing;
}

json ColorToJson(const SDL_Color& c) {
	return json::array({ static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b), static_cast<int>(c.a) });
}

GuiNode::GuiElement::Anchor AnchorFromString(const std::string& s) {
	using Anchor = GuiNode::GuiElement::Anchor;
	if (s == "top_right") return Anchor::TOP_RIGHT;
	if (s == "bottom_left") return Anchor::BOTTOM_LEFT;
	if (s == "bottom_right") return Anchor::BOTTOM_RIGHT;
	if (s == "center") return Anchor::CENTER;
	return Anchor::TOP_LEFT;
}

std::string AnchorToString(GuiNode::GuiElement::Anchor anchor) {
	using Anchor = GuiNode::GuiElement::Anchor;
	switch (anchor) {
	case Anchor::TOP_RIGHT: return "top_right";
	case Anchor::BOTTOM_LEFT: return "bottom_left";
	case Anchor::BOTTOM_RIGHT: return "bottom_right";
	case Anchor::CENTER: return "center";
	default: return "top_left";
	}
}

void ReadLayout(const json& el, GuiNode::GuiElement& e) {
	if (el.contains("resizable")) {
		e.isResizable = el.value("resizable", false);
		e.minWidth = el.value("min_width", 10.0f);
		e.minHeight = el.value("min_height", 10.0f);
		e.maxWidth = el.value("max_width", -1.0f);
		e.maxHeight = el.value("max_height", -1.0f);
	}
	if (el.contains("anchor") && el["anchor"].is_string()) {
		e.anchor = AnchorFromString(el["anchor"].get<std::string>());
	}
	const auto relPos = el.find("relative_position");
	if (relPos != el.end() && relPos->is_array() && relPos->size() >= 2) {
		e.relativePosition = glm::vec2((*relPos)[0].get<float>(), (*relPos)[1].get<float>());
		e.useRelativePositioning = true;
	}
	const auto relSize = el.find("relative_size");
	if (relSize != el.end() && relSize->is_array() && relSize->size() >= 2) {
		e.relativeSize = glm::vec2((*relSize)[0].get<float>(), (*relSize)[1].get<float>());
		e.useRelativeSize = true;
	}
}

void ReadInteraction(const json& el, GuiNode::GuiElement& e) {
	e.visible = el.value("visible", true);
	e.interactive = el.value("interactive", false);
	e.id = el.value("id", std::string());
	e.onClick = el.value("on_click", std::string());
}

void WriteInteraction(json& out, const GuiNode::GuiElement& e) {
	if (!e.visible) out["visible"] = false;
	if (e.interactive) out["interactive"] = true;
	if (!e.id.empty()) out["id"] = e.id;
	if (!e.onClick.empty()) out["on_click"] = e.onClick;
}

void WriteLayout(json& out, const GuiNode::GuiElement& e) {
	if (e.isResizable) {
		out["resizable"] = true;
		out["min_width"] = e.minWidth;
		out["min_height"] = e.minHeight;
		out["max_width"] = e.maxWidth;
		out["max_height"] = e.maxHeight;
	}
	if (e.anchor != GuiNode::GuiElement::Anchor::TOP_LEFT) {
		out["anchor"] = AnchorToString(e.anchor);
	}
	if (e.useRelativePositioning) {
		out["relative_position"] = json::array({ e.relativePosition.x, e.relativePosition.y });
	}
	if (e.useRelativeSize) {
		out["relative_size"] = json::array({ e.relativeSize.x, e.relativeSize.y });
	}
}

} // namespace

bool GuiElementFromJson(const json& el, GuiNode::GuiElement& out, std::string* kindOut) {
	if (!el.is_object()) return false;
	const std::string kind = el.value("kind", "text");
	if (kindOut) *kindOut = kind;
	out = GuiNode::GuiElement();
	out.x = el.value("x", 0.0f);
	out.y = el.value("y", 0.0f);

	if (kind == "text") {
		out.type = GuiNode::GuiType::TEXT;
		out.content = el.value("text", "");
		out.color = { 255, 255, 255, 255 };
		ReadColor(el, "color", out.color, 255);
	}
	else if (kind == "image") {
		out.type = GuiNode::GuiType::IMAGE;
		out.content = el.value("path", "");
		out.width = el.value("w", 128.0f);
		out.height = el.value("h", 64.0f);
	}
	else if (kind == "solid_rect" || kind == "rect_solid") {   // rect_solid: spelling used by older scenes
		out.type = GuiNode::GuiType::RECT_SOLID;
		out.width = el.value("w", 100.0f);
		out.height = el.value("h", 100.0f);
		out.color = { 128, 128, 128, 255 };
		ReadColor(el, "color", out.color, 255);
	}
	else if (kind == "gradient_rect") {
		out.type = GuiNode::GuiType::RECT_GRADIENT;
		out.width = el.value("w", 100.0f);
		out.height = el.value("h", 100.0f);
		GuiNode::GradientStyle& g = out.gradient;
		const std::string gradientType = el.value("gradient_type", "vertical");
		if (gradientType == "horizontal") g.type = GuiNode::GradientType::LINEAR_HORIZONTAL;
		else if (gradientType == "radial") g.type = GuiNode::GradientType::RADIAL;
		else g.type = GuiNode::GradientType::LINEAR_VERTICAL;
		g.startColor = { 255, 255, 255, 255 };
		ReadColor(el, "start_color", g.startColor, 255);
		g.endColor = { 0, 0, 0, 255 };
		ReadColor(el, "end_color", g.endColor, 255);
		if (g.type == GuiNode::GradientType::RADIAL) {
			const auto center = el.find("center");
			if (center != el.end() && center->is_array() && center->size() >= 2) {
				g.center = glm::vec2((*center)[0].get<float>(), (*center)[1].get<float>());
			}
			g.radius = el.value("radius", 1.0f);
		}
	}
	else if (kind == "bevel_rect") {
		out.type = GuiNode::GuiType::RECT_BEVEL;
		out.width = el.value("w", 100.0f);
		out.height = el.value("h", 100.0f);
		out.color = { 128, 128, 128, 255 };
		ReadColor(el, "color", out.color, 255);
		out.bevel.radius = el.value("corner_radius", 0.0f);
		out.bevel.bevelSize = el.value("bevel_size", 2.0f);
		ReadColor(el, "highlight_color", out.bevel.highlightColor, 128);
		ReadColor(el, "shadow_color", out.bevel.shadowColor, 128);
	}
	else if (kind == "progress_bar") {
		out.type = GuiNode::GuiType::PROGRESS_BAR;
		out.width = el.value("w", 200.0f);
		out.height = el.value("h", 20.0f);
		GuiNode::ProgressBarStyle& s = out.progressStyle;
		s.backgroundColor = { 40, 40, 40, 255 };
		ReadColor(el, "background_color", s.backgroundColor, 255);
		s.fillColor = { 0, 170, 255, 255 };
		ReadColor(el, "fill_color", s.fillColor, 255);
		s.borderColor = { 255, 255, 255, 255 };
		ReadColor(el, "border_color", s.borderColor, 255);
		s.borderWidth = el.value("border_width", 2.0f);
		s.cornerRadius = el.value("corner_radius", 0.0f);
		s.showLabel = el.value("show_label", true);
		s.labelFontSize = el.value("label_font_size", 16);
		out.progressValue = el.value("progress_value", 0.0f);
		out.progressLabel = el.value("progress_label", "");
	}
	else {
		return false;
	}

	ReadLayout(el, out);
	ReadInteraction(el, out);
	return true;
}

json GuiElementToJson(const GuiNode::GuiElement& e) {
	json out = json::object();
	out["x"] = e.x;
	out["y"] = e.y;
	switch (e.type) {
	case GuiNode::GuiType::TEXT:
		out["kind"] = "text";
		out["text"] = e.content;
		out["color"] = ColorToJson(e.color);
		break;
	case GuiNode::GuiType::IMAGE:
		out["kind"] = "image";
		out["path"] = e.content;
		out["w"] = e.width;
		out["h"] = e.height;
		break;
	case GuiNode::GuiType::RECT_SOLID:
		out["kind"] = "solid_rect";
		out["w"] = e.width;
		out["h"] = e.height;
		out["color"] = ColorToJson(e.color);
		break;
	case GuiNode::GuiType::RECT_GRADIENT: {
		out["kind"] = "gradient_rect";
		out["w"] = e.width;
		out["h"] = e.height;
		const auto& g = e.gradient;
		out["gradient_type"] = g.type == GuiNode::GradientType::LINEAR_HORIZONTAL ? "horizontal"
			: g.type == GuiNode::GradientType::RADIAL ? "radial" : "vertical";
		out["start_color"] = ColorToJson(g.startColor);
		out["end_color"] = ColorToJson(g.endColor);
		if (g.type == GuiNode::GradientType::RADIAL) {
			out["center"] = json::array({ g.center.x, g.center.y });
			out["radius"] = g.radius;
		}
		break;
	}
	case GuiNode::GuiType::RECT_BEVEL:
		out["kind"] = "bevel_rect";
		out["w"] = e.width;
		out["h"] = e.height;
		out["color"] = ColorToJson(e.color);
		out["corner_radius"] = e.bevel.radius;
		out["bevel_size"] = e.bevel.bevelSize;
		out["highlight_color"] = ColorToJson(e.bevel.highlightColor);
		out["shadow_color"] = ColorToJson(e.bevel.shadowColor);
		break;
	case GuiNode::GuiType::PROGRESS_BAR: {
		out["kind"] = "progress_bar";
		out["w"] = e.width;
		out["h"] = e.height;
		const auto& s = e.progressStyle;
		out["background_color"] = ColorToJson(s.backgroundColor);
		out["fill_color"] = ColorToJson(s.fillColor);
		out["border_color"] = ColorToJson(s.borderColor);
		out["border_width"] = s.borderWidth;
		out["corner_radius"] = s.cornerRadius;
		out["show_label"] = s.showLabel;
		out["label_font_size"] = s.labelFontSize;
		out["progress_value"] = e.progressValue;
		out["progress_label"] = e.progressLabel;
		break;
	}
	}
	WriteLayout(out, e);
	WriteInteraction(out, e);
	// Relative layout is resolved against the screen every frame (GuiNode::RefreshElementPositions), so the derived
	// geometry is not part of the file: writing it would make a save depend on the window size.
	if (e.useRelativePositioning) {
		out.erase("x");
		out.erase("y");
	}
	if (e.useRelativeSize) {
		out.erase("w");
		out.erase("h");
	}
	return out;
}
