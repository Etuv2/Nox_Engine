#include "ModelManager.h"
#include "Scene.h"
#include "SceneNode.h"
#include <iostream>
std::shared_ptr<Scene> ModelManager::LoadModel(const std::string& path)
{
    auto it = m_modelCache.find(path);
    if (it != m_modelCache.end())
        return it->second;

    std::shared_ptr<Scene> model = std::make_shared<Scene>();
    if (!model->LoadFromGLTF(path)) {
        std::cerr << "Failed to load model: " << path << std::endl;
        return nullptr;
    }

    m_modelCache[path] = model;
    
    // NEW: Track the path for this model
    m_modelToPathMap[model.get()] = path;
    
    return model;
}

void ModelManager::UnloadModel(const std::string& path)
{
    auto it = m_modelCache.find(path);
    if (it != m_modelCache.end()) {
        // Remove from reverse map
        m_modelToPathMap.erase(it->second.get());
        m_modelCache.erase(it);
    }
}

// NEW: Get the original path for a loaded model
std::string ModelManager::GetModelPath(const Scene* model) const {
    auto it = m_modelToPathMap.find(model);
    if (it != m_modelToPathMap.end()) {
        return it->second;
    }
    return "";
}