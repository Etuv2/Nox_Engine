#pragma once

#include <cstdint>

// Frame clock for frame-indexed test hooks (NOX_FRAMEBUFFER_DUMP_FRAMES, NOX_EXIT_AFTER_FRAMES, NOX_CAMERA_PATH,
// NOX_SWAP_SCENE, NOX_LIGHT_STEP, the editor smoke schedule, ...). Core advances it once per Update; see
// docs/scenes.md "Loading".
//
// Scene loading is asynchronous: while a scene loads, and for the frames that draw the loading screen over a
// newly committed scene, frames are not scene frames. Hooks index scene frames so that a request for frame N
// means the Nth frame the scene was shown live, whatever the load time was.
namespace nox {

struct SceneFrameClock {
	// Raw count of frames Core has updated since process start. Loading frames included.
	inline static uint64_t total = 0;

	// True for the frame being updated and rendered when it is a live scene frame: the scene is committed and no
	// loading screen is drawn over it. Hooks that act on a frame must check this first.
	inline static bool live = false;

	// 1-based index of the live scene frame: the first frame shown live after a commit is 1. Reset to 0 by every
	// scene commit (startup and scene swaps), so each scene is numbered from its own first frame. While `live` is
	// false this holds the index of the last live frame and must not be used to act on the current frame.
	inline static uint64_t scene = 0;
};

} // namespace nox
