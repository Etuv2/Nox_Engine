#include "RoadNetwork.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <queue>

using nlohmann::json;

namespace {
	glm::vec2 ReadVec2(const json& j, const glm::vec2& fallback) {
		if (j.is_array() && j.size() >= 2 && j[0].is_number() && j[1].is_number()) {
			return glm::vec2(j[0].get<float>(), j[1].get<float>());
		}
		return fallback;
	}

	// Distance from p to segment ab; t receives the clamped parameter.
	float SegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float& t, glm::vec2& closest) {
		const glm::vec2 ab = b - a;
		const float len2 = glm::dot(ab, ab);
		t = len2 > 1e-6f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
		closest = a + ab * t;
		return glm::length(p - closest);
	}

	long long CellKey(int x, int z) { return (static_cast<long long>(x) << 32) ^ static_cast<unsigned int>(z); }
}

void RoadNetwork::Clear()
{
	m_nodes.clear();
	m_edges.clear();
	m_grid.clear();
	m_file.clear();
	m_leftHand = false;
	m_bandWalls = true;
	m_maxHalfWidth = 0.0f;
}

bool RoadNetwork::Load(const json& j)
{
	Clear();
	if (!j.is_object()) return false;
	if (j.contains("file") && j["file"].is_string()) {
		const std::string file = j["file"].get<std::string>();
		std::ifstream in(file);
		const json data = in ? json::parse(in, nullptr, false) : json();
		if (!data.is_object() || !LoadInline(data)) {
			std::cerr << "[RoadNetwork] Could not load " << file << std::endl;
			Clear();
			return false;
		}
		m_file = file;
		std::cout << "[RoadNetwork] " << file << ": " << m_nodes.size() << " nodes, " << m_edges.size() << " edges" << std::endl;
		return true;
	}
	return LoadInline(j);
}

bool RoadNetwork::LoadInline(const json& j)
{
	m_roadHalfWidth = j.value("road_half_width", 7.5f);
	m_sidewalkWidth = j.value("sidewalk_width", 4.25f);
	m_leftHand = j.value("traffic_side", std::string("right")) == "left";
	m_bandWalls = j.value("band_walls", true);
	std::unordered_map<std::string, int> ids;
	if (j.contains("nodes") && j["nodes"].is_array()) {
		m_nodes.reserve(j["nodes"].size());
		for (const json& n : j["nodes"]) {
			if (!n.is_object()) continue;
			Node node;
			node.id = n.value("id", std::string("N") + std::to_string(m_nodes.size()));
			const json pos = n.value("pos", json());
			if (pos.is_array() && pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
				node.pos = glm::vec2(pos[0].get<float>(), pos[2].get<float>());
				node.y = pos[1].get<float>();
			}
			else {
				node.pos = ReadVec2(pos, glm::vec2(0.0f));
			}
			ids[node.id] = static_cast<int>(m_nodes.size());
			m_nodes.push_back(node);
		}
	}
	auto find = [&](const std::string& id) { auto it = ids.find(id); return it == ids.end() ? -1 : it->second; };
	if (j.contains("edges") && j["edges"].is_array()) {
		m_edges.reserve(j["edges"].size());
		for (const json& e : j["edges"]) {
			Edge edge;
			edge.halfWidth = m_roadHalfWidth;
			std::string a, b;
			if (e.is_array() && e.size() >= 2 && e[0].is_string() && e[1].is_string()) {
				a = e[0].get<std::string>();
				b = e[1].get<std::string>();
			}
			else if (e.is_object()) {
				a = e.value("a", std::string());
				b = e.value("b", std::string());
				edge.halfWidth = e.value("half_width", m_roadHalfWidth);
				edge.name = e.value("name", std::string());
				edge.oneway = e.value("oneway", false);
				edge.lanes = e.value("lanes", 0);
				edge.laneWidth = e.value("lane_width", 3.5f);
				edge.carriageway = e.value("carriageway", std::string());
			}
			edge.a = find(a);
			edge.b = find(b);
			if (edge.a >= 0 && edge.b >= 0 && edge.a != edge.b) m_edges.push_back(edge);
		}
	}
	if (m_edges.empty()) {
		Clear();
		return false;
	}
	BuildIndex();
	return true;
}

json RoadNetwork::Save() const
{
	if (!m_file.empty()) return json{ {"file", m_file} };
	json j;
	j["road_half_width"] = m_roadHalfWidth;
	if (m_leftHand) j["traffic_side"] = "left";
	if (!m_bandWalls) j["band_walls"] = false;
	j["sidewalk_width"] = m_sidewalkWidth;
	json nodes = json::array();
	for (const Node& n : m_nodes) {
		if (n.y != 0.0f) nodes.push_back({ {"id", n.id}, {"pos", { n.pos.x, n.y, n.pos.y }} });
		else nodes.push_back({ {"id", n.id}, {"pos", { n.pos.x, n.pos.y }} });
	}
	j["nodes"] = nodes;
	json edges = json::array();
	for (const Edge& e : m_edges) {
		json je = { {"a", m_nodes[e.a].id}, {"b", m_nodes[e.b].id} };
		if (std::abs(e.halfWidth - m_roadHalfWidth) > 1e-3f) je["half_width"] = e.halfWidth;
		if (!e.name.empty()) je["name"] = e.name;
		if (e.oneway) je["oneway"] = true;
		if (e.lanes > 0) { je["lanes"] = e.lanes; je["lane_width"] = e.laneWidth; }
		if (!e.carriageway.empty()) je["carriageway"] = e.carriageway;
		edges.push_back(je);
	}
	j["edges"] = edges;
	return j;
}

void RoadNetwork::BuildDefaultDistrict()
{
	// Two avenues (x = 0 and x = 150) joined by four cross streets: three city blocks per side,
	// several 4-way intersections, a long loop and two dead-end-free turnarounds.
	Clear();
	m_roadHalfWidth = 7.5f;
	m_sidewalkWidth = 4.25f;
	const float rows[] = { -80.0f, 60.0f, 200.0f, 340.0f };
	for (int r = 0; r < 4; ++r) {
		m_nodes.push_back({ "A" + std::to_string(r), glm::vec2(0.0f, rows[r]) });
		m_nodes.push_back({ "B" + std::to_string(r), glm::vec2(150.0f, rows[r]) });
	}
	auto add = [&](const std::string& a, const std::string& b, const std::string& name) {
		Edge e;
		e.a = FindNode(a);
		e.b = FindNode(b);
		e.halfWidth = m_roadHalfWidth;
		e.name = name;
		m_edges.push_back(e);
	};
	for (int r = 0; r < 3; ++r) {
		add("A" + std::to_string(r), "A" + std::to_string(r + 1), "Ventura Ave");
		add("B" + std::to_string(r), "B" + std::to_string(r + 1), "Palm Blvd");
	}
	const char* cross[] = { "1st St", "2nd St", "3rd St", "4th St" };
	for (int r = 0; r < 4; ++r) add("A" + std::to_string(r), "B" + std::to_string(r), cross[r]);
	BuildIndex();
}

int RoadNetwork::FindNode(const std::string& id) const
{
	for (size_t i = 0; i < m_nodes.size(); ++i) {
		if (m_nodes[i].id == id) return static_cast<int>(i);
	}
	return -1;
}

void RoadNetwork::BuildIndex()
{
	m_grid.clear();
	m_maxHalfWidth = 0.0f;
	for (const Edge& e : m_edges) m_maxHalfWidth = std::max(m_maxHalfWidth, e.halfWidth);
	if (m_edges.size() < kIndexThreshold) return;
	for (size_t i = 0; i < m_edges.size(); ++i) {
		const glm::vec2 a = m_nodes[m_edges[i].a].pos, b = m_nodes[m_edges[i].b].pos;
		const glm::vec2 mn = glm::min(a, b), mx = glm::max(a, b);
		const int x0 = static_cast<int>(std::floor(mn.x / m_cell)), x1 = static_cast<int>(std::floor(mx.x / m_cell));
		const int z0 = static_cast<int>(std::floor(mn.y / m_cell)), z1 = static_cast<int>(std::floor(mx.y / m_cell));
		for (int x = x0; x <= x1; ++x)
			for (int z = z0; z <= z1; ++z) m_grid[CellKey(x, z)].push_back(static_cast<int>(i));
	}
}

// Calls f(edgeIndex) for every edge whose centre line may lie within 'radius' of p (each edge at
// most once). Without an index every edge is visited.
template <typename F>
void RoadNetwork::ForEdgesNear(const glm::vec2& p, float radius, F&& f) const
{
	if (m_grid.empty()) {
		for (size_t i = 0; i < m_edges.size(); ++i) f(static_cast<int>(i));
		return;
	}
	const int x0 = static_cast<int>(std::floor((p.x - radius) / m_cell)), x1 = static_cast<int>(std::floor((p.x + radius) / m_cell));
	const int z0 = static_cast<int>(std::floor((p.y - radius) / m_cell)), z1 = static_cast<int>(std::floor((p.y + radius) / m_cell));
	if (static_cast<long long>(x1 - x0 + 1) * (z1 - z0 + 1) > 4096) {
		for (size_t i = 0; i < m_edges.size(); ++i) f(static_cast<int>(i));
		return;
	}
	std::vector<int> seen;
	for (int x = x0; x <= x1; ++x) {
		for (int z = z0; z <= z1; ++z) {
			auto it = m_grid.find(CellKey(x, z));
			if (it == m_grid.end()) continue;
			for (int e : it->second) {
				if (std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
				seen.push_back(e);
				f(e);
			}
		}
	}
}

glm::vec3 RoadNetwork::EdgePoint(int e, float t) const
{
	const Edge& edge = m_edges[e];
	return glm::mix(NodePos3(edge.a), NodePos3(edge.b), t);
}

float RoadNetwork::ClosestOnRoads(const glm::vec2& p, glm::vec2* outClosest, int* outEdge, float* outT) const
{
	glm::vec3 c3;
	const float d = ClosestOnRoads3D(glm::vec3(p.x, 0.0f, p.y), std::numeric_limits<float>::max(), &c3, outEdge, outT);
	if (outClosest) *outClosest = glm::vec2(c3.x, c3.z);
	return d;
}

float RoadNetwork::ClosestOnRoads3D(const glm::vec3& p3, float maxDy, glm::vec3* outClosest, int* outEdge, float* outT) const
{
	const glm::vec2 p(p3.x, p3.z);
	float best = std::numeric_limits<float>::max();
	int bestEdge = -1;
	float bestT = 0.0f;
	glm::vec3 bestPoint = p3;
	auto consider = [&](int i) {
		const Edge& e = m_edges[i];
		float t;
		glm::vec2 c;
		const float d = SegmentDistance(p, m_nodes[e.a].pos, m_nodes[e.b].pos, t, c);
		if (d >= best) return;
		const float y = glm::mix(m_nodes[e.a].y, m_nodes[e.b].y, t);
		if (std::abs(y - p3.y) > maxDy) return;
		best = d;
		bestEdge = i;
		bestT = t;
		bestPoint = glm::vec3(c.x, y, c.y);
	};
	if (m_grid.empty()) {
		for (size_t i = 0; i < m_edges.size(); ++i) consider(static_cast<int>(i));
	}
	else {
		// Growing search radius over the index (a hit within r is final once r covers it).
		for (float r = m_cell; ; r *= 4.0f) {
			ForEdgesNear(p, r, consider);
			if ((bestEdge >= 0 && best <= r) || r > 2e5f) break;
		}
	}
	if (outClosest) *outClosest = bestPoint;
	if (outEdge) *outEdge = bestEdge;
	if (outT) *outT = bestT;
	return best;
}

float RoadNetwork::Clearance(const glm::vec2& p, glm::vec2* outNormal) const
{
	if (m_edges.empty() || !m_bandWalls) {
		if (outNormal) *outNormal = glm::vec2(0.0f);
		return 1e6f;
	}
	float best = -std::numeric_limits<float>::max();
	glm::vec2 normal(0.0f);
	ForEdgesNear(p, m_maxHalfWidth + m_sidewalkWidth + m_cell, [&](int i) {
		const Edge& e = m_edges[i];
		const glm::vec2 a = m_nodes[e.a].pos, b = m_nodes[e.b].pos;
		const float band = e.halfWidth + m_sidewalkWidth;
		// Box around the segment (square ends): clearance = band - max(|lateral|, overhang).
		const glm::vec2 dir = glm::length(b - a) > 1e-4f ? glm::normalize(b - a) : glm::vec2(1, 0);
		const glm::vec2 side(-dir.y, dir.x);
		const float len = glm::length(b - a);
		const float along = glm::dot(p - a, dir);
		const float lateral = glm::dot(p - a, side);
		const float overhang = along < 0.0f ? -along : (along > len ? along - len : 0.0f);
		const float clearLat = band - std::abs(lateral);
		const float clearEnd = band - overhang;
		const float c = std::min(clearLat, clearEnd);
		if (c > best) {
			best = c;
			normal = clearLat < clearEnd ? side * (lateral >= 0.0f ? 1.0f : -1.0f) : dir * (along < 0.0f ? -1.0f : 1.0f);
		}
	});
	if (outNormal) *outNormal = normal;
	return best;
}

bool RoadNetwork::IsOnRoad(const glm::vec2& p, float margin) const
{
	bool on = false;
	ForEdgesNear(p, m_maxHalfWidth, [&](int i) {
		if (on) return;
		const Edge& e = m_edges[i];
		const glm::vec2 a = m_nodes[e.a].pos, b = m_nodes[e.b].pos;
		const float len = glm::length(b - a);
		if (len < 1e-4f) return;
		const glm::vec2 dir = (b - a) / len;
		const float along = glm::dot(p - a, dir);
		if (along < -e.halfWidth || along > len + e.halfWidth) return;
		const float lateral = std::abs(glm::dot(p - a, glm::vec2(-dir.y, dir.x)));
		if (lateral <= e.halfWidth - margin) on = true;
	});
	return on;
}

glm::vec3 RoadNetwork::LanePosition(int ei, float t, float heading, int lane, float& outHeading) const
{
	const Edge& e = m_edges[ei];
	const glm::vec3 c = EdgePoint(ei, t);
	glm::vec2 dir = m_nodes[e.b].pos - m_nodes[e.a].pos;
	dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec2(0, 1);
	const glm::vec2 fwd(std::sin(heading), std::cos(heading));
	if (!e.oneway && glm::dot(dir, fwd) < 0.0f) dir = -dir;
	// Facing dir, the car's right is (-dir.y, dir.x).
	const glm::vec2 right(-dir.y, dir.x);
	float offset;
	if (e.oneway && e.lanes > 0) {
		// Carriageway centre line = centre of its lanes; lane 0 is the leftmost.
		const int l = lane < 0 ? (m_leftHand ? 0 : e.lanes - 1) : std::min(lane, e.lanes - 1);
		offset = (static_cast<float>(l) - 0.5f * static_cast<float>(e.lanes - 1)) * e.laneWidth;
	}
	else {
		// Two-way street: the driving side's lane.
		offset = e.halfWidth * 0.3f * (m_leftHand ? -1.0f : 1.0f);
	}
	outHeading = std::atan2(dir.x, dir.y);
	return c + glm::vec3(right.x, 0.0f, right.y) * offset;
}

std::vector<glm::vec2> RoadNetwork::FindRoute(const glm::vec2& from, const glm::vec2& to) const
{
	int eFrom = -1, eTo = -1;
	float tFrom = 0.0f, tTo = 0.0f;
	ClosestOnRoads(from, nullptr, &eFrom, &tFrom);
	ClosestOnRoads(to, nullptr, &eTo, &tTo);
	return RouteBetween(eFrom, tFrom, eTo, tTo);
}

std::vector<glm::vec2> RoadNetwork::FindRoute(const glm::vec3& from, const glm::vec3& to, float maxDy) const
{
	int eFrom = -1, eTo = -1;
	float tFrom = 0.0f, tTo = 0.0f;
	ClosestOnRoads3D(from, maxDy, nullptr, &eFrom, &tFrom);
	if (eFrom < 0) ClosestOnRoads3D(from, std::numeric_limits<float>::max(), nullptr, &eFrom, &tFrom);
	ClosestOnRoads3D(to, maxDy, nullptr, &eTo, &tTo);
	if (eTo < 0) ClosestOnRoads3D(to, std::numeric_limits<float>::max(), nullptr, &eTo, &tTo);
	return RouteBetween(eFrom, tFrom, eTo, tTo);
}

std::vector<glm::vec2> RoadNetwork::RouteBetween(int eFrom, float tFrom, int eTo, float tTo) const
{
	std::vector<glm::vec2> route;
	if (eFrom < 0 || eTo < 0) return route;
	auto at = [&](int e, float t) { return glm::mix(m_nodes[m_edges[e].a].pos, m_nodes[m_edges[e].b].pos, t); };
	const glm::vec2 pFrom = at(eFrom, tFrom), pTo = at(eTo, tTo);
	if (eFrom == eTo && (!m_edges[eFrom].oneway || tFrom <= tTo)) {
		route = { pFrom, pTo };
		return route;
	}
	// Graph nodes 0..N-1 plus virtual start (N) and goal (N+1).
	const int n = static_cast<int>(m_nodes.size());
	std::vector<std::vector<std::pair<int, float>>> adj(n + 2);
	for (const Edge& e : m_edges) {
		const float len = glm::length(m_nodes[e.a].pos - m_nodes[e.b].pos);
		adj[e.a].push_back({ e.b, len });
		if (!e.oneway) adj[e.b].push_back({ e.a, len });
	}
	const Edge& ef = m_edges[eFrom];
	const Edge& et = m_edges[eTo];
	adj[n].push_back({ ef.b, glm::length(pFrom - m_nodes[ef.b].pos) });
	if (!ef.oneway) adj[n].push_back({ ef.a, glm::length(pFrom - m_nodes[ef.a].pos) });
	adj[et.a].push_back({ n + 1, glm::length(pTo - m_nodes[et.a].pos) });
	if (!et.oneway) adj[et.b].push_back({ n + 1, glm::length(pTo - m_nodes[et.b].pos) });

	std::vector<float> dist(n + 2, std::numeric_limits<float>::max());
	std::vector<int> prev(n + 2, -1);
	using Item = std::pair<float, int>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
	dist[n] = 0.0f;
	open.push({ 0.0f, n });
	while (!open.empty()) {
		const auto [d, u] = open.top();
		open.pop();
		if (d > dist[u]) continue;
		if (u == n + 1) break;
		for (const auto& [v, w] : adj[u]) {
			if (dist[u] + w < dist[v]) {
				dist[v] = dist[u] + w;
				prev[v] = u;
				open.push({ dist[v], v });
			}
		}
	}
	if (prev[n + 1] < 0) return route;
	std::vector<int> chain;
	for (int v = prev[n + 1]; v >= 0 && v != n; v = prev[v]) chain.push_back(v);
	route.push_back(pFrom);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) route.push_back(m_nodes[*it].pos);
	route.push_back(pTo);
	return route;
}

void RoadNetwork::Bounds(glm::vec2& mn, glm::vec2& mx) const
{
	mn = glm::vec2(std::numeric_limits<float>::max());
	mx = glm::vec2(-std::numeric_limits<float>::max());
	for (const Node& n : m_nodes) {
		mn = glm::min(mn, n.pos);
		mx = glm::max(mx, n.pos);
	}
	const float band = m_roadHalfWidth + m_sidewalkWidth;
	mn -= glm::vec2(band);
	mx += glm::vec2(band);
}
