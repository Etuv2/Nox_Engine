/*
* Main entry point for the Nox Engine application.
* Initializes the main window, runs the application loop, and performs cleanup.
* Returns EXIT_SUCCESS on successful execution, or -1 on initialization failure.
*/
#include "MainWindow.h"
#include <cstdio>
inline int main(int argc, char* argv[]) 
{
	MainWindow mainWindow;
	if (!mainWindow.Initialize())
		return EXIT_FAILURE;
	mainWindow.Run();
	mainWindow.Cleanup();
	return EXIT_SUCCESS;
}