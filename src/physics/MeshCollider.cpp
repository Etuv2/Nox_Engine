#include "MeshCollider.h"

#include "../RigidBody.h"

#include <glm/gtc/matrix_transform.hpp>

#include <iterator>
#include <map>
#include <string>

namespace MeshCollider {

namespace {

std::map<std::string, std::shared_ptr<const CollisionWorld>>& Cache() {
	static std::map<std::string, std::shared_ptr<const CollisionWorld>> cache;
	return cache;
}

} // namespace

std::shared_ptr<const CollisionWorld> LoadShared(const std::string& path, const glm::vec3& scale) {
	if (path.empty()) return nullptr;
	const bool unit = scale == glm::vec3(1.0f);
	const std::string key = unit ? path : path + "|" + std::to_string(scale.x) + "," + std::to_string(scale.y) + "," + std::to_string(scale.z);
	auto& cache = Cache();
	if (auto it = cache.find(key); it != cache.end()) return it->second;
	auto world = std::make_shared<CollisionWorld>();
	const glm::mat4 transform = unit ? glm::mat4(1.0f) : glm::scale(glm::mat4(1.0f), scale);
	if (world->LoadGLTF(path, transform) < 0) {
		cache.emplace(key, nullptr);   // remember the failure so a bad path is not reloaded every frame
		return nullptr;
	}
	std::shared_ptr<const CollisionWorld> shared = world;
	cache.emplace(key, shared);
	return shared;
}

void ClearCache() {
	Cache().clear();
}

void Invalidate(const std::string& path) {
	auto& cache = Cache();
	for (auto it = cache.begin(); it != cache.end();) {
		const bool same = it->first == path || it->first.compare(0, path.size() + 1, path + "|") == 0;
		it = same ? cache.erase(it) : std::next(it);
	}
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

void BoxContacts(const RigidBody& meshBody, const glm::vec3& center, const glm::vec3& halfExtents,
	const glm::quat& rotation, float skin, std::vector<CollisionWorld::SphereContact>& out) {
	const CollisionWorld* mesh = meshBody.getMesh();
	if (!mesh) return;
	// Rigid mesh transform: its axes are the normalised columns of the world matrix.
	const glm::mat4 world = meshBody.getTransformMatrix();
	const glm::mat3 meshAxes(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])),
		glm::normalize(glm::vec3(world[2])));
	const glm::vec3 localCenter(glm::inverse(world) * glm::vec4(center, 1.0f));
	const glm::mat3 localAxes = glm::transpose(meshAxes) * glm::mat3_cast(rotation);
	std::vector<CollisionWorld::SphereContact> local;
	mesh->BoxContacts(localCenter, halfExtents, localAxes, skin, 7 /* Ground | Wall | Ceiling */, local);
	for (CollisionWorld::SphereContact& c : local) {
		c.point = glm::vec3(world * glm::vec4(c.point, 1.0f));
		c.normal = glm::normalize(meshAxes * c.normal);
		out.push_back(c);
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
