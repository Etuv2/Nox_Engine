#pragma once
#include <GL/glew.h>

// Final output framebuffer of the renderer. 0 = the window back buffer (standalone game); the
// editor points it at the active viewport's offscreen target so the frame graph renders into a
// texture shown inside a docked panel. Passes bind this wherever they previously bound 0.
namespace PresentTarget {
	inline GLuint g_framebuffer = 0;
	inline GLuint Get() { return g_framebuffer; }
	inline void Set(GLuint framebuffer) { g_framebuffer = framebuffer; }
}
