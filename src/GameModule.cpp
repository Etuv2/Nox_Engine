#include "GameModule.h"

namespace {
	GameModuleFactory& Factory()
	{
		static GameModuleFactory factory;
		return factory;
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
