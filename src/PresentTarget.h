#pragma once
#include "rhi/RHI.h"

// Final output framebuffer of the renderer. kBackBuffer = the window back buffer (standalone game); the
// editor points it at the active viewport's offscreen target so the frame graph renders into a
// texture shown inside a docked panel. Passes bind this wherever they would bind the back buffer.
namespace PresentTarget {
	inline rhi::FramebufferHandle g_framebuffer = rhi::kBackBuffer;
	inline rhi::FramebufferHandle Get() { return g_framebuffer; }
	inline void Set(rhi::FramebufferHandle framebuffer) { g_framebuffer = framebuffer; }
}
