#include "MainWindow.h"
int main(int argc, char* argv[]) 
{
	MainWindow mainWindow;
	if (!mainWindow.Initialize())
		return -1;
	mainWindow.Run();
	mainWindow.Cleanup();
	return 0;
}