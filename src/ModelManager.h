// ModelManager.h
#pragma once
#include <string>
#include <memory>
#include <iostream>
#include <unordered_map>

class Scene;
class ModelManager {
public:
    std::shared_ptr<Scene> LoadModel(const std::string& path);
    void UnloadModel(const std::string& path);

    // NEW: Get the original path for a loaded model
    std::string GetModelPath(const Scene* model) const;

private:
    std::unordered_map<std::string, std::shared_ptr<Scene>> m_modelCache;

    // NEW: Reverse lookup from model to path
    std::unordered_map<const Scene*, std::string> m_modelToPathMap;
};
