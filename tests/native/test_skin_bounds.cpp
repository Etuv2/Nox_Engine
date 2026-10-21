// Skin influence merging (SkinBounds::MergeInfluences, used by the glTF loader) and the skinned bounds used for
// culling (SkinBounds::JointBounds / SkinnedLocalBounds). Headless: pure functions, no model files or GPU.
#include "TestFramework.h"

#include "SkinBounds.h"

#include <cmath>
#include <cstdint>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {

// Mirror of the skinning shader's joint blend, in bind-space vertex order.
glm::vec3 Skin(const glm::vec3& p, const glm::ivec4& ids, const glm::vec4& w, const std::vector<glm::mat4>& joints)
{
    glm::vec3 out(0.0f);
    for (int c = 0; c < 4; ++c) {
        if (w[c] <= 0.0f) continue;
        out += w[c] * glm::vec3(joints[static_cast<size_t>(ids[c])] * glm::vec4(p, 1.0f));
    }
    return out;
}

// Deterministic pseudo-random float in [-1, 1].
float Rand(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
}

} // namespace

NOX_TEST(SkinBounds, MergeUsesFourLargestAndNormalises)
{
    // JOINTS_0 {0:0.2, 1:0.2}, JOINTS_1 {2:0.1, 0:0.0}: sum 0.5 -> 0.4, 0.4, 0.2, 0.
    const uint32_t joints[8] = { 0, 1, 0, 0, 2, 0, 0, 0 };
    const float weights[8] = { 0.2f, 0.2f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f };
    glm::ivec4 ids;
    glm::vec4 w;
    NOX_CHECK(SkinBounds::MergeInfluences(joints, weights, 8, 3, ids, w));
    NOX_CHECK(std::abs(w.x + w.y + w.z + w.w - 1.0f) < 1e-6f);
    NOX_CHECK(std::abs(w.x - 0.4f) < 1e-6f);
    NOX_CHECK(std::abs(w.y - 0.4f) < 1e-6f);
    NOX_CHECK(std::abs(w.z - 0.2f) < 1e-6f);
    NOX_CHECK(w.w == 0.0f);
    NOX_CHECK(ids.z == 2);
}

NOX_TEST(SkinBounds, MergeDropsSmallestOfEightAndNormalisesTheRest)
{
    // Eight influences, all weighted: the four largest survive and sum to one.
    const uint32_t joints[8] = { 0, 1, 2, 3, 4, 0, 1, 2 };
    const float weights[8] = { 0.05f, 0.30f, 0.10f, 0.05f, 0.20f, 0.05f, 0.15f, 0.10f };
    glm::ivec4 ids;
    glm::vec4 w;
    NOX_CHECK(SkinBounds::MergeInfluences(joints, weights, 8, 5, ids, w));
    NOX_CHECK(std::abs(w.x + w.y + w.z + w.w - 1.0f) < 1e-6f);
    // Largest four: 0.30 (joint 1), 0.20 (joint 4), 0.15 (joint 1), 0.10 (joint 2 or 2)
    NOX_CHECK(ids.x == 1);
    NOX_CHECK(ids.y == 4);
    NOX_CHECK(std::abs(w.x - 0.30f / 0.75f) < 1e-5f);
    NOX_CHECK(std::abs(w.y - 0.20f / 0.75f) < 1e-5f);
    NOX_CHECK(std::abs(w.z - 0.15f / 0.75f) < 1e-5f);
    NOX_CHECK(std::abs(w.w - 0.10f / 0.75f) < 1e-5f);
}

NOX_TEST(SkinBounds, MergeClampsJointIndicesToSkinSize)
{
    const uint32_t joints[4] = { 9, 1, 0, 0 };
    const float weights[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
    glm::ivec4 ids;
    glm::vec4 w;
    NOX_CHECK(SkinBounds::MergeInfluences(joints, weights, 4, 3, ids, w));
    NOX_CHECK(ids.x == 2);  // 9 clamped to jointCount - 1
    NOX_CHECK(ids.y == 1);
}

NOX_TEST(SkinBounds, MergeWithoutWeightBindsToJointZero)
{
    const uint32_t joints[4] = { 3, 2, 1, 0 };
    const float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glm::ivec4 ids;
    glm::vec4 w;
    NOX_CHECK(!SkinBounds::MergeInfluences(joints, weights, 4, 4, ids, w));
    NOX_CHECK(ids == glm::ivec4(0));
    NOX_CHECK(w == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

NOX_TEST(SkinBounds, AnimatedBoundsContainEverySkinnedVertex)
{
    // Random rig: 5 joints, 40 vertices with 1..4 influences, random bind positions.
    uint32_t seed = 12345u;
    const size_t jointCount = 5;
    const size_t vertexCount = 40;
    std::vector<glm::vec3> bind(vertexCount);
    std::vector<glm::ivec4> ids(vertexCount);
    std::vector<glm::vec4> weights(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        bind[i] = glm::vec3(Rand(seed) * 3.0f, Rand(seed) * 3.0f, Rand(seed) * 3.0f);
        const int influences = 1 + static_cast<int>((Rand(seed) * 0.5f + 0.5f) * 3.999f);
        uint32_t jointIn[8] = {};
        float weightIn[8] = {};
        for (int c = 0; c < influences; ++c) {
            jointIn[c] = static_cast<uint32_t>((Rand(seed) * 0.5f + 0.5f) * 4.999f);
            weightIn[c] = 0.1f + 0.5f * (Rand(seed) * 0.5f + 0.5f);
        }
        SkinBounds::MergeInfluences(jointIn, weightIn, static_cast<size_t>(influences), static_cast<uint32_t>(jointCount),
            ids[i], weights[i]);
    }

    SkinBounds::JointBounds bounds;
    bounds.Reset(jointCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        for (int c = 0; c < 4; ++c) {
            if (weights[i][c] > 0.0f) bounds.Expand(static_cast<size_t>(ids[i][c]), bind[i]);
        }
    }

    // Sampled poses: random rotations and translations per joint.
    for (int pose = 0; pose < 200; ++pose) {
        std::vector<glm::mat4> joints(jointCount);
        for (auto& m : joints) {
            const glm::quat q = glm::normalize(glm::quat(Rand(seed), Rand(seed), Rand(seed), Rand(seed)));
            m = glm::translate(glm::mat4(1.0f), glm::vec3(Rand(seed), Rand(seed), Rand(seed)) * 2.0f) * glm::mat4_cast(q);
        }
        glm::vec3 lo;
        glm::vec3 hi;
        NOX_REQUIRE(SkinBounds::SkinnedLocalBounds(bounds, joints, lo, hi));
        for (size_t i = 0; i < vertexCount; ++i) {
            const glm::vec3 p = Skin(bind[i], ids[i], weights[i], joints);
            NOX_CHECK(p.x >= lo.x - 1e-3f && p.x <= hi.x + 1e-3f);
            NOX_CHECK(p.y >= lo.y - 1e-3f && p.y <= hi.y + 1e-3f);
            NOX_CHECK(p.z >= lo.z - 1e-3f && p.z <= hi.z + 1e-3f);
        }
    }
}

NOX_TEST(SkinBounds, SphereEnclosesBox)
{
    glm::vec3 c;
    float r = 0.0f;
    SkinBounds::AabbToSphere(glm::vec3(-1.0f, 0.0f, 2.0f), glm::vec3(1.0f, 2.0f, 4.0f), c, r);
    NOX_CHECK(glm::length(c - glm::vec3(0.0f, 1.0f, 3.0f)) < 1e-5f);
    NOX_CHECK(std::abs(r - glm::length(glm::vec3(1.0f, 1.0f, 1.0f))) < 1e-5f);
}
