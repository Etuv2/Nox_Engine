#include "RoadNetwork.h"

#include <algorithm>
#include <cmath>
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
}

bool RoadNetwork::Load(const json& j)
{
	Clear();
	if (!j.is_object()) return false;
	m_roadHalfWidth = j.value("road_half_width", 7.5f);
	m_sidewalkWidth = j.value("sidewalk_width", 4.25f);
	if (j.contains("nodes") && j["nodes"].is_array()) {
		for (const json& n : j["nodes"]) {
			if (!n.is_object()) continue;
			Node node;
			node.id = n.value("id", std::string("N") + std::to_string(m_nodes.size()));
			node.pos = ReadVec2(n.value("pos", json()), glm::vec2(0.0f));
			m_nodes.push_back(node);
		}
	}
	if (j.contains("edges") && j["edges"].is_array()) {
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
			}
			edge.a = FindNode(a);
			edge.b = FindNode(b);
			if (edge.a >= 0 && edge.b >= 0 && edge.a != edge.b) m_edges.push_back(edge);
		}
	}
	if (m_edges.empty()) {
		Clear();
		return false;
	}
	return true;
}

json RoadNetwork::Save() const
{
	json j;
	j["road_half_width"] = m_roadHalfWidth;
	j["sidewalk_width"] = m_sidewalkWidth;
	json nodes = json::array();
	for (const Node& n : m_nodes) nodes.push_back({ {"id", n.id}, {"pos", { n.pos.x, n.pos.y }} });
	j["nodes"] = nodes;
	json edges = json::array();
	for (const Edge& e : m_edges) {
		json je = { {"a", m_nodes[e.a].id}, {"b", m_nodes[e.b].id} };
		if (std::abs(e.halfWidth - m_roadHalfWidth) > 1e-3f) je["half_width"] = e.halfWidth;
		if (!e.name.empty()) je["name"] = e.name;
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
}

int RoadNetwork::FindNode(const std::string& id) const
{
	for (size_t i = 0; i < m_nodes.size(); ++i) {
		if (m_nodes[i].id == id) return static_cast<int>(i);
	}
	return -1;
}

float RoadNetwork::ClosestOnRoads(const glm::vec2& p, glm::vec2* outClosest, int* outEdge, float* outT) const
{
	float best = std::numeric_limits<float>::max();
	int bestEdge = -1;
	float bestT = 0.0f;
	glm::vec2 bestPoint = p;
	for (size_t i = 0; i < m_edges.size(); ++i) {
		const Edge& e = m_edges[i];
		float t;
		glm::vec2 c;
		const float d = SegmentDistance(p, m_nodes[e.a].pos, m_nodes[e.b].pos, t, c);
		if (d < best) {
			best = d;
			bestEdge = static_cast<int>(i);
			bestT = t;
			bestPoint = c;
		}
	}
	if (outClosest) *outClosest = bestPoint;
	if (outEdge) *outEdge = bestEdge;
	if (outT) *outT = bestT;
	return best;
}

float RoadNetwork::Clearance(const glm::vec2& p, glm::vec2* outNormal) const
{
	if (m_edges.empty()) {
		if (outNormal) *outNormal = glm::vec2(0.0f);
		return 1e6f;
	}
	float best = -std::numeric_limits<float>::max();
	glm::vec2 normal(0.0f);
	for (const Edge& e : m_edges) {
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
	}
	if (outNormal) *outNormal = normal;
	return best;
}

bool RoadNetwork::IsOnRoad(const glm::vec2& p, float margin) const
{
	for (const Edge& e : m_edges) {
		float t;
		glm::vec2 c;
		const glm::vec2 a = m_nodes[e.a].pos, b = m_nodes[e.b].pos;
		const glm::vec2 dir = glm::normalize(b - a);
		const float along = glm::dot(p - a, dir);
		const float len = glm::length(b - a);
		if (along < -e.halfWidth || along > len + e.halfWidth) continue;
		SegmentDistance(p, a, b, t, c);
		const float lateral = std::abs(glm::dot(p - a, glm::vec2(-dir.y, dir.x)));
		if (lateral <= e.halfWidth - margin) return true;
	}
	return false;
}

std::vector<glm::vec2> RoadNetwork::FindRoute(const glm::vec2& from, const glm::vec2& to) const
{
	std::vector<glm::vec2> route;
	if (m_edges.empty()) return route;
	glm::vec2 pFrom, pTo;
	int eFrom = -1, eTo = -1;
	ClosestOnRoads(from, &pFrom, &eFrom);
	ClosestOnRoads(to, &pTo, &eTo);
	if (eFrom < 0 || eTo < 0) return route;
	if (eFrom == eTo) {
		route = { pFrom, pTo };
		return route;
	}
	// Graph nodes 0..N-1 plus virtual start (N) and goal (N+1).
	const int n = static_cast<int>(m_nodes.size());
	std::vector<std::vector<std::pair<int, float>>> adj(n + 2);
	for (const Edge& e : m_edges) {
		const float len = glm::length(m_nodes[e.a].pos - m_nodes[e.b].pos);
		adj[e.a].push_back({ e.b, len });
		adj[e.b].push_back({ e.a, len });
	}
	const Edge& ef = m_edges[eFrom];
	const Edge& et = m_edges[eTo];
	adj[n].push_back({ ef.a, glm::length(pFrom - m_nodes[ef.a].pos) });
	adj[n].push_back({ ef.b, glm::length(pFrom - m_nodes[ef.b].pos) });
	adj[et.a].push_back({ n + 1, glm::length(pTo - m_nodes[et.a].pos) });
	adj[et.b].push_back({ n + 1, glm::length(pTo - m_nodes[et.b].pos) });

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
