#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"

/*
* Shared road layout for the racing prototype (docs/nfs2015/game.md, docs/shuto/network_format.md).
*
* One graph of nodes and straight edges drives:
*   * world geometry: ProceduralStreetKit's "district" generator builds roads, sidewalks,
*     buildings and lights from it (planar networks);
*   * the minimap: edges are drawn as road lines;
*   * navigation: shortest routes for the event/checkpoint guidance line (one-way edges are only
*     followed from a to b);
*   * vehicle boundaries: the drivable band is road + sidewalk around every edge, unless the
*     network sets "band_walls": false (worlds with authored collision meshes, e.g. stacked decks).
*
* Nodes have an optional height, so stacked carriageways (elevated decks, tunnels, interchanges)
* stay distinct: the 3D queries only consider edges within a vertical tolerance of the query point.
*
* Units: metres. World position (x, z) is stored as glm::vec2(x, z); the node height is 'y'.
* Scene JSON: game.road_network = { road_half_width, sidewalk_width, traffic_side ("right"|"left"),
* band_walls, nodes: [{id, pos:[x,z] or [x,y,z]}], edges: [[idA, idB] or {a, b, name, half_width,
* oneway, lanes, lane_width, carriageway}] } or { "file": "path/to/network.json" } (same schema;
* file-backed networks are saved back as the file reference only).
*/
class RoadNetwork {
public:
	struct Node { std::string id; glm::vec2 pos{ 0.0f }; float y = 0.0f; };
	struct Edge {
		int a = 0, b = 0;
		float halfWidth = 7.5f;
		std::string name;
		bool oneway = false;     // travel only from a to b
		int lanes = 0;           // 0 = unspecified (two-way street)
		float laneWidth = 3.5f;
		std::string carriageway; // stable id of the source carriageway (expressway data)
	};

	// Returns false (and leaves the network empty) when the JSON has no usable nodes/edges.
	bool Load(const nlohmann::json& j);
	nlohmann::json Save() const;
	// Compact grid district used when a scene does not author its own network.
	void BuildDefaultDistrict();
	void Clear();
	bool Empty() const { return m_edges.empty(); }
	// Loaded from an external file (large generated networks): read-only in the editor.
	bool IsExternal() const { return !m_file.empty(); }
	const std::string& SourceFile() const { return m_file; }

	const std::vector<Node>& Nodes() const { return m_nodes; }
	const std::vector<Edge>& Edges() const { return m_edges; }
	std::vector<Node>& MutableNodes() { return m_nodes; }
	std::vector<Edge>& MutableEdges() { return m_edges; }
	int FindNode(const std::string& id) const;

	float SidewalkWidth() const { return m_sidewalkWidth; }
	float DefaultHalfWidth() const { return m_roadHalfWidth; }
	void SetSidewalkWidth(float w) { m_sidewalkWidth = w; }
	void SetDefaultHalfWidth(float w) { m_roadHalfWidth = w; }
	bool LeftHandTraffic() const { return m_leftHand; }
	bool BandWalls() const { return m_bandWalls; }

	// Point on edge e at parameter t (3D, height interpolated).
	glm::vec3 EdgePoint(int e, float t) const;
	glm::vec3 NodePos3(int n) const { return glm::vec3(m_nodes[n].pos.x, m_nodes[n].y, m_nodes[n].pos.y); }

	// Closest point on any edge centre line. Returns the distance from the centre line; outEdge
	// receives the edge index (-1 when empty) and outT the parameter along it.
	float ClosestOnRoads(const glm::vec2& p, glm::vec2* outClosest = nullptr, int* outEdge = nullptr, float* outT = nullptr) const;
	// As above, but only edges whose height at the closest point is within maxDy of p.y (planar
	// distance is returned). Use for stacked roads.
	float ClosestOnRoads3D(const glm::vec3& p, float maxDy, glm::vec3* outClosest = nullptr, int* outEdge = nullptr, float* outT = nullptr) const;
	// Signed clearance to the outer edge of the drivable band (road + sidewalk), using the union of
	// all edge bands and intersection squares. Positive = inside by that many metres. Always large
	// when band walls are disabled.
	float Clearance(const glm::vec2& p, glm::vec2* outNormal = nullptr) const;
	bool IsOnRoad(const glm::vec2& p, float margin = 0.0f) const;  // asphalt only

	// Respawn / placement helper: a position in the driving lane at edge e, parameter t, facing the
	// direction of travel closest to 'heading' (one-way edges always face a -> b). lane < 0 picks the
	// default lane (keep-left/keep-right side lane). Returns the heading in outHeading.
	glm::vec3 LanePosition(int e, float t, float heading, int lane, float& outHeading) const;

	// Polyline from 'from' to 'to' following the streets (Dijkstra over the graph, with both ends
	// projected onto their closest edges; one-way edges respected). Empty when no route exists.
	std::vector<glm::vec2> FindRoute(const glm::vec2& from, const glm::vec2& to) const;
	std::vector<glm::vec2> FindRoute(const glm::vec3& from, const glm::vec3& to, float maxDy = 4.0f) const;

	// Axis-aligned bounds of all nodes, expanded by the band width.
	void Bounds(glm::vec2& mn, glm::vec2& mx) const;

private:
	bool LoadInline(const nlohmann::json& j);
	void BuildIndex();
	template <typename F> void ForEdgesNear(const glm::vec2& p, float radius, F&& f) const;
	std::vector<glm::vec2> RouteBetween(int eFrom, float tFrom, int eTo, float tTo) const;

	std::vector<Node> m_nodes;
	std::vector<Edge> m_edges;
	float m_roadHalfWidth = 7.5f;
	float m_sidewalkWidth = 4.25f;
	bool m_leftHand = false;
	bool m_bandWalls = true;
	std::string m_file;

	// Uniform grid over edges (large networks only; small editable networks stay brute force so
	// editor node moves need no re-indexing).
	static constexpr size_t kIndexThreshold = 512;
	float m_cell = 64.0f;
	std::unordered_map<long long, std::vector<int>> m_grid;
	float m_maxHalfWidth = 0.0f;
};
