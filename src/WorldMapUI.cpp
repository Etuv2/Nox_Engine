#include "WorldMapUI.h"

#include "RoadNetwork.h"
#include "Texture.h"
#include "UiIconAtlas.h"
#include "rhi/RHI.h"
#include "stb_image.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using nlohmann::json;

namespace {
	// Detail levels: Douglas-Peucker tolerance in metres. A level is used when its tolerance is
	// below ~0.7 px on screen, so the coarsest acceptable line is picked at every zoom.
	constexpr float kTolerance[4] = { 1.0f, 4.0f, 16.0f, 64.0f };
	constexpr float kDedupCell = 4.0f;       // metres: duplicate carriageways share midpoints
	constexpr float kRefHeight = 1080.0f;    // layout reference height (px)
	constexpr float kDefaultVisibleM = 12000.0f;   // metres visible vertically at the start zoom
	constexpr float kMinVisibleM = 500.0f;        // closest zoom
	constexpr const char* kFontTitle = "BarlowCondensed-SemiBold";
	constexpr const char* kFontBody = "BarlowCondensed-Medium";

	ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

	// Douglas-Peucker (iterative). Keeps both end points.
	void Simplify(const std::vector<glm::vec2>& in, float tol, std::vector<glm::vec2>& out) {
		out.clear();
		if (in.size() <= 2) { out = in; return; }
		std::vector<char> keep(in.size(), 0);
		keep.front() = keep.back() = 1;
		std::vector<std::pair<size_t, size_t>> stack{ { 0, in.size() - 1 } };
		while (!stack.empty()) {
			const auto [a, b] = stack.back();
			stack.pop_back();
			if (b <= a + 1) continue;
			const glm::vec2 A = in[a], AB = in[b] - in[a];
			const float L2 = glm::dot(AB, AB);
			float maxD = 0.0f;
			size_t idx = a;
			for (size_t i = a + 1; i < b; ++i) {
				const glm::vec2 P = in[i] - A;
				float d;
				if (L2 < 1e-9f) d = glm::length(P);
				else {
					const float t = std::clamp(glm::dot(P, AB) / L2, 0.0f, 1.0f);
					d = glm::length(P - AB * t);
				}
				if (d > maxD) { maxD = d; idx = i; }
			}
			if (maxD > tol) {
				keep[idx] = 1;
				stack.push_back({ a, idx });
				stack.push_back({ idx, b });
			}
		}
		for (size_t i = 0; i < in.size(); ++i) if (keep[i]) out.push_back(in[i]);
	}

	float PolyLength(const std::vector<glm::vec2>& p) {
		float L = 0.0f;
		for (size_t i = 1; i < p.size(); ++i) L += glm::length(p[i] - p[i - 1]);
		return L;
	}

	glm::vec2 PointAtArc(const std::vector<glm::vec2>& p, float s) {
		for (size_t i = 1; i < p.size(); ++i) {
			const float seg = glm::length(p[i] - p[i - 1]);
			if (s <= seg && seg > 1e-4f) return p[i - 1] + (p[i] - p[i - 1]) * (s / seg);
			s -= seg;
		}
		return p.empty() ? glm::vec2(0.0f) : p.back();
	}

	int64_t CellKey(const glm::vec2& p) {
		const int64_t x = static_cast<int64_t>(std::floor(p.x / kDedupCell));
		const int64_t z = static_cast<int64_t>(std::floor(p.y / kDedupCell));
		return x * 1000003LL + z;
	}

	// Route id -> the label drivers read on signs: "C1", "11", "1" ("1-Haneda" -> "1").
	std::string ShortRouteName(const std::string& route) {
		if (route.empty() || route == "UNASSIGNED" || route == "PA") return {};
		const size_t dash = route.find('-');
		return dash == std::string::npos ? route : route.substr(0, dash);
	}

	// "GINZA" -> "Ginza" (sub-line of the event panel).
	std::string TitleCase(const std::string& s) {
		std::string out = s;
		for (size_t i = 0; i < out.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(out[i]);
			if (i == 0) out[i] = static_cast<char>(std::toupper(c));
			else out[i] = static_cast<char>(std::tolower(c));
		}
		return out;
	}
}

// Start zoom: about kDefaultVisibleM metres high, or the whole map when it is smaller than that
// (small district networks), never closer than kMinVisibleM.
float WorldMapUI::DefaultPpm(const ImVec2& size) const
{
	const float byHeight = size.y / kDefaultVisibleM;
	const float fit = std::min(size.x / std::max(1.0f, m_mapMax.x - m_mapMin.x),
		size.y / std::max(1.0f, m_mapMax.y - m_mapMin.y)) * 0.95f;
	const float maxPpm = size.y / kMinVisibleM;
	return std::clamp(std::min(byHeight, fit), size.y / (kDefaultVisibleM * 2.0f), maxPpm);
}

WorldMapUI::WorldMapUI() = default;
WorldMapUI::~WorldMapUI() = default;

void WorldMapUI::Configure(const json& cfg, const RoadNetwork& roads)
{
	m_configured = true;
	m_base.reset();
	m_hasMapBounds = false;
	m_attribution.clear();
	m_chains.clear();
	m_routeNames.clear();
	m_labels.clear();
	m_cacheValid = false;
	m_viewInit = false;

	if (cfg.is_object()) {
		m_attribution = cfg.value("attribution", std::string());
		const std::string image = cfg.value("image", std::string());
		if (!image.empty()) {
			int w = 0, h = 0, n = 0;
			stbi_set_flip_vertically_on_load_thread(0);
			unsigned char* px = stbi_load(image.c_str(), &w, &h, &n, 4);
			if (px) {
				m_base = std::make_unique<Texture>(w, h, rhi::Format::RGBA8, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, px, false);
				stbi_image_free(px);
				m_base->SetParameters(rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Filter::Linear, rhi::Filter::Linear);
			}
			else {
				std::cerr << "[WorldMap] Cannot load base image " << image << " (plain grid instead)" << std::endl;
			}
		}
		if (cfg.contains("bounds") && cfg["bounds"].is_array() && cfg["bounds"].size() == 4) {
			const json& b = cfg["bounds"];
			m_mapMin = glm::vec2(b[0].get<float>(), b[1].get<float>());
			m_mapMax = glm::vec2(b[2].get<float>(), b[3].get<float>());
			m_hasMapBounds = true;
		}
	}
	if (!m_hasMapBounds) {
		glm::vec2 mn(0.0f), mx(1.0f);
		roads.Bounds(mn, mx);
		m_mapMin = mn;
		m_mapMax = mx;
		m_hasMapBounds = true;
	}

	// Icons are optional: the atlas loads once and the map falls back to vector shapes without it.
	UiIconAtlas::Get().Load("textures/ui_icons/manifest.json", 128);

	BuildChains(roads, cfg.is_object() ? cfg.value("network", std::string()) : std::string());
	BuildLabels(cfg.is_object() ? cfg.value("labels", std::string()) : std::string());
}

void WorldMapUI::BuildChains(const RoadNetwork& roads, const std::string& networkPath)
{
	// Carriageway metadata (route id, kind) from the authoritative network file, keyed by the
	// carriageway ids that RoadNetwork edges carry.
	struct Meta { int route; uint8_t kind; };
	std::unordered_map<std::string, Meta> meta;
	std::unordered_map<std::string, int> routeIndex;
	if (!networkPath.empty()) {
		std::ifstream in(networkPath);
		const json net = in ? json::parse(in, nullptr, false) : json();
		if (net.is_object() && net.contains("carriageways") && net["carriageways"].is_array()) {
			for (const json& cw : net["carriageways"]) {
				const std::string id = cw.value("id", std::string());
				const std::string route = cw.value("route", std::string());
				const std::string kind = cw.value("kind", std::string("main"));
				if (id.empty()) continue;
				auto it = routeIndex.find(route);
				int ri;
				if (it == routeIndex.end()) {
					ri = static_cast<int>(m_routeNames.size());
					m_routeNames.push_back(route);
					routeIndex[route] = ri;
				}
				else ri = it->second;
				const uint8_t k = kind == "main" ? 0 : (kind == "pa" || kind == "toll") ? 2 : 1;
				meta[id] = { ri, k };
			}
		}
	}

	// Edges grouped by carriageway id (file order); edges without an id form the generic street group.
	const std::vector<RoadNetwork::Node>& nodes = roads.Nodes();
	const std::vector<RoadNetwork::Edge>& edges = roads.Edges();
	std::unordered_map<std::string, std::vector<int>> groups;
	for (int i = 0; i < static_cast<int>(edges.size()); ++i) groups[edges[i].carriageway].push_back(i);

	// Main carriageways first so the dedupe keeps them over the opposite direction or ramps.
	std::vector<std::pair<int, std::string>> order;
	for (const auto& g : groups) {
		const auto it = meta.find(g.first);
		const int rank = g.first.empty() ? 3 : (it != meta.end() ? it->second.kind : 1);
		order.push_back({ rank, g.first });
	}
	std::sort(order.begin(), order.end());

	std::unordered_set<int64_t> drawn;
	std::vector<glm::vec2> tmp;
	for (const auto& [rank, key] : order) {
		const auto mit = meta.find(key);
		const int route = mit != meta.end() ? mit->second.route : -1;
		const uint8_t kind = mit != meta.end() ? mit->second.kind : 2;
		const std::vector<int>& idxs = groups[key];

		// Split the group into connected runs (a -> b, b -> c, ...).
		std::vector<std::vector<glm::vec2>> runs;
		std::vector<glm::vec2> cur;
		int last = -1;
		for (int ei : idxs) {
			const RoadNetwork::Edge& e = edges[ei];
			if (e.a != last) {
				if (cur.size() >= 2) runs.push_back(cur);
				cur.clear();
				cur.push_back(nodes[e.a].pos);
			}
			cur.push_back(nodes[e.b].pos);
			last = e.b;
		}
		if (cur.size() >= 2) runs.push_back(cur);

		for (const std::vector<glm::vec2>& run : runs) {
			// Drop runs that repeat an already drawn carriageway (the opposite direction of a pair).
			size_t segs = 0, hits = 0;
			for (size_t i = 1; i < run.size(); ++i) {
				++segs;
				if (drawn.count(CellKey((run[i - 1] + run[i]) * 0.5f))) ++hits;
			}
			if (segs == 0 || hits * 10 >= segs * 6) continue;
			for (size_t i = 1; i < run.size(); ++i) drawn.insert(CellKey((run[i - 1] + run[i]) * 0.5f));

			ChainInfo ch;
			ch.route = route;
			ch.kind = kind;
			ch.mn = glm::vec2(FLT_MAX);
			ch.mx = glm::vec2(-FLT_MAX);
			for (const glm::vec2& p : run) { ch.mn = glm::min(ch.mn, p); ch.mx = glm::max(ch.mx, p); }
			for (int l = 0; l < 4; ++l) {
				Simplify(run, kTolerance[l], tmp);
				ch.lv[l] = tmp;
			}
			m_chains.push_back(std::move(ch));
		}
	}

	// One route label per route: midpoint of its longest main carriageway.
	std::vector<float> bestLen(m_routeNames.size(), 0.0f);
	std::vector<int> bestChain(m_routeNames.size(), -1);
	for (int i = 0; i < static_cast<int>(m_chains.size()); ++i) {
		const ChainInfo& c = m_chains[i];
		if (c.route < 0 || c.kind != 0) continue;
		const float L = PolyLength(c.lv[1]);
		if (L > bestLen[c.route]) { bestLen[c.route] = L; bestChain[c.route] = i; }
	}
	for (size_t r = 0; r < m_routeNames.size(); ++r) {
		if (bestChain[r] < 0) continue;
		const std::string text = ShortRouteName(m_routeNames[r]);
		if (text.empty()) continue;
		MapLabel lab;
		lab.text = text;
		lab.kind = 0;
		lab.pos = PointAtArc(m_chains[bestChain[r]].lv[1], bestLen[r] * 0.5f);
		m_labels.push_back(lab);
	}
	std::cout << "[WorldMap] " << m_chains.size() << " carriageway runs, " << m_routeNames.size()
		<< " routes, " << edges.size() << " edges" << std::endl;
}

void WorldMapUI::BuildLabels(const std::string& labelsPath)
{
	if (labelsPath.empty()) return;
	std::ifstream in(labelsPath);
	const json j = in ? json::parse(in, nullptr, false) : json();
	if (!j.is_object() || !j.contains("labels") || !j["labels"].is_array()) return;
	for (const json& l : j["labels"]) {
		if (!l.is_object() || !l.contains("pos") || !l["pos"].is_array() || l["pos"].size() < 2) continue;
		MapLabel lab;
		lab.text = l.value("text", std::string());
		const std::string kind = l.value("kind", std::string("district"));
		lab.kind = kind == "water" ? 2 : 1;
		lab.pos = glm::vec2(l["pos"][0].get<float>(), l["pos"][1].get<float>());
		if (!lab.text.empty()) m_labels.push_back(lab);
	}
}

void WorldMapUI::Open(const WorldMapContext& ctx)
{
	m_open = true;
	m_selected = std::clamp(ctx.selectedEvent, 0, std::max(0, static_cast<int>(ctx.events.size()) - 1));
	m_hovered = -1;
	m_pressed = false;
	m_dragging = false;
	m_viewInit = false;
	m_centre = ctx.player;
	m_cacheValid = false;
}

glm::vec2 WorldMapUI::WorldToScreen(const glm::vec2& w, const ImVec2& origin, const ImVec2& size) const
{
	return glm::vec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f) + (w - m_centre) * m_ppm;
}

glm::vec2 WorldMapUI::ScreenToWorld(const glm::vec2& s, const ImVec2& origin, const ImVec2& size) const
{
	return m_centre + (s - glm::vec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f)) / m_ppm;
}

int WorldMapUI::LevelForZoom() const
{
	for (int l = 3; l >= 0; --l) {
		if (kTolerance[l] * m_ppm <= 0.7f) return l;
	}
	return 0;
}

int WorldMapUI::NearestEvent(const WorldMapContext& ctx, const glm::vec2& mouse, float radius) const
{
	int best = -1;
	float bestD = radius * radius;
	for (int i = 0; i < static_cast<int>(m_markerScreen.size()) && i < static_cast<int>(ctx.events.size()); ++i) {
		const glm::vec2 d = glm::vec2(m_markerScreen[i].x, m_markerScreen[i].y) - mouse;
		const float d2 = glm::dot(d, d);
		if (d2 <= bestD) { bestD = d2; best = i; }
	}
	return best;
}

std::string WorldMapUI::DistrictNear(const glm::vec2& p, float maxDist) const
{
	const MapLabel* best = nullptr;
	float bestD = maxDist * maxDist;
	for (const MapLabel& l : m_labels) {
		if (l.kind != 1) continue;
		const glm::vec2 d = l.pos - p;
		const float d2 = glm::dot(d, d);
		if (d2 <= bestD) { bestD = d2; best = &l; }
	}
	return best ? TitleCase(best->text) : std::string();
}

ImFont* WorldMapUI::Font(const char* family, float px) const
{
	ImGuiIO& io = ImGui::GetIO();
	ImFont* best = nullptr;
	float bestD = FLT_MAX;
	if (io.Fonts) {
		for (ImFont* f : io.Fonts->Fonts) {
			if (!f || !f->Sources) continue;
			if (std::string(f->GetDebugName()).find(family) == std::string::npos) continue;
			const float d = std::fabs(f->FontSize - px);
			if (d < bestD) { bestD = d; best = f; }
		}
	}
	return best ? best : ImGui::GetFont();
}

WorldMapAction WorldMapUI::Update(const WorldMapInput& in, const WorldMapContext& ctx, float dt)
{
	WorldMapAction act;
	m_time += dt;
	if (!m_open) return act;
	if (m_size.x < 2.0f || m_size.y < 2.0f) {
		// Not drawn yet (first frame): only handle close.
		if (in.close) { m_open = false; act.close = true; }
		return act;
	}

	const float minPpm = m_size.y / (kDefaultVisibleM * 2.0f);
	const float fitPpm = std::min(m_size.x / std::max(1.0f, (m_mapMax.x - m_mapMin.x)) * 0.95f,
		m_size.y / std::max(1.0f, (m_mapMax.y - m_mapMin.y)) * 0.95f);
	const float maxPpm = m_size.y / kMinVisibleM;
	if (!m_viewInit) {
		m_viewInit = true;
		m_ppm = DefaultPpm(m_size);
		m_centre = ctx.player;
	}

	// Pan (keys / stick): a fixed fraction of the visible height per second.
	if (glm::length(in.pan) > 0.0f) {
		glm::vec2 p = in.pan;
		if (glm::length(p) > 1.0f) p = glm::normalize(p);
		m_centre += p * (0.9f * m_size.y / m_ppm) * dt;
	}

	// Zoom about the cursor when it is over the map, else about the centre.
	const float steps = in.zoomSteps + in.zoomRate * dt * 2.5f;
	if (std::fabs(steps) > 1e-6f) {
		const glm::vec2 mouse = in.mouse;
		const glm::vec2 centre(m_origin.x + m_size.x * 0.5f, m_origin.y + m_size.y * 0.5f);
		const bool inside = mouse.x >= m_origin.x && mouse.x <= m_origin.x + m_size.x &&
			mouse.y >= m_origin.y && mouse.y <= m_origin.y + m_size.y;
		const glm::vec2 offset = inside ? (mouse - centre) : glm::vec2(0.0f);
		const glm::vec2 anchor = m_centre + offset / m_ppm;
		m_ppm = std::clamp(m_ppm * std::pow(1.2f, steps), std::min(minPpm, fitPpm), maxPpm);
		m_centre = anchor - offset / m_ppm;
	}

	if (in.recenter) m_centre = ctx.player;

	// Drag to pan; a press that does not move is a click (select the marker under the cursor).
	m_hovered = -1;
	const bool overMap = in.mouse.x >= m_origin.x && in.mouse.x <= m_origin.x + m_size.x &&
		in.mouse.y >= m_origin.y && in.mouse.y <= m_origin.y + m_size.y;
	if (in.mouseDown) {
		if (!m_pressed) {
			m_pressed = true;
			m_dragging = false;
			m_pressStart = in.mouse;
			m_lastMouse = in.mouse;
		}
		else {
			if (!m_dragging && glm::length(in.mouse - m_pressStart) > 4.0f) m_dragging = true;
			if (m_dragging) m_centre -= (in.mouse - m_lastMouse) / m_ppm;
			m_lastMouse = in.mouse;
		}
	}
	else if (m_pressed) {
		if (!m_dragging && overMap) {
			const int hit = NearestEvent(ctx, m_pressStart, 18.0f * std::clamp(m_size.y / kRefHeight, 0.6f, 2.2f));
			if (hit >= 0) m_selected = hit;
		}
		m_pressed = false;
		m_dragging = false;
	}
	if (overMap && !m_dragging) {
		m_hovered = NearestEvent(ctx, in.mouse, 16.0f * std::clamp(m_size.y / kRefHeight, 0.6f, 2.2f));
	}

	const int n = static_cast<int>(ctx.events.size());
	if (n > 0 && in.cycle != 0) m_selected = (m_selected + in.cycle % n + n) % n;
	m_selected = std::clamp(m_selected, 0, std::max(0, n - 1));

	if (in.confirm && n > 0) {
		act.toggleWaypoint = true;
		act.eventIndex = m_selected;
	}
	if (in.close) {
		m_open = false;
		act.close = true;
	}
	return act;
}

void WorldMapUI::UpdateScreenCache(const ImVec2& origin, const ImVec2& size)
{
	const int level = LevelForZoom();
	if (m_cacheValid && m_cachePpm == m_ppm && m_cacheCentre == m_centre && m_cacheSize.x == size.x &&
		m_cacheSize.y == size.y && m_cacheLevel == level) {
		return;
	}
	m_cacheValid = true;
	m_cachePpm = m_ppm;
	m_cacheCentre = m_centre;
	m_cacheSize = size;
	m_cacheLevel = level;
	m_screenPts.clear();
	m_runs.clear();

	// World-space view rectangle (north-up: min x/z = top-left).
	const glm::vec2 vmin = ScreenToWorld(glm::vec2(origin.x, origin.y), origin, size);
	const glm::vec2 vmax = ScreenToWorld(glm::vec2(origin.x + size.x, origin.y + size.y), origin, size);
	const float margin = 20.0f / m_ppm;
	for (int i = 0; i < static_cast<int>(m_chains.size()); ++i) {
		const ChainInfo& c = m_chains[i];
		if (c.mx.x < vmin.x - margin || c.mn.x > vmax.x + margin || c.mx.y < vmin.y - margin || c.mn.y > vmax.y + margin) continue;
		const std::vector<glm::vec2>& pts = c.lv[level];
		if (pts.size() < 2) continue;
		ScreenRun run;
		run.chain = i;
		run.offset = m_screenPts.size();
		run.count = pts.size();
		for (const glm::vec2& p : pts) {
			const glm::vec2 s = WorldToScreen(p, origin, size);
			m_screenPts.push_back(ImVec2(s.x, s.y));
		}
		m_runs.push_back(run);
	}
}

void WorldMapUI::DrawSpacedText(ImDrawList* dl, ImFont* font, float px, ImVec2 p, ImU32 col, const char* text, float spacing, bool centre)
{
	if (!font || !text) return;
	float total = 0.0f;
	for (const char* c = text; *c; ++c) {
		char buf[2] = { *c, 0 };
		total += font->CalcTextSizeA(px, FLT_MAX, 0.0f, buf).x + spacing;
	}
	total -= spacing;
	float x = centre ? p.x - total * 0.5f : p.x;
	for (const char* c = text; *c; ++c) {
		char buf[2] = { *c, 0 };
		dl->AddText(font, px, ImVec2(x, p.y), col, buf);
		x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, buf).x + spacing;
	}
}

void WorldMapUI::DrawKeyCap(ImDrawList* dl, ImFont* font, float px, ImVec2 p, const char* key, ImU32 keyCol, float s, float* outWidth)
{
	const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, key);
	const float padX = 7.0f * s, h = px * 1.55f;
	const float w = std::max(ts.x + padX * 2.0f, h);
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), keyCol, 3.0f * s, 0, 1.4f * s);
	dl->AddText(font, px, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), keyCol, key);
	if (outWidth) *outWidth = w;
}

void WorldMapUI::DrawGlowPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 core, float coreW, ImU32 glow, float glowW)
{
	if (n < 2) return;
	dl->AddPolyline(pts, n, glow, ImDrawFlags_None, glowW);
	dl->AddPolyline(pts, n, core, ImDrawFlags_None, coreW);
}

void WorldMapUI::DrawEventMarker(ImDrawList* dl, const ImVec2& c, float s, bool selected, bool hovered)
{
	const float r = 13.0f * s;
	dl->AddCircleFilled(c, r + 6.0f * s, C(170, 100, 255, 46), 32);
	dl->AddCircleFilled(c, r, C(64, 26, 108, 240), 32);
	dl->AddCircle(c, r, C(200, 150, 255, 255), 32, 2.0f * s);
	if (selected || hovered) {
		dl->AddCircle(c, r + 6.0f * s, selected ? C(255, 255, 255, 235) : C(255, 255, 255, 120), 32, 1.4f * s);
	}
	// Flag icon (atlas icon when available, drawn flag otherwise).
	if (!UiIconAtlas::Get().DrawCentered(dl, "events/sprint", c, 17.0f * s, C(255, 255, 255, 255))) {
		const ImVec2 pole0(c.x - 4.0f * s, c.y - 7.0f * s), pole1(c.x - 4.0f * s, c.y + 7.0f * s);
		dl->AddLine(pole0, pole1, C(255, 255, 255, 255), 1.6f * s);
		dl->AddTriangleFilled(ImVec2(c.x - 4.0f * s, c.y - 7.0f * s), ImVec2(c.x + 6.0f * s, c.y - 3.0f * s),
			ImVec2(c.x - 4.0f * s, c.y + 1.0f * s), C(255, 255, 255, 255));
	}
}

void WorldMapUI::DrawParkingMarker(ImDrawList* dl, const ImVec2& c, float s)
{
	const float r = 12.0f * s;
	dl->AddCircleFilled(c, r + 5.0f * s, C(40, 120, 255, 40), 32);
	dl->AddCircleFilled(c, r, C(30, 110, 240, 255), 32);
	dl->AddCircle(c, r, C(200, 225, 255, 230), 32, 1.5f * s);
	ImFont* f = Font(kFontTitle, 17.0f * s);
	const ImVec2 ts = f->CalcTextSizeA(17.0f * s, FLT_MAX, 0.0f, "P");
	dl->AddText(f, 17.0f * s, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), C(255, 255, 255, 255), "P");
}

void WorldMapUI::DrawPlayer(ImDrawList* dl, const ImVec2& c, float heading, float s)
{
	const glm::vec2 f(std::sin(heading), std::cos(heading));  // screen: x east, y south
	const glm::vec2 perp(-f.y, f.x);
	const glm::vec2 o(c.x, c.y);
	dl->AddCircleFilled(c, 20.0f * s, C(120, 230, 255, 40), 32);
	const glm::vec2 tip = o + f * 13.0f * s;
	const glm::vec2 a = o - f * 6.5f * s + perp * 8.5f * s;
	const glm::vec2 b = o - f * 6.5f * s - perp * 8.5f * s;
	if (!UiIconAtlas::Get().Has("navigation/player")) {
		dl->AddTriangleFilled(ImVec2(tip.x, tip.y), ImVec2(a.x, a.y), ImVec2(b.x, b.y), C(245, 252, 255, 255));
		dl->AddTriangle(ImVec2(tip.x, tip.y), ImVec2(a.x, a.y), ImVec2(b.x, b.y), C(120, 230, 255, 255), 1.2f * s);
	}
	else {
		// Icon oriented by the heading: rotate the quad corners.
		const float r = 15.0f * s;
		ImVec2 uv0, uv1;
		if (UiIconAtlas::Get().Uv("navigation/player", uv0, uv1)) {
			const glm::vec2 corners[4] = { o + (-f - perp) * r, o + (-f + perp) * r, o + (f + perp) * r, o + (f - perp) * r };
			// Icon points up (north) in its art, so align its top edge with the forward vector.
			const ImTextureID tex = UiIconAtlas::Get().TextureId();
			dl->AddImageQuad(tex, ImVec2(corners[0].x, corners[0].y), ImVec2(corners[3].x, corners[3].y),
				ImVec2(corners[2].x, corners[2].y), ImVec2(corners[1].x, corners[1].y),
				ImVec2(uv0.x, uv1.y), ImVec2(uv1.x, uv1.y), ImVec2(uv1.x, uv0.y), ImVec2(uv0.x, uv0.y), C(255, 255, 255, 255));
		}
	}
}

void WorldMapUI::Draw(ImDrawList* dl, const glm::vec2& origin, const glm::vec2& size, const WorldMapContext& ctx, float dt)
{
	(void)dt;
	if (!dl || !m_open || size.x <= 1.0f || size.y <= 1.0f) return;
	const auto drawStart = std::chrono::steady_clock::now();
	const ImVec2 o(origin.x, origin.y), sz(size.x, size.y);
	m_origin = o;
	m_size = sz;
	if (!m_viewInit) {
		m_viewInit = true;
		m_ppm = DefaultPpm(sz);
		m_centre = ctx.player;
	}
	const float s = std::clamp(sz.y / kRefHeight, 0.6f, 2.2f);
	ImFont* title = Font(kFontTitle, 34.0f * s);
	ImFont* body = Font(kFontBody, 18.0f * s);
	ImFont* small = Font(kFontBody, 13.0f * s);

	dl->PushClipRect(o, ImVec2(o.x + sz.x, o.y + sz.y), true);
	dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), C(21, 25, 31, 255));

	// Base image: placed by its world bounds (north-up), so roads and markers line up with it.
	if (m_base) {
		const glm::vec2 a = WorldToScreen(m_mapMin, o, sz), b = WorldToScreen(m_mapMax, o, sz);
		const ImTextureID tex = (ImTextureID)rhi::Gpu().ImGuiTextureId(m_base->ID());
		dl->AddImage(tex, ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(0, 0), ImVec2(1, 1), C(255, 255, 255, 255));
	}
	// Faint 1 km grid over the whole map (also the plain background without an image).
	{
		const float step = 1000.0f * m_ppm;
		if (step >= 12.0f) {
			const glm::vec2 tl = ScreenToWorld(glm::vec2(o.x, o.y), o, sz);
			const glm::vec2 br = ScreenToWorld(glm::vec2(o.x + sz.x, o.y + sz.y), o, sz);
			const float x0 = std::floor(tl.x / 1000.0f) * 1000.0f, z0 = std::floor(tl.y / 1000.0f) * 1000.0f;
			for (float x = x0; x <= br.x; x += 1000.0f) {
				const float px = WorldToScreen(glm::vec2(x, 0.0f), o, sz).x;
				dl->AddLine(ImVec2(px, o.y), ImVec2(px, o.y + sz.y), C(120, 150, 170, 14), 1.0f);
			}
			for (float z = z0; z <= br.y; z += 1000.0f) {
				const float py = WorldToScreen(glm::vec2(0.0f, z), o, sz).y;
				dl->AddLine(ImVec2(o.x, py), ImVec2(o.x + sz.x, py), C(120, 150, 170, 14), 1.0f);
			}
		}
	}

	// Carriageways: expressways amber with a bright core; ramps thinner; generic streets grey.
	UpdateScreenCache(o, sz);
	for (const ScreenRun& run : m_runs) {
		const ChainInfo& c = m_chains[run.chain];
		const ImVec2* pts = &m_screenPts[run.offset];
		const int n = static_cast<int>(run.count);
		if (c.kind == 0) {
			DrawGlowPolyline(dl, pts, n, C(255, 178, 66, 240), 2.6f * s, C(255, 120, 20, 52), 11.0f * s);
			dl->AddPolyline(pts, n, C(255, 236, 196, 150), ImDrawFlags_None, 1.0f * s);
		}
		else if (c.kind == 1) {
			DrawGlowPolyline(dl, pts, n, C(236, 140, 56, 190), 1.6f * s, C(255, 120, 20, 30), 6.0f * s);
		}
		else {
			DrawGlowPolyline(dl, pts, n, C(150, 170, 190, 120), 1.0f * s, C(120, 150, 170, 18), 3.0f * s);
		}
	}

	// Route preview of the displayed (hovered, else selected) event: its checkpoints routed through
	// the road graph, in the same cyan as the guidance route. The finish gets a chequered marker.
	if (ctx.previewRoute && ctx.previewRoute->size() >= 2 && !(ctx.route && ctx.route == ctx.previewRoute)) {
		std::vector<ImVec2> pv;
		pv.reserve(ctx.previewRoute->size());
		for (const glm::vec2& p : *ctx.previewRoute) {
			const glm::vec2 q = WorldToScreen(p, o, sz);
			pv.push_back(ImVec2(q.x, q.y));
		}
		// Event route preview, as in the reference: wide soft glow, bright cyan body, white-hot core.
		dl->AddPolyline(pv.data(), static_cast<int>(pv.size()), C(60, 210, 255, 40), ImDrawFlags_None, 22.0f * s);
		dl->AddPolyline(pv.data(), static_cast<int>(pv.size()), C(70, 215, 255, 90), ImDrawFlags_None, 12.0f * s);
		dl->AddPolyline(pv.data(), static_cast<int>(pv.size()), C(90, 225, 255, 230), ImDrawFlags_None, 5.5f * s);
		dl->AddPolyline(pv.data(), static_cast<int>(pv.size()), C(200, 250, 255, 255), ImDrawFlags_None, 2.2f * s);
	}
	if (!ctx.events.empty()) {
		const int di = DisplayedEvent();
		if (di >= 0 && ctx.events[di].hasEnd) {
			const glm::vec2 q = WorldToScreen(ctx.events[di].end, o, sz);
			const float r = 10.0f * s;
			dl->AddCircleFilled(ImVec2(q.x, q.y), r + 4.0f * s, C(0, 0, 0, 140), 24);
			dl->AddCircleFilled(ImVec2(q.x, q.y), r, C(18, 24, 32, 240), 24);
			dl->AddCircle(ImVec2(q.x, q.y), r, C(255, 255, 255, 235), 24, 1.6f * s);
			// Chequered flag: 2x2 squares.
			const float cs = r * 0.55f;
			for (int cy = 0; cy < 2; ++cy) {
				for (int cx = 0; cx < 2; ++cx) {
					if ((cx + cy) % 2 == 0) {
						const float x0 = q.x - cs + cx * cs, y0 = q.y - cs + cy * cs;
						dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + cs, y0 + cs), C(245, 248, 252, 255));
					}
				}
			}
		}
	}

	// Active guidance route: bright cyan with a soft glow.
	std::vector<ImVec2> route;
	if (ctx.route && ctx.route->size() >= 2) {
		route.reserve(ctx.route->size());
		for (const glm::vec2& p : *ctx.route) {
			const glm::vec2 q = WorldToScreen(p, o, sz);
			route.push_back(ImVec2(q.x, q.y));
		}
		dl->AddPolyline(route.data(), static_cast<int>(route.size()), C(80, 220, 255, 60), ImDrawFlags_None, 16.0f * s);
		dl->AddPolyline(route.data(), static_cast<int>(route.size()), C(80, 220, 255, 150), ImDrawFlags_None, 7.0f * s);
		dl->AddPolyline(route.data(), static_cast<int>(route.size()), C(160, 248, 255, 255), ImDrawFlags_None, 2.8f * s);
	}

	// Labels with greedy placement: markers first (player > events > waypoint > parking), then
	// districts, route shields and water names, each trying candidate offsets around its anchor and
	// skipped when nothing fits. Keeps GINZA off the player arrow, SHIBAURA off the P marker, etc.
	{
		struct Box { float x0, y0, x1, y1; };
		std::vector<Box> taken;
		auto overlaps = [&](const Box& b) {
			for (const Box& t : taken) {
				if (b.x0 < t.x1 && b.x1 > t.x0 && b.y0 < t.y1 && b.y1 > t.y0) return true;
			}
			return false;
		};
		auto circleBox = [&](const glm::vec2& q, float r) { return Box{ q.x - r, q.y - r, q.x + r, q.y + r }; };
		{
			const glm::vec2 q = WorldToScreen(ctx.player, o, sz);
			taken.push_back(circleBox(q, 22.0f * s));
		}
		for (const WorldMapMarker& ev : ctx.events) {
			taken.push_back(circleBox(WorldToScreen(ev.pos, o, sz), 20.0f * s));
			if (ev.hasEnd) taken.push_back(circleBox(WorldToScreen(ev.end, o, sz), 14.0f * s));
		}
		if (ctx.hasWaypoint) taken.push_back(circleBox(WorldToScreen(ctx.waypoint, o, sz), 14.0f * s));
		for (const glm::vec2& p : ctx.places) taken.push_back(circleBox(WorldToScreen(p, o, sz), 16.0f * s));

		auto textWidth = [&](ImFont* f, float px, const char* t, float spacing) {
			float w = 0.0f;
			for (const char* c = t; *c; ++c) {
				char buf[2] = { *c, 0 };
				w += f->CalcTextSizeA(px, FLT_MAX, 0.0f, buf).x + spacing;
			}
			return std::max(0.0f, w - spacing);
		};
		// Places a box of (w, h) around anchor q: centre first, then right, left, above, below, diagonals.
		auto place = [&](const glm::vec2& q, float w, float h, float gap, Box& out) {
			const float hw = w * 0.5f + gap, hh = h * 0.5f + gap;
			const glm::vec2 cands[] = { { 0, 0 }, { hw, 0 }, { -hw, 0 }, { 0, -hh * 2.0f }, { 0, hh * 2.0f },
				{ hw, -hh }, { -hw, -hh }, { hw, hh }, { -hw, hh } };
			for (const glm::vec2& d : cands) {
				const glm::vec2 c = q + d;
				const Box b{ c.x - w * 0.5f, c.y - h * 0.5f, c.x + w * 0.5f, c.y + h * 0.5f };
				if (!overlaps(b)) { out = b; return true; }
			}
			return false;
		};
		// Draw order: districts, then route shields, then water names (lower priority = last).
		for (int pass = 0; pass < 3; ++pass) {
			const int kind = pass == 0 ? 1 : (pass == 1 ? 0 : 2);
			for (const MapLabel& l : m_labels) {
				if (l.kind != kind) continue;
				const glm::vec2 q = WorldToScreen(l.pos, o, sz);
				if (q.x < o.x - 200 || q.x > o.x + sz.x + 200 || q.y < o.y - 60 || q.y > o.y + sz.y + 60) continue;
				const ImVec2 p(q.x, q.y);
				if (kind == 1) {
					const float px = 17.0f * s, spacing = 3.0f * s;
					const float w = textWidth(body, px, l.text.c_str(), spacing), h = 17.0f * s;
					Box b;
					if (!place(q, w + 8.0f * s, h + 6.0f * s, 14.0f * s, b)) continue;
					taken.push_back(b);
					const ImVec2 c((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
					DrawSpacedText(dl, body, px, ImVec2(c.x + 1.0f, c.y - 8.0f * s + 1.0f), C(0, 0, 0, 170), l.text.c_str(), spacing, true);
					DrawSpacedText(dl, body, px, ImVec2(c.x, c.y - 8.0f * s), C(240, 244, 248, 240), l.text.c_str(), spacing, true);
				}
				else if (kind == 0) {
					const float px = 15.0f * s;
					const ImVec2 ts = small->CalcTextSizeA(px, FLT_MAX, 0.0f, l.text.c_str());
					const float h = 17.0f * s;
					Box b;
					if (!place(q, ts.x + 12.0f * s, h, 2.0f * s, b)) continue;
					taken.push_back(b);
					const ImVec2 c((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
					dl->AddRectFilled(ImVec2(b.x0, b.y0), ImVec2(b.x1, b.y1), C(8, 12, 18, 215), 3.0f * s);
					dl->AddText(small, px, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), C(245, 248, 252, 255), l.text.c_str());
				}
				else {
					const float px = 16.0f * s, spacing = 6.0f * s;
					const float w = textWidth(body, px, l.text.c_str(), spacing), h = 16.0f * s;
					Box b;
					if (!place(q, w, h, 4.0f * s, b)) continue;
					taken.push_back(b);
					DrawSpacedText(dl, body, px, ImVec2((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f - 8.0f * s),
						C(140, 196, 210, 170), l.text.c_str(), spacing, true);
				}
			}
		}
	}

	// Waypoint (diamond) and the parking / event / player markers.
	if (ctx.hasWaypoint) {
		const glm::vec2 q = WorldToScreen(ctx.waypoint, o, sz);
		const float r = 11.0f * s;
		const ImVec2 d[4] = { ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y) };
		dl->AddConvexPolyFilled(d, 4, C(120, 240, 255, 230));
		dl->AddPolyline(d, 4, C(255, 255, 255, 255), ImDrawFlags_Closed, 1.5f * s);
	}
	for (const glm::vec2& p : ctx.places) {
		const glm::vec2 q = WorldToScreen(p, o, sz);
		if (q.x < o.x - 40 || q.x > o.x + sz.x + 40 || q.y < o.y - 40 || q.y > o.y + sz.y + 40) continue;
		DrawParkingMarker(dl, ImVec2(q.x, q.y), s);
	}
	m_markerScreen.clear();
	for (int i = 0; i < static_cast<int>(ctx.events.size()); ++i) {
		const glm::vec2 q = WorldToScreen(ctx.events[i].pos, o, sz);
		m_markerScreen.push_back(ImVec2(q.x, q.y));
		DrawEventMarker(dl, ImVec2(q.x, q.y), s, i == m_selected, i == m_hovered);
	}
	{
		const glm::vec2 q = WorldToScreen(ctx.player, o, sz);
		DrawPlayer(dl, ImVec2(q.x, q.y), ctx.playerHeading, s);
	}

	// Title (top-left) and filter (top-centre).
	const float left = o.x + 40.0f * s, top = o.y + 30.0f * s;
	DrawSpacedText(dl, title, 34.0f * s, ImVec2(left, top), C(245, 248, 252, 255), "WORLD MAP", 2.5f * s, false);
	DrawSpacedText(dl, body, 17.0f * s, ImVec2(left, top + 46.0f * s), C(150, 186, 216, 235), "SHUTO EXPRESSWAY", 3.5f * s, false);
	{
		const char* filter = "ALL";
		const float fy = top + 6.0f * s;
		const float cx = o.x + sz.x * 0.5f;
		const ImVec2 ts = body->CalcTextSizeA(20.0f * s, FLT_MAX, 0.0f, filter);
		DrawSpacedText(dl, body, 20.0f * s, ImVec2(cx, fy), C(235, 240, 246, 245), filter, 3.0f * s, true);
		const float half = ts.x * 0.5f + 12.0f * s;
		const float ly = fy + ts.y * 0.5f;
		dl->AddLine(ImVec2(cx - half - 30.0f * s, ly), ImVec2(cx - half - 8.0f * s, ly), C(200, 215, 230, 160), 1.5f * s);
		dl->AddLine(ImVec2(cx + half + 8.0f * s, ly), ImVec2(cx + half + 30.0f * s, ly), C(200, 215, 230, 160), 1.5f * s);
	}

	// Event panel (right): hovered event, else the selected one.
	if (!ctx.events.empty()) {
		const int idx = std::clamp(m_hovered >= 0 ? m_hovered : m_selected, 0, static_cast<int>(ctx.events.size()) - 1);
		const WorldMapMarker& ev = ctx.events[idx];
		std::string sub;
		const std::string a = DistrictNear(ev.pos, 3000.0f);
		const std::string b = ev.hasEnd ? DistrictNear(ev.end, 3000.0f) : std::string();
		if (!a.empty() && !b.empty() && a != b) sub = a + " to " + b;
		else if (!a.empty()) sub = "Near " + a;
		const float px = o.x + sz.x - 48.0f * s - 360.0f * s;
		float py = o.y + sz.y * 0.44f;
		const ImU32 lav = C(206, 160, 255, 255);
		DrawSpacedText(dl, title, 30.0f * s, ImVec2(px, py), lav, ev.type.c_str(), 0.6f * s, false);
		py += 38.0f * s;
		DrawSpacedText(dl, title, 28.0f * s, ImVec2(px, py), C(246, 248, 252, 255), ev.title.c_str(), 0.4f * s, false);
		py += 36.0f * s;
		if (!sub.empty()) {
			DrawSpacedText(dl, body, 18.0f * s, ImVec2(px, py), C(206, 218, 232, 250), sub.c_str(), 0.3f * s, false);
		}
		py += 40.0f * s;
		const bool isWp = ctx.hasWaypoint && glm::length(ctx.waypoint - ev.pos) < 1.0f;
		float kw = 0.0f;
		DrawKeyCap(dl, body, 15.0f * s, ImVec2(px, py), ctx.padConnected ? "A" : "ENTER", C(246, 248, 252, 255), s, &kw);
		DrawSpacedText(dl, body, 17.0f * s, ImVec2(px + kw + 12.0f * s, py + 3.0f * s), C(240, 244, 248, 245),
			isWp ? "Clear waypoint" : "Set waypoint", 0.2f * s, false);
	}

	// Legend (bottom-left).
	{
		const float ly = o.y + sz.y - 84.0f * s;
		float x = o.x + 40.0f * s;
		UiIconAtlas::Get().DrawCentered(dl, "navigation/player", ImVec2(x + 12.0f * s, ly), 26.0f * s, C(255, 255, 255, 255));
		if (!UiIconAtlas::Get().Has("navigation/player")) {
			dl->AddTriangleFilled(ImVec2(x + 12.0f * s, ly - 11.0f * s), ImVec2(x + 22.0f * s, ly + 8.0f * s),
				ImVec2(x + 2.0f * s, ly + 8.0f * s), C(245, 252, 255, 255));
		}
		DrawSpacedText(dl, body, 18.0f * s, ImVec2(x + 32.0f * s, ly - 10.0f * s), C(235, 240, 246, 240), "Player", 0.2f * s, false);
		x += 150.0f * s;
		DrawEventMarker(dl, ImVec2(x + 12.0f * s, ly), s * 0.9f, false, false);
		DrawSpacedText(dl, body, 18.0f * s, ImVec2(x + 36.0f * s, ly - 10.0f * s), C(235, 240, 246, 240), "Race", 0.2f * s, false);
		x += 130.0f * s;
		DrawParkingMarker(dl, ImVec2(x + 12.0f * s, ly), s * 0.9f);
		DrawSpacedText(dl, body, 18.0f * s, ImVec2(x + 36.0f * s, ly - 10.0f * s), C(235, 240, 246, 240), "Parking", 0.2f * s, false);
	}

	// Controls (bottom-right): key names are live text in key caps.
	{
		struct Hint { const char* key; const char* label; };
		const std::vector<Hint> hints = ctx.padConnected
			? std::vector<Hint>{ {"LS", "Pan"}, {"LT/RT", "Zoom"}, {"Y", "Center"}, {"B", "Back"} }
			: std::vector<Hint>{ {"WASD", "Pan"}, {"SCROLL", "Zoom"}, {"F", "Center"}, {"ESC", "Back"} };
		const float gap = 28.0f * s;
		float total = 0.0f;
		for (const Hint& h : hints) {
			total += body->CalcTextSizeA(15.0f * s, FLT_MAX, 0.0f, h.key).x + 14.0f * s + 4.0f * s +
				body->CalcTextSizeA(17.0f * s, FLT_MAX, 0.0f, h.label).x + gap;
		}
		total -= gap;
		float x = o.x + sz.x - 40.0f * s - total;
		const float y = o.y + sz.y - 40.0f * s;
		for (const Hint& h : hints) {
			float kw = 0.0f;
			DrawKeyCap(dl, body, 15.0f * s, ImVec2(x, y - 12.0f * s), h.key, C(235, 240, 246, 240), s, &kw);
			x += kw + 8.0f * s;
			DrawSpacedText(dl, body, 17.0f * s, ImVec2(x, y - 12.0f * s + 3.0f * s), C(235, 240, 246, 240), h.label, 0.2f * s, false);
			x += body->CalcTextSizeA(17.0f * s, FLT_MAX, 0.0f, h.label).x + gap;
		}
	}

	// Attribution (ODbL) small, bottom-left.
	if (!m_attribution.empty()) {
		dl->AddText(small, 12.0f * s, ImVec2(o.x + 40.0f * s, o.y + sz.y - 18.0f * s), C(150, 165, 180, 150), m_attribution.c_str());
	}
	dl->PopClipRect();
	RecordDrawTime(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - drawStart).count());
}

void WorldMapUI::RecordDrawTime(double us)
{
	static const bool enabled = [] { const char* v = std::getenv("NOX_WORLDMAP_TIMING"); return v && v[0] == '1'; }();
	if (!enabled) return;
	m_timeSumUs += us;
	m_timeMaxUs = std::max(m_timeMaxUs, us);
	if (++m_timeCount == 60) {
		std::cout << "[WorldMap] Draw CPU avg " << m_timeSumUs / m_timeCount << " us, max " << m_timeMaxUs
			<< " us over " << m_timeCount << " frames (" << m_size.x << "x" << m_size.y << ")" << std::endl;
		m_timeSumUs = 0.0;
		m_timeMaxUs = 0.0;
		m_timeCount = 0;
	}
}
