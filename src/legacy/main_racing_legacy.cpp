/*
* Entry point of the legacy native racing executable (Nox_RacingLegacy.exe). It is the generic engine
* command line plus the compiled racing game (GameSession), registered before the window opens. Used for
* parity comparisons with the Lua racing project; --no-game runs the bare engine, as in Nox_Engine.exe.
* Not part of Nox_Engine.exe (docs/engine_boundary.md).
*/
#include "app/EngineMain.h"
#include "app/ProjectBootstrap.h"
#include "NoxRacingGame.h"
#include <cstring>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Trying to force PC to use dedicated GPU instead of integrated graphics(weird bug)
extern "C" {
	__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// Required by the engine's editor code (EditorWorkspace).
std::string NoxExecutablePath()
{
	return ProjectBootstrap::ExecutablePath().string();
}

int main(int argc, char* argv[])
{
	// The racing game is an optional module; --no-game runs the bare engine (viewer/editor).
	bool withGame = true;
	for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--no-game") == 0) withGame = false;
	if (withGame) RegisterNoxRacingGame();
	return RunNoxEngine(argc, argv);
}
