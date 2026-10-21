#include "MeshCollider.h"

#include "../RigidBody.h"

#include <glm/gtc/matrix_transform.hpp>

#include <map>

namespace MeshCollider {

namespace {

std::map<std::string, std::shared_ptr<const CollisionWorld>>& Cache() {
	static std::map<std::string, std::shared_ptr<const CollisionWorld>> cache;
	return cache;
}

} // namespace

std::shared_ptr<const CollisionWorld> LoadShared(const std::string& path) {
	if (path.empty()) return nullptr;
	auto& cache = Cache();
	if (auto it = cache.find(path); it != cache.end()) return it->second;
	auto world = std::make_shared<CollisionWorld>();
	if (world->LoadGLTF(path, glm::mat4(1.0f)) < 0) {
		cache.emplace(path, nullptr);   // remember the failure so a bad path is not reloaded every frame
		return nullptr;
	}
	std::shared_ptr<const CollisionWorld> shared = world;
	cache.emplace(path, shared);
	return shared;
}

void ClearCache() {
	Cache().clear();
}

bool LocalBounds(const CollisionWorld& mesh, glm::vec3& mn, glm::vec3& mx) {
	return mesh.Bounds(mn, mx);
}

void SphereContacts(const RigidBody& meshBody, const glm::vec3& center, float radius, float skin,
	std::vector<CollisionWorld::SphereContact>& out) {
	const CollisionWorld* mesh = meshBody.getMesh();
	if (!mesh) return;
	// Rigid transform only (mesh bodies take no scale): the sphere maps to a sphere of the same radius.
	const glm::mat4 world = meshBody.getTransformMatrix();
	const glm::mat4 inverse = glm::inverse(world);
	const glm::vec3 localCenter(inverse * glm::vec4(center, 1.0f));
	const glm::mat3 rotation(world);
	const size_t first = out.size();
	mesh->SphereContacts(localCenter, radius, skin, 7 /* Ground | Wall | Ceiling */, out);
	for (size_t i = first; i < out.size(); ++i) {
		CollisionWorld::SphereContact& c = out[i];
		c.point = glm::vec3(world * glm::vec4(c.point, 1.0f));
		c.normal = glm::normalize(rotation * c.normal);
	}
}

bool DeepestSphereContact(const RigidBody& meshBody, const glm::vec3& center, float radius, float skin,
	CollisionWorld::SphereContact& out) {
	std::vector<CollisionWorld::SphereContact> contacts;
	SphereContacts(meshBody, center, radius, skin, contacts);
	if (contacts.empty()) return false;
	const CollisionWorld::SphereContact* best = &contacts[0];
	for (const auto& c : contacts) {
		if (c.depth > best->depth) best = &c;
	}
	out = *best;
	return true;
}

} // namespace MeshCollider
