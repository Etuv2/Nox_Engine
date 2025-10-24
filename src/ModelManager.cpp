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
    return model;
}
void ModelManager::UnloadModel(const std::string& path)
{
	auto it = m_modelCache.find(path);
	if (it != m_modelCache.end())
		m_modelCache.erase(it);
}