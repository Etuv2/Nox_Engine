// GUI descriptor: the settings of a GUI node. Individual widgets are edited through the GUI scripting
// and the scene file, not through this descriptor.
#include "../ComponentBuilders.h"
#include "../../GuiNode.h"
#include "../../SceneNode.h"

namespace nox::reflect {

namespace {

GuiNode* GuiOf(SceneNode& n) { return dynamic_cast<GuiNode*>(&n); }
const GuiNode* GuiOf(const SceneNode& n) { return dynamic_cast<const GuiNode*>(&n); }

void RegisterGui() {
	ComponentDesc c;
	c.name = "Gui";
	c.category = "UI";
	c.tooltip = "Screen-space GUI container: font and element list of the node";
	c.order = 40;
	c.has = [](const SceneNode& n) { return GuiOf(n) != nullptr; };

	PropertyDesc fontPath = Prop("font_path", PropType::AssetPath,
		[](const SceneNode& n) {
			const GuiNode* g = GuiOf(n);
			return PropValue(g ? g->GetFontPath() : std::string());
		},
		[](SceneNode& n, const PropValue& v) {
			GuiNode* g = GuiOf(n);
			const std::string& path = std::get<std::string>(v);
			if (!g || path.empty()) return false;
			g->LoadFont(path, g->GetFontSize());
			return true;
		});
	fontPath.assetFilter = { ".ttf", ".otf" };

	c.properties = {
		fontPath,
		Ranged(Prop("font_size", PropType::Int,
			[](const SceneNode& n) { const GuiNode* g = GuiOf(n); return PropValue(static_cast<int64_t>(g ? g->GetFontSize() : 0)); },
			[](SceneNode& n, const PropValue& v) {
				GuiNode* g = GuiOf(n);
				if (!g) return false;
				g->LoadFont(g->GetFontPath(), static_cast<int>(std::get<int64_t>(v)));
				return true;
			}), 1.0, 512.0, 1.0),
		RuntimeProp("element_count", PropType::Int,
			[](const SceneNode& n) { const GuiNode* g = GuiOf(n); return PropValue(static_cast<int64_t>(g ? g->GetElementCount() : 0)); }),
	};

	c.methods.push_back(VoidMethod("refresh_layout", "Recalculate element positions and sizes from the current screen size",
		[](SceneNode& n) {
			GuiNode* g = GuiOf(n);
			if (!g) return false;
			g->RefreshElementPositions();
			return true;
		}));

	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterGuiComponents() {
	RegisterGui();
}

} // namespace nox::reflect
