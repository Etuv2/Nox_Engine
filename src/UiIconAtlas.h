#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <IMGUI/imgui.h>

class Texture;

/*
* UI icon atlas (game-agnostic): one packed RGBA atlas plus a JSON manifest of icon rectangles.
*
* Manifest format (textures/ui_icons/manifest.json): { "atlases": { "<size>": { "file": ... } },
* "assets": [ { "id": "group/name", "alias_of": "group/name"?, "atlas_rects": { "<size>":
* { "pixels": [l, t, r, b] } } } ] } with exclusive right/bottom pixel coordinates in top-left
* image space. The atlas is uploaded once (no mipmaps, clamp-to-edge, linear) without row
* flipping, so UVs are the recorded top-left coordinates.
*
* Icons are white with straight alpha: drawing multiplies RGB by the tint and alpha by the tint's
* alpha, which is exactly ImGui's vertex-colour modulation with its straight-alpha blend
* (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). Missing ids (or a missing atlas) return false so callers keep
* their previous vector/text fallback. Aliases resolve to their target's rectangle.
*/
class UiIconAtlas {
public:
	static UiIconAtlas& Get();

	// Loads (once; later calls with the same arguments are no-ops) the atlas of the given size. An
	// empty atlasPath uses the manifest's file for that size; otherwise the given PNG (same layout).
	bool Load(const std::string& manifestPath, int size = 128, const std::string& atlasPath = std::string());
	bool Loaded() const { return m_texture != nullptr; }
	bool Has(const std::string& id) const;
	// UVs of an icon (after alias resolution). False when unknown.
	bool Uv(const std::string& id, ImVec2& uv0, ImVec2& uv1) const;
	ImTextureID TextureId() const;

	// Draw list helpers (HUD / overlays). Return false when the icon is unavailable.
	bool Draw(ImDrawList* dl, const std::string& id, const ImVec2& min, const ImVec2& max, ImU32 tint) const;
	bool DrawCentered(ImDrawList* dl, const std::string& id, const ImVec2& center, float size, ImU32 tint) const;
	// Widget helpers (menus / editor). Image: sized icon in the layout. Button: icon button with
	// an optional text label to the right (label keeps the widget's dynamic text); returns pressed.
	bool Image(const std::string& id, float size, const ImVec4& tint = ImVec4(1, 1, 1, 1)) const;
	bool Button(const char* strId, const std::string& id, float size, const ImVec4& tint = ImVec4(1, 1, 1, 1),
		const char* label = nullptr) const;
	// Icon followed by text on the same line (falls back to the text alone).
	void IconText(const std::string& id, const char* text, float size, const ImVec4& tint = ImVec4(1, 1, 1, 1)) const;

private:
	struct Rect { float u0, v0, u1, v1; };
	const Rect* Find(const std::string& id) const;
	std::unique_ptr<Texture> m_texture;
	std::unordered_map<std::string, Rect> m_rects;
	std::unordered_map<std::string, std::string> m_aliases;
	std::string m_loadedKey;
	bool m_failed = false;
};
