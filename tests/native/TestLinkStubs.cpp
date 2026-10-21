// nox_tests links NoxEngineCore but not the executables' main.cpp. EditorWorkspace::RunStandalone calls
// NoxExecutablePath(), which only Nox_Engine.exe and Nox_RacingLegacy.exe define. The tests never start a
// standalone game, so an empty path is enough to link. (Pre-existing since 9d0ac15.)
#include <string>

std::string NoxExecutablePath()
{
	return std::string();
}
