#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"

/*
* Shared road layout for the racing prototype (docs/nfs2015/game.md).
*
* One graph of intersections (nodes) and straight streets (edges) on the XZ plane drives:
*   * world geometry: ProceduralStreetKit's "district" generator builds roads, sidewalks,
*     buildings and lights from it;
*   * the minimap: edges are drawn as road lines;
*   * navigation: shortest routes for the event/checkpoint guidance line;
*   * vehicle boundaries: the drivable band is road + sidewalk around every edge.
*
* Units: metres. World position (x, z) is stored as glm::vec2(x, z).
* Scene JSON: game.road_network = { road_half_width, sidewalk_width, nodes: [{id, pos:[x,z]}],
* edges: [[idA, idB] or {a, b, name, half_width}] }.
*/
class RoadNetwork {
public:
	struct Node { std::string id; glm::vec2 pos{ 0.0f }; };
	struct Edge { int a = 0, b = 0; float halfWidth = 7.5f; std::string name; };

	// Returns false (and leaves the network empty) when the JSON has no usable nodes/edges.
	bool Load(const nlohmann::json& j);
	nlohmann::json Save() const;
	// Compact grid district used when a scene does not author its own network.
	void BuildDefaultDistrict();
	void Clear() { m_nodes.clear(); m_edges.clear(); }
	bool Empty() const { return m_edges.empty(); }

	const std::vector<Node>& Nodes() const { return m_nodes; }
	const std::vector<Edge>& Edges() const { return m_edges; }
	std::vector<Node>& MutableNodes() { return m_nodes; }
	std::vector<Edge>& MutableEdges() { return m_edges; }
	int FindNode(const std::string& id) const;

	float SidewalkWidth() const { return m_sidewalkWidth; }
	float DefaultHalfWidth() const { return m_roadHalfWidth; }
	void SetSidewalkWidth(float w) { m_sidewalkWidth = w; }
	void SetDefaultHalfWidth(float w) { m_roadHalfWidth = w; }

	// Closest point on any edge centre line. Returns the distance from the centre line; outEdge
	// receives the edge index (-1 when empty) and outT the parameter along it.
	float ClosestOnRoads(const glm::vec2& p, glm::vec2* outClosest = nullptr, int* outEdge = nullptr, float* outT = nullptr) const;
	// Signed clearance to the outer edge of the drivable band (road + sidewalk), using the union of
	// all edge bands and intersection squares. Positive = inside by that many metres.
	float Clearance(const glm::vec2& p, glm::vec2* outNormal = nullptr) const;
	bool IsOnRoad(const glm::vec2& p, float margin = 0.0f) const;  // asphalt only

	// Polyline from 'from' to 'to' following the streets (Dijkstra over the graph, with both ends
	// projected onto their closest edges). Empty when the network is empty.
	std::vector<glm::vec2> FindRoute(const glm::vec2& from, const glm::vec2& to) const;

	// Axis-aligned bounds of all nodes, expanded by the band width.
	void Bounds(glm::vec2& mn, glm::vec2& mx) const;

private:
	std::vector<Node> m_nodes;
	std::vector<Edge> m_edges;
	float m_roadHalfWidth = 7.5f;
	float m_sidewalkWidth = 4.25f;
};
