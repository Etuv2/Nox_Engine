/*
* Main entry point for the Nox Engine application (generic executable: no game module is compiled in).
*
* Command line (docs/nfs2015/game.md, "Launching"):
*   Nox_Engine.exe [--preset <name>] [--game | --editor] [--scene <path>] [--quality high|ultra]
*                  [--window windowed|borderless|fullscreen] [--width N --height N] [--no-game]
*                  [--project <projects/x/project.json>] [--bootstrap <nox_bootstrap.json>]
*   Nox_Engine.exe --new-project <name> [--template <id>] [--path projects/<folder>] [--force]
*                  (creates a project from templates/ without a window; docs/engine_overview.md)
* Presets come from config/launch_presets.json ("default" is used without arguments). Assets are
* mounted by ProjectBootstrap (packs beside a shipped exe, or the source checkout above a development
* exe); nothing depends on the working directory. Game code arrives through project runtimes (Lua) and
* scene data; the legacy native racing game is a separate executable (Nox_RacingLegacy.exe).
*/
#include "app/EngineMain.h"
#include "app/ProjectBootstrap.h"
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
	return RunNoxEngine(argc, argv);
}
