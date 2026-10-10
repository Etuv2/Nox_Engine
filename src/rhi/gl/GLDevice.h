#pragma once
#include "../RHI.h"

// OpenGL 4.6 backend of the RHI (the only place in the engine that calls OpenGL).
namespace rhi::gl {

	// Sets the SDL GL context attributes; returns the SDL window flags the backend needs.
	uint32_t PrepareWindow(bool debug);
	// Creates the GL context on info.window, loads the API and returns the device.
	std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info);

}
