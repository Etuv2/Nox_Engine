#pragma once
// Main-thread phase timer for finding where frame time goes outside the GPU passes.
//
// NOX_LOG_FRAME_PHASES=N prints, every N frames, the median and p95 of each named phase over that
// window ("[FramePhases] name=median/p95 ..."). Phases are marked in execution order with
// FrameProfiler::Mark("name") (time since the previous mark is attributed to "name"); the frame
// ends with FrameProfiler::EndFrame(). Disabled (one branch per mark) when the variable is unset.
#include <string>

namespace FrameProfiler {
	bool Enabled();
	void BeginFrame();
	void Mark(const char* phase);
	void EndFrame();
}
