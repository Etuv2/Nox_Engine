#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

/*
* Static triangle collision for large authored worlds (game-agnostic).
*
* A CollisionWorld holds any number of collision meshes (typically one glTF/GLB per world chunk,
* exported separately from the render meshes). Each mesh keeps its triangles in world space with a
* bounding-volume hierarchy, so queries stay logarithmic in the triangle count.
*
* Triangles are classified once by their normal: ground (walkable, normal.y >= filter.groundMinNormalY),
* ceiling (normal.y <= -filter.groundMinNormalY) and wall (everything else). Queries take a class mask so
* vehicles can raycast for the surface under a wheel and push against walls separately, which is
* what makes stacked decks, tunnels and ramps work without any planar road assumptions.
*
* Meshes can be enabled/disabled (chunk activation) without reloading.
*/
class CollisionWorld {
public:
	enum Class : uint8_t { Ground = 1, Wall = 2, Ceiling = 4, Any = 7 };

	struct Hit {
		float t = 0.0f;
		glm::vec3 position{ 0.0f };
		glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
		uint8_t cls = 0;
		int mesh = -1;
	};

	// Classification and sliver rules applied to each triangle when a mesh is added (see AddMesh).
	// Set 'filter' before loading; meshes already in the world keep the rules they were built with.
	// The defaults are neutral: no sliver filtering. A game sets its own thresholds per collision set.
	struct TriangleFilter {
		float groundMinNormalY = 0.6f; // |normal.y| at or above this is ground / ceiling, below is wall
		float minWallHeight = 0.0f;    // m; wall triangles with a vertical extent below this are dropped (0 = off)
		float minWallAspect = 0.0f;    // wall triangles with altitude / longest edge below this are dropped (0 = off)
	};
	TriangleFilter filter;

	// Loads every mesh primitive of a .glb/.gltf (node transforms applied, scaled by 'transform').
	// Returns the mesh index or -1. Nodes whose name contains "NOCOLLIDE" are skipped.
	int LoadGLTF(const std::string& path, const glm::mat4& transform = glm::mat4(1.0f));
	// Adds an indexed triangle list already in world space (classified with the current 'filter').
	int AddMesh(const std::string& name, const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices);
	void Clear() { m_meshes.clear(); }
	bool Empty() const { return m_meshes.empty(); }
	int MeshCount() const { return static_cast<int>(m_meshes.size()); }
	size_t TriangleCount() const;
	void SetEnabled(int mesh, bool enabled);
	bool IsEnabled(int mesh) const { return mesh >= 0 && mesh < MeshCount() && m_meshes[mesh].enabled; }
	const std::string& MeshName(int mesh) const { return m_meshes[mesh].name; }
	void MeshBounds(int mesh, glm::vec3& mn, glm::vec3& mx) const { mn = m_meshes[mesh].nodes.empty() ? glm::vec3(0) : m_meshes[mesh].nodes[0].mn; mx = m_meshes[mesh].nodes.empty() ? glm::vec3(0) : m_meshes[mesh].nodes[0].mx; }

	// Closest hit along origin + dir * t, t in [0, maxT] (dir need not be normalised; t is in its units).
	bool Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxT, uint8_t classMask, Hit& out) const;
	// Highest ground surface at (x, z) between yTop and yTop - maxDrop (first ground hit going down).
	bool GroundBelow(const glm::vec3& top, float maxDrop, Hit& out) const;
	// Pushes a sphere out of all triangles of the given classes. Returns the total displacement; the
	// accumulated contact normal (normalised) is written to outNormal when there was contact.
	// Contacts whose closest point lies above contactMaxY are ignored (e.g. a vehicle's roof line:
	// a sphere is wider than tall bodies, so overhead geometry would otherwise catch it).
	glm::vec3 ResolveSphere(glm::vec3& center, float radius, uint8_t classMask, glm::vec3* outNormal = nullptr, int iterations = 2,
		float contactMaxY = 1e30f) const;

	// One sphere/triangle contact: point on the surface, unit normal pointing from the surface toward the sphere
	// centre (the push direction for the sphere), and penetration depth (radius minus centre distance).
	struct SphereContact {
		glm::vec3 point{ 0.0f };
		glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
		float depth = 0.0f;
	};
	// Appends a contact for every triangle (of the given classes, in enabled meshes) whose closest point is within
	// radius + skin of center. Does not move the sphere; used by the physics engine's mesh colliders.
	void SphereContacts(const glm::vec3& center, float radius, float skin, uint8_t classMask,
		std::vector<SphereContact>& out) const;
	// Appends a contact for every triangle (of the given classes, in enabled meshes) that overlaps the oriented box
	// (centre, half extents, axes = box axis columns in this world's space) within skin. Separating-axis test over
	// the 13 candidate axes. The normal points from the triangle toward the box, depth is the least SAT overlap, and
	// the point is the box corner deepest into the triangle. Does not move the box.
	void BoxContacts(const glm::vec3& center, const glm::vec3& halfExtents, const glm::mat3& axes, float skin,
		uint8_t classMask, std::vector<SphereContact>& out) const;
	// Bounds of all enabled triangles (local space of this world). Returns false when there is no geometry.
	bool Bounds(glm::vec3& mn, glm::vec3& mx) const;
private:
	struct Tri { glm::vec3 a, e1, e2, n; uint8_t cls; };
	struct Node { glm::vec3 mn, mx; int left = -1, right = -1; int first = 0, count = 0; };
	struct Mesh {
		std::string name;
		std::vector<Tri> tris;
		std::vector<Node> nodes;
		bool enabled = true;
	};
	void Build(Mesh& m);
	int BuildNode(Mesh& m, std::vector<glm::vec3>& centroids, int first, int count, int depth);
	template <typename F> void Visit(const Mesh& m, const glm::vec3& mn, const glm::vec3& mx, F&& f) const;
	std::vector<Mesh> m_meshes;
};
