#include "BVH.h"
#include "SceneNode.h"
#include <iostream>

std::unique_ptr<BVH::SceneNodeBVH> BVH::CreateSceneNodeBVH(const std::vector<std::shared_ptr<SceneNode>>& nodes) {
    auto extractor = [](const std::shared_ptr<SceneNode>& node) -> BoundingVolume {
        if (node && node->GetModel()) {
            auto bounds = node->GetBoundingBox();
            return BoundingVolume(bounds.first, bounds.second);
        }
        return BoundingVolume();
    };
    
    auto bvh = std::make_unique<SceneNodeBVH>(extractor);
    bvh->Build(nodes);
    return bvh;
}