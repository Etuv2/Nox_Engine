/*
* Main entry point for the Nox Engine application.
* Initializes the main window, runs the application loop, and performs cleanup.
* Returns EXIT_SUCCESS on successful execution, or -1 on initialization failure.
*/
#include "MainWindow.h"
#include <cstdio>
// Trying to force PC to use GPU instead of integrated graphics(weird bug)
extern "C" {
	__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
int main(int argc, char* argv[]) 
{
	// Create and initialize the main application window
	MainWindow mainWindow;
	if (!mainWindow.Initialize())
		return EXIT_FAILURE;
	mainWindow.Run();
	mainWindow.Cleanup();
	return EXIT_SUCCESS;
}