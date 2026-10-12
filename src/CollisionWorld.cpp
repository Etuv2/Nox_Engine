#include "CollisionWorld.h"

#include "tiny_gltf.h"
#include "assets/AssetIOGltf.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>

namespace {
	constexpr int kLeafTris = 6;

	glm::mat4 NodeMatrix(const tinygltf::Node& n) {
		if (n.matrix.size() == 16) {
			glm::mat4 m;
			for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = static_cast<float>(n.matrix[i]);
			return m;
		}
		glm::mat4 m(1.0f);
		if (n.translation.size() == 3) m = glm::translate(m, glm::vec3(n.translation[0], n.translation[1], n.translation[2]));
		if (n.rotation.size() == 4) m *= glm::mat4_cast(glm::quat(static_cast<float>(n.rotation[3]), static_cast<float>(n.rotation[0]),
			static_cast<float>(n.rotation[1]), static_cast<float>(n.rotation[2])));
		if (n.scale.size() == 3) m = glm::scale(m, glm::vec3(n.scale[0], n.scale[1], n.scale[2]));
		return m;
	}

	// Raw element pointer and stride of an accessor (no sparse support; collision meshes are plain).
	bool AccessorData(const tinygltf::Model& model, int index, const unsigned char*& data, size_t& stride) {
		if (index < 0 || index >= static_cast<int>(model.accessors.size())) return false;
		const tinygltf::Accessor& acc = model.accessors[index];
		if (acc.bufferView < 0) return false;
		const tinygltf::BufferView& view = model.bufferViews[acc.bufferView];
		const tinygltf::Buffer& buf = model.buffers[view.buffer];
		data = buf.data.data() + view.byteOffset + acc.byteOffset;
		const int size = tinygltf::GetComponentSizeInBytes(acc.componentType) * tinygltf::GetNumComponentsInType(acc.type);
		stride = view.byteStride ? view.byteStride : static_cast<size_t>(size);
		return true;
	}

	glm::vec3 ClosestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
		// Ericson, Real-Time Collision Detection 5.1.5.
		const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
		const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f) return a;
		const glm::vec3 bp = p - b;
		const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
		if (d3 >= 0.0f && d4 <= d3) return b;
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
		const glm::vec3 cp = p - c;
		const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
		if (d6 >= 0.0f && d5 <= d6) return c;
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
		const float denom = 1.0f / (va + vb + vc);
		return a + ab * (vb * denom) + ac * (vc * denom);
	}

	bool RayBox(const glm::vec3& o, const glm::vec3& invD, const glm::vec3& mn, const glm::vec3& mx, float maxT) {
		const glm::vec3 t0 = (mn - o) * invD, t1 = (mx - o) * invD;
		const glm::vec3 lo = glm::min(t0, t1), hi = glm::max(t0, t1);
		const float tmin = std::max({ lo.x, lo.y, lo.z, 0.0f });
		const float tmax = std::min({ hi.x, hi.y, hi.z, maxT });
		return tmin <= tmax;
	}
}

int CollisionWorld::LoadGLTF(const std::string& path, const glm::mat4& transform)
{
	tinygltf::TinyGLTF loader;
	tinygltf::Model model;
	std::string err, warn;
	if (!loader.SetFsCallbacks(AssetIO::MakeGltfFsCallbacks(), &err)) {
		std::cerr << "[CollisionWorld] " << err << std::endl;
		return -1;
	}
	const bool binary = path.size() > 4 && (path.substr(path.size() - 4) == ".glb" || path.substr(path.size() - 4) == ".GLB");
	const bool ok = binary ? loader.LoadBinaryFromFile(&model, &err, &warn, path) : loader.LoadASCIIFromFile(&model, &err, &warn, path);
	if (!ok) {
		std::cerr << "[CollisionWorld] Failed to load " << path << ": " << err << std::endl;
		return -1;
	}
	std::vector<glm::vec3> positions;
	std::vector<uint32_t> indices;
	std::function<void(int, const glm::mat4&)> visit = [&](int nodeIndex, const glm::mat4& parent) {
		const tinygltf::Node& node = model.nodes[nodeIndex];
		const glm::mat4 world = parent * NodeMatrix(node);
		if (node.name.find("NOCOLLIDE") != std::string::npos) return;
		if (node.mesh >= 0) {
			for (const tinygltf::Primitive& prim : model.meshes[node.mesh].primitives) {
				if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;
				auto pos = prim.attributes.find("POSITION");
				if (pos == prim.attributes.end()) continue;
				const tinygltf::Accessor& pacc = model.accessors[pos->second];
				if (pacc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || pacc.type != TINYGLTF_TYPE_VEC3) continue;
				const unsigned char* pdata = nullptr;
				size_t pstride = 0;
				if (!AccessorData(model, pos->second, pdata, pstride)) continue;
				const uint32_t base = static_cast<uint32_t>(positions.size());
				for (size_t i = 0; i < pacc.count; ++i) {
					float v[3];
					std::memcpy(v, pdata + i * pstride, sizeof(v));
					positions.push_back(glm::vec3(world * glm::vec4(v[0], v[1], v[2], 1.0f)));
				}
				if (prim.indices >= 0) {
					const tinygltf::Accessor& iacc = model.accessors[prim.indices];
					const unsigned char* idata = nullptr;
					size_t istride = 0;
					if (!AccessorData(model, prim.indices, idata, istride)) continue;
					for (size_t i = 0; i < iacc.count; ++i) {
						uint32_t idx = 0;
						switch (iacc.componentType) {
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: idx = idata[i * istride]; break;
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, idata + i * istride, 2); idx = s; break; }
						default: std::memcpy(&idx, idata + i * istride, 4); break;
						}
						indices.push_back(base + idx);
					}
				}
				else {
					for (uint32_t i = 0; i < pacc.count; ++i) indices.push_back(base + i);
				}
			}
		}
		for (int child : node.children) visit(child, world);
	};
	const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
	if (sceneIndex < static_cast<int>(model.scenes.size())) {
		for (int n : model.scenes[sceneIndex].nodes) visit(n, transform);
	}
	else {
		for (int n = 0; n < static_cast<int>(model.nodes.size()); ++n) visit(n, transform);
	}
	return AddMesh(path, positions, indices);
}

int CollisionWorld::AddMesh(const std::string& name, const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices)
{
	Mesh m;
	m.name = name;
	m.tris.reserve(indices.size() / 3);
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		if (indices[i] >= positions.size() || indices[i + 1] >= positions.size() || indices[i + 2] >= positions.size()) continue;
		Tri t;
		t.a = positions[indices[i]];
		t.e1 = positions[indices[i + 1]] - t.a;
		t.e2 = positions[indices[i + 2]] - t.a;
		const glm::vec3 c = glm::cross(t.e1, t.e2);
		const float len = glm::length(c);
		if (len < 1e-8f) continue; // degenerate
		t.n = c / len;
		t.cls = t.n.y >= filter.groundMinNormalY ? Ground : (t.n.y <= -filter.groundMinNormalY ? Ceiling : Wall);
		if (t.cls == Wall) {
			const glm::vec3 b = t.a + t.e1, c = t.a + t.e2;
			const float extent = std::max({ t.a.y, b.y, c.y }) - std::min({ t.a.y, b.y, c.y });
			if (filter.minWallHeight > 0.0f && extent < filter.minWallHeight) continue; // sliver between surfaces: neither ground nor wall
			// Needle triangles (altitude below minWallAspect of the longest edge) are tessellation slivers, not walls.
			const float longest = std::max({ glm::length(t.e1), glm::length(t.e2), glm::length(t.e2 - t.e1) });
			if (filter.minWallAspect > 0.0f && longest > 1e-6f && len / longest < filter.minWallAspect) continue;
		}
		m.tris.push_back(t);
	}
	if (m.tris.empty()) {
		std::cerr << "[CollisionWorld] " << name << ": no triangles" << std::endl;
		return -1;
	}
	Build(m);
	m_meshes.push_back(std::move(m));
	const Mesh& added = m_meshes.back();
	std::cout << "[CollisionWorld] " << name << ": " << added.tris.size() << " triangles, " << added.nodes.size() << " BVH nodes" << std::endl;
	return static_cast<int>(m_meshes.size()) - 1;
}

size_t CollisionWorld::TriangleCount() const
{
	size_t n = 0;
	for (const Mesh& m : m_meshes) n += m.tris.size();
	return n;
}

void CollisionWorld::SetEnabled(int mesh, bool enabled)
{
	if (mesh >= 0 && mesh < MeshCount()) m_meshes[mesh].enabled = enabled;
}

void CollisionWorld::Build(Mesh& m)
{
	std::vector<glm::vec3> centroids(m.tris.size());
	for (size_t i = 0; i < m.tris.size(); ++i) {
		const Tri& t = m.tris[i];
		centroids[i] = t.a + (t.e1 + t.e2) * (1.0f / 3.0f);
	}
	m.nodes.clear();
	m.nodes.reserve(m.tris.size() / 2 + 1);
	BuildNode(m, centroids, 0, static_cast<int>(m.tris.size()), 0);
}

int CollisionWorld::BuildNode(Mesh& m, std::vector<glm::vec3>& centroids, int first, int count, int depth)
{
	const int index = static_cast<int>(m.nodes.size());
	m.nodes.emplace_back();
	glm::vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
	for (int i = first; i < first + count; ++i) {
		const Tri& t = m.tris[i];
		const glm::vec3 b = t.a + t.e1, c = t.a + t.e2;
		mn = glm::min(mn, glm::min(t.a, glm::min(b, c)));
		mx = glm::max(mx, glm::max(t.a, glm::max(b, c)));
		cmn = glm::min(cmn, centroids[i]);
		cmx = glm::max(cmx, centroids[i]);
	}
	m.nodes[index].mn = mn - glm::vec3(1e-3f);
	m.nodes[index].mx = mx + glm::vec3(1e-3f);
	if (count <= kLeafTris || depth > 40) {
		m.nodes[index].first = first;
		m.nodes[index].count = count;
		return index;
	}
	// Median split on the longest centroid axis.
	const glm::vec3 ext = cmx - cmn;
	const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
	const int mid = first + count / 2;
	std::vector<int> order(count);
	for (int i = 0; i < count; ++i) order[i] = first + i;
	std::nth_element(order.begin(), order.begin() + count / 2, order.end(), [&](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });
	std::vector<Tri> tris(count);
	std::vector<glm::vec3> cents(count);
	for (int i = 0; i < count; ++i) { tris[i] = m.tris[order[i]]; cents[i] = centroids[order[i]]; }
	for (int i = 0; i < count; ++i) { m.tris[first + i] = tris[i]; centroids[first + i] = cents[i]; }
	const int left = BuildNode(m, centroids, first, mid - first, depth + 1);
	const int right = BuildNode(m, centroids, mid, first + count - mid, depth + 1);
	m.nodes[index].left = left;
	m.nodes[index].right = right;
	return index;
}

template <typename F>
void CollisionWorld::Visit(const Mesh& m, const glm::vec3& mn, const glm::vec3& mx, F&& f) const
{
	if (m.nodes.empty()) return;
	int stack[96];
	int sp = 0;
	stack[sp++] = 0;
	while (sp > 0) {
		const Node& n = m.nodes[stack[--sp]];
		if (n.mx.x < mn.x || n.mn.x > mx.x || n.mx.y < mn.y || n.mn.y > mx.y || n.mx.z < mn.z || n.mn.z > mx.z) continue;
		if (n.left < 0) {
			for (int i = n.first; i < n.first + n.count; ++i) f(m.tris[i]);
		}
		else if (sp < 94) {
			stack[sp++] = n.left;
			stack[sp++] = n.right;
		}
	}
}

bool CollisionWorld::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxT, uint8_t classMask, Hit& out) const
{
	bool any = false;
	float best = maxT;
	const glm::vec3 invD(1.0f / (std::abs(dir.x) > 1e-12f ? dir.x : 1e-12f), 1.0f / (std::abs(dir.y) > 1e-12f ? dir.y : 1e-12f),
		1.0f / (std::abs(dir.z) > 1e-12f ? dir.z : 1e-12f));
	for (int mi = 0; mi < MeshCount(); ++mi) {
		const Mesh& m = m_meshes[mi];
		if (!m.enabled || m.nodes.empty()) continue;
		int stack[96];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0) {
			const Node& n = m.nodes[stack[--sp]];
			if (!RayBox(origin, invD, n.mn, n.mx, best)) continue;
			if (n.left >= 0) {
				if (sp < 94) { stack[sp++] = n.left; stack[sp++] = n.right; }
				continue;
			}
			for (int i = n.first; i < n.first + n.count; ++i) {
				const Tri& t = m.tris[i];
				if (!(t.cls & classMask)) continue;
				// Moller-Trumbore, two-sided.
				const glm::vec3 p = glm::cross(dir, t.e2);
				const float det = glm::dot(t.e1, p);
				if (std::abs(det) < 1e-12f) continue;
				const float inv = 1.0f / det;
				const glm::vec3 s = origin - t.a;
				const float u = glm::dot(s, p) * inv;
				if (u < 0.0f || u > 1.0f) continue;
				const glm::vec3 q = glm::cross(s, t.e1);
				const float v = glm::dot(dir, q) * inv;
				if (v < 0.0f || u + v > 1.0f) continue;
				const float tt = glm::dot(t.e2, q) * inv;
				if (tt < 0.0f || tt > best) continue;
				best = tt;
				out.t = tt;
				out.position = origin + dir * tt;
				out.normal = t.n;
				out.cls = t.cls;
				out.mesh = mi;
				any = true;
			}
		}
	}
	return any;
}

bool CollisionWorld::GroundBelow(const glm::vec3& top, float maxDrop, Hit& out) const
{
	if (!Raycast(top, glm::vec3(0.0f, -1.0f, 0.0f), maxDrop, Ground, out)) return false;
	if (out.normal.y < 0.0f) out.normal = -out.normal; // two-sided ground
	return true;
}

glm::vec3 CollisionWorld::ResolveSphere(glm::vec3& center, float radius, uint8_t classMask, glm::vec3* outNormal, int iterations,
	float contactMaxY) const
{
	glm::vec3 total(0.0f);
	for (int iter = 0; iter < iterations; ++iter) {
		glm::vec3 push(0.0f);
		int contacts = 0;
		const glm::vec3 mn = center - glm::vec3(radius), mx = center + glm::vec3(radius);
		for (const Mesh& m : m_meshes) {
			if (!m.enabled) continue;
			Visit(m, mn, mx, [&](const Tri& t) {
				if (!(t.cls & classMask)) return;
				const glm::vec3 q = ClosestOnTriangle(center, t.a, t.a + t.e1, t.a + t.e2);
				if (q.y > contactMaxY) return;
				const glm::vec3 d = center - q;
				const float dist2 = glm::dot(d, d);
				if (dist2 >= radius * radius) return;
				const float dist = std::sqrt(dist2);
				// Push away from the face (centre behind a one-sided wall face is pushed back out the
				// front side the sphere came from, using the separation direction when available).
				const glm::vec3 n = dist > 1e-5f ? d / dist : t.n;
				const glm::vec3 p = n * (radius - dist);
				// Keep the largest push per axis direction (avoids double-counting coplanar triangles).
				for (int k = 0; k < 3; ++k) {
					if (std::abs(p[k]) > std::abs(push[k])) push[k] = p[k];
				}
				++contacts;
			});
		}
		if (contacts == 0) break;
		center += push;
		total += push;
	}
	if (outNormal && glm::dot(total, total) > 1e-12f) *outNormal = glm::normalize(total);
	return total;
}
