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
private:

    std::unordered_map<std::string, std::shared_ptr<Scene>> m_modelCache;
};
