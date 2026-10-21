// Automation server, headless part: JSON-RPC line validation, dispatch and envelopes, line framing and the
// command-line / environment options. The socket thread and engine commands need a running engine and are
// covered by tools/nox_mcp/test_e2e.py.
#include "TestFramework.h"

#include "automation/AutomationProtocol.h"
#include "automation/AutomationServer.h"

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

using namespace automation;

namespace {
	int CodeOf(const json& response) { return response["error"]["code"].get<int>(); }

	// Parses one line, runs it through the dispatcher and returns the complete response object.
	json RoundTrip(Dispatcher& dispatcher, const std::string& line) {
		ParsedLine parsed = ParseRequestLine(line);
		if (!parsed.ok) return parsed.errorResponse;
		std::future<json> future = parsed.call->reply.get_future();
		dispatcher.Run(parsed.call, nullptr);
		return ResponseFor(parsed.call->id, future.get());
	}
}

NOX_TEST(Automation, ValidRequestGetsDefaultParamsAndId)
{
	ParsedLine parsed = ParseRequestLine(R"({"jsonrpc":"2.0","id":7,"method":"engine.info"})");
	NOX_REQUIRE(parsed.ok);
	NOX_CHECK_EQ(parsed.call->method, std::string("engine.info"));
	NOX_CHECK(parsed.call->params.is_object());
	NOX_CHECK(!parsed.call->isNotification);
	NOX_CHECK_EQ(parsed.call->id.get<int>(), 7);
}

NOX_TEST(Automation, RequestWithoutIdIsNotification)
{
	ParsedLine parsed = ParseRequestLine(R"({"jsonrpc":"2.0","method":"engine.pause","params":{}})");
	NOX_REQUIRE(parsed.ok);
	NOX_CHECK(parsed.call->isNotification);
	NOX_CHECK(parsed.call->id.is_null());
}

NOX_TEST(Automation, MalformedInputGetsProtocolErrorsNeverExceptions)
{
	// Not JSON at all.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine("{not json").errorResponse), rpc::kParseError);
	// Batch arrays are not supported.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"engine.info\"}]").errorResponse), rpc::kInvalidRequest);
	// Scalars are not requests.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine("42").errorResponse), rpc::kInvalidRequest);
	// Missing or non-string method.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine(R"({"jsonrpc":"2.0","id":1})").errorResponse), rpc::kInvalidRequest);
	NOX_CHECK_EQ(CodeOf(ParseRequestLine(R"({"jsonrpc":"2.0","id":1,"method":5})").errorResponse), rpc::kInvalidRequest);
	// Wrong protocol version.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine(R"({"jsonrpc":"1.0","id":1,"method":"engine.info"})").errorResponse), rpc::kInvalidRequest);
	// Params must be an object.
	NOX_CHECK_EQ(CodeOf(ParseRequestLine(R"({"jsonrpc":"2.0","id":1,"method":"x","params":[1,2]})").errorResponse), rpc::kInvalidParams);
	// Fractional ids are rejected (ids are strings, integers or null).
	NOX_CHECK_EQ(CodeOf(ParseRequestLine(R"({"jsonrpc":"2.0","id":1.5,"method":"x"})").errorResponse), rpc::kInvalidRequest);
	// The error response still echoes the id when it is known.
	NOX_CHECK_EQ(ParseRequestLine(R"({"jsonrpc":"2.0","id":"abc","method":5})").errorResponse["id"].get<std::string>(), std::string("abc"));
}

NOX_TEST(Automation, DispatcherRoutesResultsAndErrors)
{
	Dispatcher dispatcher;
	dispatcher.Register("math.add", [](Invocation& inv) {
		if (!inv.params.contains("a") || !inv.params.contains("b")) throw RpcError{ rpc::kInvalidParams, "need a and b", nullptr };
		return json(inv.params["a"].get<int>() + inv.params["b"].get<int>());
	});
	dispatcher.Register("boom.std", [](Invocation&) -> json { throw std::runtime_error("kaboom"); });
	dispatcher.Register("boom.unknown", [](Invocation&) -> json { throw 42; });

	json ok = RoundTrip(dispatcher, R"({"jsonrpc":"2.0","id":1,"method":"math.add","params":{"a":2,"b":3}})");
	NOX_CHECK_EQ(ok["result"].get<int>(), 5);
	NOX_CHECK_EQ(ok["jsonrpc"].get<std::string>(), std::string("2.0"));

	NOX_CHECK_EQ(CodeOf(RoundTrip(dispatcher, R"({"jsonrpc":"2.0","id":2,"method":"math.add","params":{}})")), rpc::kInvalidParams);
	NOX_CHECK_EQ(CodeOf(RoundTrip(dispatcher, R"({"jsonrpc":"2.0","id":3,"method":"no.such"})")), rpc::kMethodNotFound);
	NOX_CHECK_EQ(CodeOf(RoundTrip(dispatcher, R"({"jsonrpc":"2.0","id":4,"method":"boom.std"})")), rpc::kInternalError);
	NOX_CHECK_EQ(CodeOf(RoundTrip(dispatcher, R"({"jsonrpc":"2.0","id":5,"method":"boom.unknown"})")), rpc::kInternalError);
}

NOX_TEST(Automation, DeferredCallsAreAnsweredOnlyByFinish)
{
	Dispatcher dispatcher;
	dispatcher.Register("later", [](Invocation& inv) -> json {
		inv.Defer();
		return json();
	});
	ParsedLine parsed = ParseRequestLine(R"({"jsonrpc":"2.0","id":9,"method":"later"})");
	NOX_REQUIRE(parsed.ok);
	std::future<json> future = parsed.call->reply.get_future();
	dispatcher.Run(parsed.call, nullptr);
	NOX_CHECK(future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

	Finish(parsed.call, ResultEnvelope(json{ { "frame", 3 } }));
	NOX_REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
	NOX_CHECK_EQ(future.get()["result"]["frame"].get<int>(), 3);

	// A second finish is ignored (the promise is set once).
	Finish(parsed.call, ErrorEnvelope(rpc::kEngineError, "late"));
}

NOX_TEST(Automation, AbandonedCallsAreSkipped)
{
	int runs = 0;
	Dispatcher dispatcher;
	dispatcher.Register("count", [&runs](Invocation&) {
		++runs;
		return json();
	});
	ParsedLine parsed = ParseRequestLine(R"({"jsonrpc":"2.0","id":1,"method":"count"})");
	NOX_REQUIRE(parsed.ok);
	parsed.call->abandoned = true;
	dispatcher.Run(parsed.call, nullptr);
	NOX_CHECK_EQ(runs, 0);
}

NOX_TEST(Automation, ResponsesCarryExactlyOneOfResultOrError)
{
	json success = ResponseFor(json(4), ResultEnvelope(json{ { "x", 1 } }));
	NOX_CHECK(success.contains("result"));
	NOX_CHECK(!success.contains("error"));
	json failure = ResponseFor(json(4), ErrorEnvelope(rpc::kEngineError, "nope", json{ { "why", "test" } }));
	NOX_CHECK(!failure.contains("result"));
	NOX_CHECK_EQ(failure["error"]["message"].get<std::string>(), std::string("nope"));
	NOX_CHECK_EQ(failure["error"]["data"]["why"].get<std::string>(), std::string("test"));
}

NOX_TEST(Automation, TakeLinesSplitsOnNewlinesAndKeepsPartialTail)
{
	std::string buffer = "{\"a\":1}\r\n\n{\"b\":2}\n{\"c\"";
	std::vector<std::string> lines = TakeLines(buffer);
	NOX_REQUIRE(lines.size() == 2);
	NOX_CHECK_EQ(lines[0], std::string("{\"a\":1}"));
	NOX_CHECK_EQ(lines[1], std::string("{\"b\":2}"));
	NOX_CHECK_EQ(buffer, std::string("{\"c\""));

	buffer += ":3}\n";
	lines = TakeLines(buffer);
	NOX_REQUIRE(lines.size() == 1);
	NOX_CHECK_EQ(lines[0], std::string("{\"c\":3}"));
	NOX_CHECK(buffer.empty());
}

NOX_TEST(Automation, OptionsOffByDefaultAndPortFromFlagOrEnvironment)
{
	char bare[] = "Nox_Engine.exe";
	char* plain[] = { bare };
	Options off = ParseOptions(1, plain, nullptr);
	NOX_CHECK(!off.enabled);
	NOX_CHECK(off.error.empty());

	char flag[] = "--automation";
	char* withFlag[] = { bare, flag };
	Options defaults = ParseOptions(2, withFlag, nullptr);
	NOX_CHECK(defaults.enabled);
	NOX_CHECK_EQ(defaults.port, kDefaultPort);
	NOX_CHECK_EQ(kDefaultPort, 47800);

	char flagPort[] = "--automation=5555";
	char* withPort[] = { bare, flagPort };
	NOX_CHECK_EQ(ParseOptions(2, withPort, nullptr).port, 5555);

	// The environment variable enables the server; a flag port wins over it.
	NOX_CHECK(ParseOptions(1, plain, "6001").enabled);
	NOX_CHECK_EQ(ParseOptions(1, plain, "6001").port, 6001);
	NOX_CHECK_EQ(ParseOptions(2, withPort, "6001").port, 5555);
	NOX_CHECK(!ParseOptions(1, plain, "").enabled);
}

NOX_TEST(Automation, MalformedPortsKeepTheServerOff)
{
	char bare[] = "Nox_Engine.exe";
	char* plain[] = { bare };
	for (const char* bad : { "abc", "0", "70000", "12x", "-1" }) {
		std::string arg = std::string("--automation=") + bad;
		std::vector<char> storage(arg.begin(), arg.end());
		storage.push_back('\0');
		char* argv[] = { bare, storage.data() };
		Options o = ParseOptions(2, argv, nullptr);
		NOX_CHECK(!o.enabled);
		NOX_CHECK(!o.error.empty());
	}
	NOX_CHECK(!ParseOptions(1, plain, "99999").enabled);
}
