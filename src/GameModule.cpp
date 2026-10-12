#include "GameModule.h"
#include "assets/AssetIO.h"

#include <map>

namespace {
	GameModuleFactory& Factory()
	{
		static GameModuleFactory factory;
		return factory;
	}

	std::map<std::string, ProjectModuleFactory>& Runtimes()
	{
		static std::map<std::string, ProjectModuleFactory> runtimes;
		return runtimes;
	}
}

void GameModules::SetFactory(GameModuleFactory factory)
{
	Factory() = std::move(factory);
}

std::unique_ptr<IGameModule> GameModules::Create()
{
	return Factory() ? Factory()() : nullptr;
}

void GameModules::RegisterProjectRuntime(const std::string& runtime, ProjectModuleFactory factory)
{
	Runtimes()[runtime] = std::move(factory);
}

std::unique_ptr<IGameModule> GameModules::CreateForProject(const std::string& descriptorPath, std::string& error)
{
	const auto logical = AssetIO::Normalize(descriptorPath, &error);
	if (!logical) return nullptr;
	const auto text = AssetIO::ReadText(*logical, &error);
	if (!text) return nullptr;
	const nlohmann::json descriptor = nlohmann::json::parse(*text, nullptr, false);
	if (!descriptor.is_object() || descriptor.value("format", 0) != 1) {
		error = "invalid project descriptor (expected an object with \"format\": 1)";
		return nullptr;
	}
	const std::string runtime = descriptor.value("runtime", std::string());
	const auto it = Runtimes().find(runtime);
	if (it == Runtimes().end() || !it->second) {
		error = "no project runtime registered for \"" + runtime + "\"";
		return nullptr;
	}
	return it->second(*logical, descriptor, error);
}
