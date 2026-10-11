#include "SceneExtensions.h"
#include <map>

namespace {
	struct Registry {
		std::map<std::string, ProceduralGenerator> generators;
		std::string defaultGenerator;
		std::vector<std::pair<std::string, SceneKeyHandler>> sceneKeys;
	};
	Registry& Get()
	{
		static Registry registry;
		return registry;
	}
}

void SceneExtensions::RegisterGenerator(const std::string& name, ProceduralGenerator generator, bool isDefault)
{
	Get().generators[name] = std::move(generator);
	if (isDefault || Get().defaultGenerator.empty()) Get().defaultGenerator = name;
}

const ProceduralGenerator* SceneExtensions::FindGenerator(const std::string& name)
{
	const auto it = Get().generators.find(name);
	return it == Get().generators.end() ? nullptr : &it->second;
}

const std::string& SceneExtensions::DefaultGenerator()
{
	return Get().defaultGenerator;
}

void SceneExtensions::RegisterSceneKey(const std::string& key, SceneKeyHandler handler)
{
	for (auto& entry : Get().sceneKeys) {
		if (entry.first == key) { entry.second = std::move(handler); return; }
	}
	Get().sceneKeys.emplace_back(key, std::move(handler));
}

const std::vector<std::pair<std::string, SceneKeyHandler>>& SceneExtensions::SceneKeys()
{
	return Get().sceneKeys;
}
