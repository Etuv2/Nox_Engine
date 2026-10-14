// Legacy native racing game (Nox_RacingLegacy.exe only): registers the GameSession module, its editor
// panels and the procedural street generators with the engine. Nothing in the engine refers to the names or
// file layouts used here.
#include "NoxRacingGame.h"
#include "GameModule.h"
#include "GameEditorPanels.h"
#include "GameSession.h"
#include "procedural/ProceduralStreetGenerators.h"

void RegisterNoxRacingGame()
{
	GameModules::SetFactory([] { return std::make_unique<GameSession>(); });
	RegisterRacingEditorPanels();
	RegisterProceduralStreetGenerators();
	// The window room atlas scene key ("window_room_root") is generic engine data (SceneLoader).
}
