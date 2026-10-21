#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "AutomationServer.h"

#include "Core.h"
#include "rhi/RHI.h"
#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace automation {
namespace {

constexpr size_t kMaxLineBytes = 16u * 1024u * 1024u;
constexpr auto kCallTimeout = std::chrono::seconds(120);

struct ServerState {
	std::mutex mutex;
	std::deque<CallPtr> queue;          // IO thread -> main thread
	std::thread thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> stop{ false };
	uint16_t port = 0;
	SOCKET listener = INVALID_SOCKET;
};

ServerState& State()
{
	static ServerState state;
	return state;
}

Dispatcher& Commands()
{
	static Dispatcher dispatcher;
	return dispatcher;
}

bool ParsePort(const std::string& text, uint16_t& out)
{
	if (text.empty() || text.size() > 5) return false;
	unsigned long value = 0;
	for (char ch : text) {
		if (ch < '0' || ch > '9') return false;
		value = value * 10 + static_cast<unsigned long>(ch - '0');
	}
	if (value == 0 || value > 65535) return false;
	out = static_cast<uint16_t>(value);
	return true;
}

bool SendAll(SOCKET sock, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		const int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
		if (n <= 0) return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

// IO thread: queues one request for the main thread and waits for its envelope. Returns the response line
// (empty for notifications).
std::string HandleLine(const std::string& line)
{
	ParsedLine parsed = ParseRequestLine(line);
	if (!parsed.ok) return parsed.errorResponse.dump() + "\n";

	const CallPtr call = parsed.call;
	std::future<json> future = call->reply.get_future();
	{
		std::lock_guard<std::mutex> lock(State().mutex);
		State().queue.push_back(call);
	}

	const auto deadline = std::chrono::steady_clock::now() + kCallTimeout;
	while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
		const bool stopping = State().stop.load();
		if (stopping || std::chrono::steady_clock::now() >= deadline) {
			// The main thread may still pick the call up; abandoned calls are skipped there.
			call->abandoned = true;
			Finish(call, ErrorEnvelope(rpc::kEngineError, stopping
				? "automation server is stopping"
				: "timed out waiting for the engine main loop (is the window hidden or the engine stopped?)"));
			break;
		}
	}
	const json envelope = future.get();
	if (call->isNotification) return std::string();
	return ResponseFor(call->id, envelope).dump() + "\n";
}

void IoLoop()
{
	ServerState& s = State();
	std::map<SOCKET, std::string> clients;
	while (!s.stop.load()) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(s.listener, &readSet);
		for (const auto& entry : clients) FD_SET(entry.first, &readSet);
		timeval timeout{ 0, 50000 };
		const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
		if (ready <= 0) continue;

		if (FD_ISSET(s.listener, &readSet)) {
			const SOCKET client = accept(s.listener, nullptr, nullptr);
			if (client != INVALID_SOCKET) clients.emplace(client, std::string());
		}

		std::vector<SOCKET> closing;
		for (auto& entry : clients) {
			const SOCKET sock = entry.first;
			if (!FD_ISSET(sock, &readSet)) continue;
			char chunk[4096];
			const int n = recv(sock, chunk, static_cast<int>(sizeof chunk), 0);
			if (n <= 0) {
				closing.push_back(sock);
				continue;
			}
			std::string& buffer = entry.second;
			buffer.append(chunk, static_cast<size_t>(n));
			bool alive = true;
			for (const std::string& line : TakeLines(buffer)) {
				const std::string reply = HandleLine(line);
				if (!reply.empty() && !SendAll(sock, reply)) {
					alive = false;
					break;
				}
			}
			if (alive && buffer.size() > kMaxLineBytes) {
				SendAll(sock, ResponseFor(nullptr, ErrorEnvelope(rpc::kInvalidRequest, "request line too long")).dump() + "\n");
				alive = false;
			}
			if (!alive) closing.push_back(sock);
		}
		for (SOCKET sock : closing) {
			closesocket(sock);
			clients.erase(sock);
		}
	}
	for (const auto& entry : clients) closesocket(entry.first);
}

} // namespace

Options ParseOptions(int argc, char* argv[], const char* envPort)
{
	Options options;
	bool flag = false;
	std::string flagPort;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--automation") {
			flag = true;
		}
		else if (arg.rfind("--automation=", 0) == 0) {
			flag = true;
			flagPort = arg.substr(std::string("--automation=").size());
		}
	}

	std::string portText;
	if (flag) portText = flagPort;
	else if (envPort && *envPort) portText = envPort;
	if (!flag && portText.empty()) return options;   // off

	options.enabled = true;
	if (portText.empty()) {
		options.port = kDefaultPort;
	}
	else if (!ParsePort(portText, options.port)) {
		options.enabled = false;
		options.error = "invalid automation port '" + portText + "' (expected 1-65535)";
	}
	return options;
}

bool Start(uint16_t port, std::string* error)
{
	ServerState& s = State();
	auto fail = [&](const std::string& message) {
		if (error) *error = message;
		return false;
	};
	if (s.running.load()) return true;
	if (port == 0) return fail("automation port must be 1-65535");

	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
	const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) {
		WSACleanup();
		return fail("socket() failed");
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // loopback only: never reachable from the network
	if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 4) != 0) {
		closesocket(listener);
		WSACleanup();
		return fail("could not listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)");
	}

	static bool registered = false;
	if (!registered) {
		RegisterEngineCommands(Commands());
		registered = true;
	}
	s.listener = listener;
	s.port = port;
	s.stop = false;
	s.running = true;
	s.thread = std::thread(IoLoop);
	std::cout << "[Automation] listening on 127.0.0.1:" << port << " (JSON-RPC 2.0, newline-delimited)" << std::endl;
	return true;
}

void Stop()
{
	ServerState& s = State();
	if (!s.running.load()) return;
	s.stop = true;
	if (s.thread.joinable()) s.thread.join();
	closesocket(s.listener);
	s.listener = INVALID_SOCKET;
	WSACleanup();
	s.running = false;
	// Calls nobody will run any more.
	std::deque<CallPtr> leftover;
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		leftover.swap(s.queue);
	}
	for (const CallPtr& call : leftover) {
		call->abandoned = true;
		Finish(call, ErrorEnvelope(rpc::kEngineError, "automation server stopped"));
	}
	std::cout << "[Automation] stopped" << std::endl;
}

bool IsRunning() { return State().running.load(); }
uint16_t Port() { return State().port; }

FrameControl& Control()
{
	static FrameControl control;
	return control;
}

bool BeginFrame(Core& core, float)
{
	if (!State().running.load()) return true;
	FrameControl& control = Control();

	std::deque<CallPtr> batch;
	{
		std::lock_guard<std::mutex> lock(State().mutex);
		batch.swap(State().queue);
	}
	for (const CallPtr& call : batch) Commands().Run(call, &core);

	if (control.stepRemaining > 0) {
		--control.stepRemaining;
		++control.frameIndex;
		return true;
	}
	if (control.paused) return false;
	++control.frameIndex;
	return true;
}

void EndFrame(int width, int height)
{
	if (!State().running.load()) return;
	FrameControl& control = Control();

	if (!control.screenshots.empty() && width > 0 && height > 0) {
		// Same readback as the NOX_FINAL_DUMP path in MainWindow: the backbuffer before Present, UI included.
		std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
		rhi::Gpu().BindReadFramebuffer(rhi::kBackBuffer);
		rhi::Gpu().ReadPixels(rhi::kBackBuffer, 0, { 0, 0, width, height }, rhi::PixelFormat::RGB, rhi::PixelType::UByte, pixels.data(), 1);
		for (const ScreenshotRequest& shot : control.screenshots) {
			std::error_code ec;
			const fs::path target(shot.path);
			if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
			stbi_flip_vertically_on_write(1);
			const int ok = stbi_write_png(shot.path.c_str(), width, height, 3, pixels.data(), width * 3);
			stbi_flip_vertically_on_write(0);
			if (!ok) {
				Finish(shot.call, ErrorEnvelope(rpc::kEngineError, "could not write PNG: " + shot.path));
				continue;
			}
			json result = json::object();
			result["path"] = shot.path;
			result["width"] = width;
			result["height"] = height;
			result["bytes"] = static_cast<uint64_t>(fs::file_size(target, ec));
			result["frame"] = control.frameIndex;
			Finish(shot.call, ResultEnvelope(std::move(result)));
		}
		control.screenshots.clear();
	}

	if (control.stepRemaining == 0 && !control.stepWaiters.empty()) {
		for (const CallPtr& call : control.stepWaiters) {
			json result = json::object();
			result["frame"] = control.frameIndex;
			result["paused"] = control.paused;
			Finish(call, ResultEnvelope(std::move(result)));
		}
		control.stepWaiters.clear();
	}
}

} // namespace automation
