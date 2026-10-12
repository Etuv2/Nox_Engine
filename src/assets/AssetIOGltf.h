#pragma once
// TinyGLTF filesystem callbacks backed by AssetIO. One implementation is shared by the render loader
// (Scene.cpp) and the collision loader (CollisionWorld.cpp).
//
// External buffers and images are resolved relative to the model's logical directory (TinyGLTF joins
// the base directory of the model path with the URI and passes the result to ExpandFilePath). Results
// are logical paths, so ".." is resolved lexically and may not leave the asset root. Writes are refused.
#include "tiny_gltf.h"

namespace AssetIO {
	tinygltf::FsCallbacks MakeGltfFsCallbacks();
}
