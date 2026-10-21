#pragma once

// Conservative bounds for skinned meshes. Each joint keeps the bind-space AABB of the vertices it
// influences (built at load). Per frame, every joint's box is moved by that joint's skin matrix and the
// union is taken. A skinned vertex is a weighted sum of joint-transformed points with weights that sum
// to one, so it is a convex combination of points inside those transformed boxes and lies inside their
// union. The result is therefore a valid (if loose) bound for any pose.

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace SkinBounds {

struct JointBounds {
    std::vector<glm::vec3> min;
    std::vector<glm::vec3> max;

    void Reset(size_t jointCount)
    {
        const glm::vec3 posInf(std::numeric_limits<float>::infinity());
        const glm::vec3 negInf(-std::numeric_limits<float>::infinity());
        min.assign(jointCount, posInf);
        max.assign(jointCount, negInf);
    }

    size_t Count() const { return std::min(min.size(), max.size()); }

    bool Empty() const { return Count() == 0; }

    // Grows the box of `joint` to contain `p`. Joints outside the range are ignored.
    void Expand(size_t joint, const glm::vec3& p)
    {
        if (joint >= Count()) {
            return;
        }
        min[joint] = glm::min(min[joint], p);
        max[joint] = glm::max(max[joint], p);
    }
};

// Union of every joint's bind box moved by its matrix. Joints that never influenced a vertex (empty box)
// are skipped. Returns false when no joint has data or the matrix list is empty.
inline bool SkinnedLocalBounds(const JointBounds& bounds, const std::vector<glm::mat4>& jointMatrices,
    glm::vec3& outMin, glm::vec3& outMax)
{
    const size_t count = std::min(bounds.Count(), jointMatrices.size());
    bool any = false;
    glm::vec3 lo(std::numeric_limits<float>::infinity());
    glm::vec3 hi(-std::numeric_limits<float>::infinity());
    for (size_t j = 0; j < count; ++j) {
        const glm::vec3& bmin = bounds.min[j];
        const glm::vec3& bmax = bounds.max[j];
        if (bmin.x > bmax.x) {
            continue;  // no vertex weighted to this joint
        }
        const glm::mat4& m = jointMatrices[j];
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p(
                (corner & 1) ? bmax.x : bmin.x,
                (corner & 2) ? bmax.y : bmin.y,
                (corner & 4) ? bmax.z : bmin.z);
            const glm::vec3 t = glm::vec3(m * glm::vec4(p, 1.0f));
            lo = glm::min(lo, t);
            hi = glm::max(hi, t);
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    outMin = lo;
    outMax = hi;
    return true;
}

// Merges up to eight glTF influences (JOINTS_0/WEIGHTS_0 then JOINTS_1/WEIGHTS_1, `count` of them) into the
// four largest, normalised to sum to one. Joint indices are clamped to [0, jointCount-1] when jointCount > 0.
// A vertex with no usable weight is bound to joint 0 at full weight. Returns true when the vertex has weight.
inline bool MergeInfluences(const uint32_t* joints, const float* weights, size_t count, uint32_t jointCount,
    glm::ivec4& outIds, glm::vec4& outWeights)
{
    struct Influence {
        int joint;
        float weight;
    };
    Influence influences[8] = {};
    size_t n = 0;
    for (size_t k = 0; k < count && n < 8; ++k) {
        int joint = static_cast<int>(joints[k]);
        if (jointCount > 0 && static_cast<uint32_t>(joint) >= jointCount) {
            joint = static_cast<int>(jointCount) - 1;
        }
        influences[n++] = Influence{ joint, weights[k] };
    }
    std::stable_sort(influences, influences + n, [](const Influence& a, const Influence& b) {
        return a.weight > b.weight;
    });

    outIds = glm::ivec4(0);
    outWeights = glm::vec4(0.0f);
    float total = 0.0f;
    for (size_t c = 0; c < 4 && c < n; ++c) {
        outIds[static_cast<int>(c)] = influences[c].joint;
        outWeights[static_cast<int>(c)] = influences[c].weight;
        total += influences[c].weight;
    }
    if (total < 1e-6f) {
        outIds = glm::ivec4(0);
        outWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        return false;
    }
    outWeights /= total;
    return true;
}

// Sphere around an AABB: centre and radius (half diagonal).
inline void AabbToSphere(const glm::vec3& mn, const glm::vec3& mx, glm::vec3& center, float& radius)
{
    center = 0.5f * (mn + mx);
    radius = 0.5f * glm::length(mx - mn);
}

} // namespace SkinBounds
