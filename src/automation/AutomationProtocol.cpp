#include "AutomationProtocol.h"

#include <exception>

namespace automation {

void Finish(const CallPtr& call, json envelope)
{
	if (!call || call->finished.exchange(true)) return;
	call->reply.set_value(std::move(envelope));
}

json ResultEnvelope(json result)
{
	json env = json::object();
	env["result"] = std::move(result);
	return env;
}

json ErrorEnvelope(int code, const std::string& message, json data)
{
	json error = json::object();
	error["code"] = code;
	error["message"] = message;
	if (!data.is_null()) error["data"] = std::move(data);
	json env = json::object();
	env["error"] = std::move(error);
	return env;
}

void Dispatcher::Register(const std::string& method, Handler handler)
{
	m_handlers[method] = std::move(handler);
}

bool Dispatcher::Has(const std::string& method) const
{
	return m_handlers.find(method) != m_handlers.end();
}

std::vector<std::string> Dispatcher::Methods() const
{
	std::vector<std::string> names;
	names.reserve(m_handlers.size());
	for (const auto& entry : m_handlers) names.push_back(entry.first);
	return names;
}

void Dispatcher::Run(const CallPtr& call, Core* core) const
{
	if (!call || call->abandoned.load()) return;
	json envelope;
	const auto it = m_handlers.find(call->method);
	if (it == m_handlers.end()) {
		envelope = ErrorEnvelope(rpc::kMethodNotFound, "method not found: " + call->method);
	}
	else {
		try {
			Invocation inv{ core, call, call->params };
			json result = it->second(inv);
			if (call->deferred) return;          // answered later through Finish()
			envelope = ResultEnvelope(std::move(result));
		}
		catch (const RpcError& e) {
			envelope = ErrorEnvelope(e.code, e.message, e.data);
		}
		catch (const std::exception& e) {
			envelope = ErrorEnvelope(rpc::kInternalError, std::string("internal error: ") + e.what());
		}
		catch (...) {
			envelope = ErrorEnvelope(rpc::kInternalError, "internal error");
		}
	}
	// Notifications are finished too; nobody waits on them, so the envelope is simply dropped.
	Finish(call, std::move(envelope));
}

json ResponseFor(const json& id, const json& envelope)
{
	json response = json::object();
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	if (envelope.contains("error")) response["error"] = envelope["error"];
	else response["result"] = envelope.value("result", json());
	return response;
}

ParsedLine ParseRequestLine(const std::string& line)
{
	ParsedLine out;
	auto fail = [&](int code, const std::string& message, const json& id) {
		out.ok = false;
		out.call.reset();
		out.errorResponse = ResponseFor(id, ErrorEnvelope(code, message));
		return out;
	};

	json doc = json::parse(line, nullptr, false);
	if (doc.is_discarded()) return fail(rpc::kParseError, "parse error: the line is not valid JSON", nullptr);
	if (doc.is_array()) return fail(rpc::kInvalidRequest, "batch requests are not supported", nullptr);
	if (!doc.is_object()) return fail(rpc::kInvalidRequest, "request must be a JSON object", nullptr);

	const json id = doc.contains("id") ? doc["id"] : json();
	if (!(id.is_null() || id.is_string() || id.is_number_integer())) {
		return fail(rpc::kInvalidRequest, "id must be a string, an integer or null", nullptr);
	}
	if (doc.contains("jsonrpc") && !(doc["jsonrpc"].is_string() && doc["jsonrpc"].get<std::string>() == "2.0")) {
		return fail(rpc::kInvalidRequest, "jsonrpc must be \"2.0\"", id);
	}
	if (!doc.contains("method") || !doc["method"].is_string()) {
		return fail(rpc::kInvalidRequest, "method must be a string", id);
	}

	json params = doc.contains("params") ? doc["params"] : json::object();
	if (params.is_null()) params = json::object();
	if (!params.is_object()) return fail(rpc::kInvalidParams, "params must be an object", id);

	auto call = std::make_shared<Call>();
	call->method = doc["method"].get<std::string>();
	call->params = std::move(params);
	call->isNotification = !doc.contains("id");
	call->id = id;
	out.ok = true;
	out.call = std::move(call);
	return out;
}

std::vector<std::string> TakeLines(std::string& buffer)
{
	std::vector<std::string> lines;
	size_t start = 0;
	for (;;) {
		const size_t nl = buffer.find('\n', start);
		if (nl == std::string::npos) break;
		std::string line = buffer.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) lines.push_back(std::move(line));
		start = nl + 1;
	}
	buffer.erase(0, start);
	return lines;
}

} // namespace automation
