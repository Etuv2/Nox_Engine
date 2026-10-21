#pragma once
// In-engine automation server: lets an external tool (the Claude Code MCP server in tools/nox_mcp, or a
// script) inspect and drive a running engine. Opt-in: --automation[=port] or NOX_AUTOMATION_PORT.
//
// Threads: a background thread owns the sockets (127.0.0.1 only) and parses lines into calls. Every command
// runs on the main thread, inside Core::Update (BeginFrame) or at the end of the frame (EndFrame), so engine
// state is never touched from the network thread. Protocol and command reference: docs/automation.md.

#include "AutomationProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace automation {

constexpr uint16_t kDefaultPort = 47800;

struct Options {
	bool enabled = false;
	uint16_t port = kDefaultPort;
	std::string error;    // non-empty when the flag or variable was malformed (the server stays off)
};
// argv: the process command line. envPort: value of NOX_AUTOMATION_PORT, or null when unset. The command line
// flag wins over the variable for the port. Pure, so tests can call it.
Options ParseOptions(int argc, char* argv[], const char* envPort);

// Starts the IO thread and listens on 127.0.0.1:port. Idempotent. False with *error on bind failure.
bool Start(uint16_t port, std::string* error);
// Stops the IO thread, closes connections and answers pending calls with an error. Safe when not running.
void Stop();
bool IsRunning();
uint16_t Port();

// Main-thread hook at the top of Core::Update. Runs the queued commands, then returns whether this frame
// simulates: false while the engine is paused (the caller returns early; rendering continues).
bool BeginFrame(Core& core, float deltaTime);
// Main-thread hook after Render and before Present. Writes requested screenshots (width x height backbuffer)
// and completes engine.step calls whose frames have run.
void EndFrame(int width, int height);

// Frame stepping state, written by the engine.pause/resume/step handlers and read by BeginFrame/EndFrame.
struct ScreenshotRequest {
	std::string path;
	CallPtr call;
};
struct FrameControl {
	bool paused = false;
	int stepRemaining = 0;         // simulation frames still owed to engine.step
	uint64_t frameIndex = 0;       // simulation frames run since start
	std::vector<CallPtr> stepWaiters;
	std::vector<ScreenshotRequest> screenshots;
};
FrameControl& Control();

// Registers the engine commands (AutomationCommands.cpp) on the dispatcher.
void RegisterEngineCommands(Dispatcher& dispatcher);

} // namespace automation
