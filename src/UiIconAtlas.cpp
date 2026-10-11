#include "UiIconAtlas.h"

#include "Texture.h"
#include "json.hpp"
#include "rhi/RHI.h"
#include "stb_image.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

using nlohmann::json;

UiIconAtlas& UiIconAtlas::Get()
{
	static UiIconAtlas atlas;
	return atlas;
}

bool UiIconAtlas::Load(const std::string& manifestPath, int size, const std::string& atlasPath)
{
	const std::string key = manifestPath + "#" + std::to_string(size) + "#" + atlasPath;
	if (key == m_loadedKey && (m_texture || m_failed)) return m_texture != nullptr;
	m_loadedKey = key;
	m_failed = true;
	m_texture.reset();
	m_rects.clear();
	m_aliases.clear();

	std::ifstream in(manifestPath);
	const json manifest = in ? json::parse(in, nullptr, false) : json();
	if (!manifest.is_object()) {
		std::cerr << "[UiIconAtlas] Missing manifest " << manifestPath << " (icons fall back to text/vector)" << std::endl;
		return false;
	}
	const std::string sizeKey = std::to_string(size);
	const json atlases = manifest.value("atlases", json::object());
	if (!atlases.contains(sizeKey)) {
		std::cerr << "[UiIconAtlas] No " << size << " px atlas in " << manifestPath << std::endl;
		return false;
	}
	const std::string file = atlasPath.empty() ? atlases[sizeKey].value("file", std::string()) : atlasPath;

	// Straight-alpha RGBA, top-left origin (no row flip, so the recorded UVs apply directly).
	int w = 0, h = 0, n = 0;
	stbi_set_flip_vertically_on_load_thread(0);
	unsigned char* pixels = stbi_load(file.c_str(), &w, &h, &n, 4);
	if (!pixels) {
		std::cerr << "[UiIconAtlas] Cannot load atlas " << file << std::endl;
		return false;
	}
	m_texture = std::make_unique<Texture>(w, h, rhi::Format::RGBA8, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, pixels, false);
	stbi_image_free(pixels);
	m_texture->SetParameters(rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Filter::Linear, rhi::Filter::Linear);

	for (const json& a : manifest.value("assets", json::array())) {
		const std::string id = a.value("id", std::string());
		if (id.empty()) continue;
		if (a.contains("alias_of") && a["alias_of"].is_string()) m_aliases[id] = a["alias_of"].get<std::string>();
		const json rects = a.value("atlas_rects", json::object());
		if (!rects.contains(sizeKey)) continue;
		const json px = rects[sizeKey].value("pixels", json::array());
		if (!px.is_array() || px.size() != 4) continue;
		Rect r;
		r.u0 = px[0].get<float>() / static_cast<float>(w);
		r.v0 = px[1].get<float>() / static_cast<float>(h);
		r.u1 = px[2].get<float>() / static_cast<float>(w);
		r.v1 = px[3].get<float>() / static_cast<float>(h);
		m_rects[id] = r;
	}
	m_failed = false;
	std::cout << "[UiIconAtlas] " << file << ": " << w << "x" << h << ", " << m_rects.size() << " icons, "
		<< m_aliases.size() << " aliases" << std::endl;
	return true;
}

const UiIconAtlas::Rect* UiIconAtlas::Find(const std::string& id) const
{
	if (!m_texture) return nullptr;
	auto it = m_rects.find(id);
	if (it != m_rects.end()) return &it->second;
	auto al = m_aliases.find(id);
	if (al != m_aliases.end()) {
		auto t = m_rects.find(al->second);
		if (t != m_rects.end()) return &t->second;
	}
	return nullptr;
}

bool UiIconAtlas::Has(const std::string& id) const { return Find(id) != nullptr; }

bool UiIconAtlas::Uv(const std::string& id, ImVec2& uv0, ImVec2& uv1) const
{
	const Rect* r = Find(id);
	if (!r) return false;
	uv0 = ImVec2(r->u0, r->v0);
	uv1 = ImVec2(r->u1, r->v1);
	return true;
}

ImTextureID UiIconAtlas::TextureId() const
{
	return m_texture ? (ImTextureID)rhi::Gpu().ImGuiTextureId(m_texture->ID()) : ImTextureID();
}

bool UiIconAtlas::Draw(ImDrawList* dl, const std::string& id, const ImVec2& min, const ImVec2& max, ImU32 tint) const
{
	const Rect* r = Find(id);
	if (!r || !dl) return false;
	dl->AddImage(TextureId(), min, max, ImVec2(r->u0, r->v0), ImVec2(r->u1, r->v1), tint);
	return true;
}

bool UiIconAtlas::DrawCentered(ImDrawList* dl, const std::string& id, const ImVec2& c, float size, ImU32 tint) const
{
	const float h = size * 0.5f;
	return Draw(dl, id, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), tint);
}

bool UiIconAtlas::Image(const std::string& id, float size, const ImVec4& tint) const
{
	const Rect* r = Find(id);
	if (!r) return false;
	ImGui::Image(TextureId(), ImVec2(size, size), ImVec2(r->u0, r->v0), ImVec2(r->u1, r->v1), tint, ImVec4(0, 0, 0, 0));
	return true;
}

bool UiIconAtlas::Button(const char* strId, const std::string& id, float size, const ImVec4& tint, const char* label) const
{
	const Rect* r = Find(id);
	if (!r) return label ? ImGui::Button(label) : ImGui::Button(strId);
	bool pressed = false;
	if (label && *label) {
		// Icon + live text in one button.
		ImGui::PushID(strId);
		const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
		const ImGuiStyle& st = ImGui::GetStyle();
		const ImVec2 full(size + st.ItemInnerSpacing.x + text.x + st.FramePadding.x * 2.0f,
			std::max(size, text.y) + st.FramePadding.y * 2.0f);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		pressed = ImGui::Button("##iconbtn", full);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float y0 = p.y + (full.y - size) * 0.5f;
		dl->AddImage(TextureId(), ImVec2(p.x + st.FramePadding.x, y0), ImVec2(p.x + st.FramePadding.x + size, y0 + size),
			ImVec2(r->u0, r->v0), ImVec2(r->u1, r->v1), ImGui::GetColorU32(tint));
		const char* end = std::strstr(label, "##");
		dl->AddText(ImVec2(p.x + st.FramePadding.x + size + st.ItemInnerSpacing.x, p.y + (full.y - text.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Text), label, end);
		ImGui::PopID();
	}
	else {
		pressed = ImGui::ImageButton(strId, TextureId(), ImVec2(size, size), ImVec2(r->u0, r->v0), ImVec2(r->u1, r->v1),
			ImVec4(0, 0, 0, 0), tint);
	}
	return pressed;
}

void UiIconAtlas::IconText(const std::string& id, const char* text, float size, const ImVec4& tint) const
{
	if (Image(id, size, tint)) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextUnformatted(text);
}
