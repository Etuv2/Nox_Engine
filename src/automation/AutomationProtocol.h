#pragma once
// JSON-RPC 2.0 layer of the automation server (docs/automation.md). Pure: no sockets, no engine state, so the
// framing, request validation and dispatch are unit tested headless (tests/native/test_automation.cpp).
//
// Wire format: one JSON object per line (UTF-8, '\n' terminated), requests and responses alike. Batches
// (JSON arrays) are rejected with -32600. A request without "id" is a notification: it runs, gets no reply.

#include "json.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Core;

namespace automation {

using json = nlohmann::json;

namespace rpc {
	constexpr int kParseError = -32700;
	constexpr int kInvalidRequest = -32600;
	constexpr int kMethodNotFound = -32601;
	constexpr int kInvalidParams = -32602;
	constexpr int kInternalError = -32603;
	// The command was understood but could not be carried out in the engine's current state
	// (not in editor mode, no scene loaded, node deleted, timeout waiting for the main thread ...).
	constexpr int kEngineError = -32000;
	// A scene-dependent command ran while a scene load is in progress. The data member carries the load state.
	constexpr int kSceneLoading = -32001;
}

// Thrown by command handlers; becomes the "error" member of the response.
struct RpcError {
	int code = rpc::kEngineError;
	std::string message;
	json data;
};

// One request in flight. The IO thread creates it, the main thread runs it, and the IO thread waits on the
// future for the envelope: {"result": ...} or {"error": {"code", "message", "data"}}.
struct Call {
	std::string method;
	json params;                       // always an object (absent params become {})
	json id;                           // string, number or null (notifications have no id; the server sets null)
	bool isNotification = false;
	bool deferred = false;             // set by a handler that completes later (render.screenshot, engine.step)
	std::atomic<bool> abandoned{false};// the IO thread gave up (timeout or shutdown); skip it
	std::atomic<bool> finished{false}; // envelope delivered (set once)
	std::promise<json> reply;
};
using CallPtr = std::shared_ptr<Call>;

// Delivers the envelope once. Later calls are ignored.
void Finish(const CallPtr& call, json envelope);
json ResultEnvelope(json result);
json ErrorEnvelope(int code, const std::string& message, json data = nullptr);

// Context a handler runs with on the main thread.
struct Invocation {
	Core* core = nullptr;   // null in unit tests
	CallPtr call;
	json& params;
	// The reply comes later through Finish(call, ...); the dispatcher must not answer now.
	void Defer() { call->deferred = true; }
};

class Dispatcher {
public:
	using Handler = std::function<json(Invocation&)>;

	void Register(const std::string& method, Handler handler);
	bool Has(const std::string& method) const;
	std::vector<std::string> Methods() const;
	// Runs the call on the calling (main) thread and delivers its envelope unless the handler deferred it.
	// Never throws: handler exceptions become error envelopes.
	void Run(const CallPtr& call, Core* core) const;

private:
	std::map<std::string, Handler> m_handlers;
};

// Result of validating one incoming line.
struct ParsedLine {
	bool ok = false;
	CallPtr call;        // valid when ok
	json errorResponse;  // complete JSON-RPC error response (id as far as known) when !ok
};
// Parses one request line into a Call. Invalid input yields a complete error response, never an exception.
ParsedLine ParseRequestLine(const std::string& line);
// Builds the complete response object sent back for an envelope.
json ResponseFor(const json& id, const json& envelope);

// Splits complete lines out of a receive buffer (the trailing partial line stays in the buffer).
// A trailing '\r' is removed. Empty lines are dropped.
std::vector<std::string> TakeLines(std::string& buffer);

} // namespace automation
