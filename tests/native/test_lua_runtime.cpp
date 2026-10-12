// Lua runtime: sandbox, error containment, stack balance, budgets, memory limits (no GPU needed).
#include "TestFramework.h"

#include "scripting/LuaRuntime.h"

#include "lua.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>

namespace {
	class MemorySource : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const auto it = files.find(logicalPath);
			if (it == files.end()) {
				error = "no such file";
				return false;
			}
			text = it->second;
			return true;
		}
	};

	// Compiles and runs a chunk with no arguments; returns success and fills error.
	bool RunChunk(NoxLua::Runtime& rt, const std::string& text, const std::string& name, NoxLua::RunError& error) {
		int ref = 0;
		std::string compileError;
		if (!rt.Compile(text, name, ref, compileError)) {
			error.message = compileError;
			return false;
		}
		const bool ok = rt.CallRef(ref, nullptr, 0, &error);
		rt.Unref(ref);
		return ok;
	}

	NoxLua::RuntimeLimits DefaultLimits() {
		NoxLua::RuntimeLimits limits;
		limits.instructionBudget = 5000000;
		return limits;
	}
}

NOX_TEST(LuaRuntime, SandboxRemovesEscapes)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	NOX_REQUIRE(RunChunk(rt,
		"assert(io == nil and os == nil and debug == nil and package == nil)\n"
		"assert(dofile == nil and loadfile == nil)\n"
		"assert(string.dump == nil)\n"
		"assert(type(require) == 'function' and type(load) == 'function')\n"
		"assert(string.format and table.concat and math.floor and utf8.char and coroutine.create)\n",
		"sandbox_check.lua", error)) ;
	NOX_CHECK_EQ(rt.StackTop(), 0);
}

NOX_TEST(LuaRuntime, LoadRefusesBinaryAndEnvless)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	// Binary mode is refused, and so is a load without an explicit environment.
	NOX_REQUIRE(RunChunk(rt,
		"local ok, err = pcall(load, 'return 1', 'x', 'b', {})\n"
		"assert(not ok and string.find(err, 'text chunks'))\n"
		"local ok2, err2 = pcall(load, 'return 1')\n"
		"assert(not ok2 and string.find(err2, 'explicit environment'))\n"
		"local f = load('return x', 'envtest', 't', {x = 42})\n"
		"assert(f() == 42)\n",
		"load_check.lua", error));
}

NOX_TEST(LuaRuntime, NonStringErrorIsContained)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	NOX_CHECK(!RunChunk(rt, "error({code = 7})", "table_error.lua", error));
	NOX_CHECK(error.nonStringError);
	NOX_CHECK(error.message.find("table value") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);

	NoxLua::RunError numberError;
	NOX_CHECK(!RunChunk(rt, "error(nil)", "nil_error.lua", numberError));
	NOX_CHECK(numberError.nonStringError);
	NOX_CHECK_EQ(rt.StackTop(), 0);
}

NOX_TEST(LuaRuntime, TracebackNamesVirtualFileAndLine)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	const std::string text =
		"local function inner()\n"
		"  error('boom')\n"
		"end\n"
		"local function outer() inner() end\n"
		"outer()\n";
	NOX_CHECK(!RunChunk(rt, text, "projects/demo/scripts/main.lua", error));
	NOX_CHECK(error.message.find("projects/demo/scripts/main.lua:2: boom") != std::string::npos);
	NOX_CHECK(error.message.find("stack traceback:") != std::string::npos);
	NOX_CHECK(error.message.find("projects/demo/scripts/main.lua:4") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);
}

NOX_TEST(LuaRuntime, CompileErrorReportsVirtualLine)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	int ref = 0;
	std::string error;
	NOX_CHECK(!rt.Compile("local x = 1\nlocal y = = 2\n", "scripts/bad.lua", ref, error));
	NOX_CHECK(error.find("scripts/bad.lua:2:") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);
}

NOX_TEST(LuaRuntime, NestedRequireErrorAndCycles)
{
	MemorySource source;
	source.files["mods/a.lua"] = "local b = require('b')\nreturn {}\n";
	source.files["mods/b.lua"] = "local x = nil\nreturn x.field\n";
	source.files["mods/c.lua"] = "return require('d')\n";
	source.files["mods/d.lua"] = "return require('c')\n";
	source.files["mods/ok.lua"] = "return { value = 5 }\n";
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	rt.SetScriptRoots({ "mods" });

	NoxLua::RunError error;
	NOX_CHECK(!RunChunk(rt, "require('a')", "main.lua", error));
	NOX_CHECK(error.message.find("mods/b.lua:2:") != std::string::npos);
	NOX_CHECK(error.message.find("stack traceback:") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);

	// The failed module is not cached: requiring it again reports the same error, not a cycle.
	NoxLua::RunError again;
	NOX_CHECK(!RunChunk(rt, "require('b')", "main2.lua", again));
	NOX_CHECK(again.message.find("circular") == std::string::npos);
	NOX_CHECK(again.message.find("mods/b.lua") != std::string::npos);

	NoxLua::RunError cycle;
	NOX_CHECK(!RunChunk(rt, "require('c')", "main3.lua", cycle));
	NOX_CHECK(cycle.message.find("circular") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);

	NoxLua::RunError ok;
	NOX_REQUIRE(RunChunk(rt, "assert(require('ok').value == 5) assert(require('ok') == require('ok'))", "main4.lua", ok));
	NOX_CHECK(!RunChunk(rt, "require('missing.module')", "main5.lua", ok));
	NOX_CHECK(ok.message.find("not found") != std::string::npos);
	NOX_CHECK(!RunChunk(rt, "require('Bad..name')", "main6.lua", ok));
	NOX_CHECK(ok.message.find("invalid module name") != std::string::npos);
}

NOX_TEST(LuaRuntime, ModuleNameAndPathRules)
{
	NOX_CHECK(NoxLua::Runtime::IsValidModuleName("vehicle.nitrous"));
	NOX_CHECK(NoxLua::Runtime::IsValidModuleName("a_1.b2"));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName(""));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName("a..b"));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName(".a"));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName("a/b"));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName("1a"));
	NOX_CHECK(!NoxLua::Runtime::IsValidModuleName("../etc"));
	NOX_CHECK_EQ(NoxLua::Runtime::ModulePath("projects/x/scripts", "vehicle.nitrous"), std::string("projects/x/scripts/vehicle/nitrous.lua"));
}

NOX_TEST(LuaRuntime, StackBalancedOver10kCalls)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	int ref = 0;
	std::string compileError;
	NOX_REQUIRE(rt.Compile("return ...", "identity.lua", ref, compileError));
	const int before = rt.StackTop();
	for (int i = 0; i < 10000; ++i) {
		// Success with results, failure with an error, and no results: the stack must not drift.
		NoxLua::RunError runError;
		const bool ok = rt.CallRef(ref, [i](lua_State* L) { lua_pushinteger(L, i); return 1; }, 1, &runError);
		NOX_REQUIRE(ok);
		lua_pop(rt.State(), 1);
		NOX_REQUIRE(rt.StackTop() == before);
		if (i % 100 == 0) {
			int failRef = 0;
			NOX_REQUIRE(rt.Compile("error('x')", "fail.lua", failRef, compileError));
			NoxLua::RunError failure;
			NOX_CHECK(!rt.CallRef(failRef, nullptr, 0, &failure));
			rt.Unref(failRef);
			NOX_REQUIRE(rt.StackTop() == before);
		}
	}
	rt.Unref(ref);
	NOX_CHECK_EQ(rt.StackTop(), before);
}

NOX_TEST(LuaRuntime, InstructionBudgetStopsInfiniteLoop)
{
	MemorySource source;
	NoxLua::RuntimeLimits limits;
	limits.instructionBudget = 300000;
	NoxLua::Runtime rt(limits, &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	NOX_CHECK(!RunChunk(rt, "while true do end", "spin.lua", error));
	NOX_CHECK(error.budgetExceeded);
	NOX_CHECK(error.message.find("instruction budget") != std::string::npos);
	NOX_CHECK_EQ(rt.StackTop(), 0);

	// The next call gets a fresh budget and runs normally.
	NoxLua::RunError ok;
	NOX_CHECK(RunChunk(rt, "local s = 0 for i = 1, 1000 do s = s + i end assert(s == 500500)", "fine.lua", ok));
	NOX_CHECK(!ok.budgetExceeded);
}

NOX_TEST(LuaRuntime, MemoryLimitIsCleanOom)
{
	MemorySource source;
	NoxLua::RuntimeLimits limits;
	limits.memoryBytes = 512 * 1024;
	limits.instructionBudget = 50000000;
	NoxLua::Runtime rt(limits, &source);
	NOX_REQUIRE(rt.Ok());
	NoxLua::RunError error;
	NOX_CHECK(!RunChunk(rt, "local t = {} for i = 1, 1000000 do t[i] = i .. 'x' end", "grow.lua", error));
	NOX_CHECK(error.memoryExceeded);
	NOX_CHECK(rt.AllocationFailures() > 0);
	NOX_CHECK_EQ(rt.StackTop(), 0);
	NOX_CHECK(rt.MemoryUsed() <= limits.memoryBytes);
	// After the failure the VM still runs small code.
	NoxLua::RunError after;
	NOX_CHECK(RunChunk(rt, "return 1 + 1", "after.lua", after));
}

NOX_TEST(LuaRuntime, StateCreationFailsCleanlyWhenLimitTooSmall)
{
	MemorySource source;
	NoxLua::RuntimeLimits limits;
	limits.memoryBytes = 1024;
	NoxLua::Runtime rt(limits, &source);
	NOX_CHECK(!rt.Ok());
	NOX_CHECK(!rt.InitError().empty());
	NoxLua::RunError error;
	NOX_CHECK(!rt.CallRef(0, nullptr, 0, &error));
}

NOX_TEST(LuaRuntime, PrintUsesLogSinkAndMemoryAccounts)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	std::string captured;
	rt.SetLogSink([&captured](const std::string& line) { captured += line; });
	NoxLua::RunError error;
	NOX_REQUIRE(RunChunk(rt, "print('a', 2, true)", "print.lua", error));
	NOX_CHECK_EQ(captured, std::string("a\t2\ttrue"));
	NOX_CHECK(rt.MemoryUsed() > 0);
	NOX_CHECK(rt.MemoryPeak() >= rt.MemoryUsed());
}

// Micro-benchmark: cost of a protected native-to-Lua call (recorded in the Lua report).
NOX_TEST(LuaRuntime, CallOverheadMicroBenchmark)
{
	MemorySource source;
	NoxLua::Runtime rt(DefaultLimits(), &source);
	NOX_REQUIRE(rt.Ok());
	int ref = 0;
	std::string compileError;
	NOX_REQUIRE(rt.Compile("local n = ... return n + 1", "bench.lua", ref, compileError));
	const int calls = 200000;
	const auto start = std::chrono::steady_clock::now();
	long long checksum = 0;
	for (int i = 0; i < calls; ++i) {
		NoxLua::RunError error;
		NOX_REQUIRE(rt.CallRef(ref, [i](lua_State* L) { lua_pushinteger(L, i); return 1; }, 1, &error));
		checksum += lua_tointeger(rt.State(), -1);
		lua_pop(rt.State(), 1);
	}
	const auto end = std::chrono::steady_clock::now();
	const double us = std::chrono::duration<double, std::micro>(end - start).count();
	std::printf("    [bench] protected call (push arg, 1 result): %.1f ns/call over %d calls (checksum %lld), VM memory %zu bytes, peak %zu\n",
		us * 1000.0 / calls, calls, checksum, rt.MemoryUsed(), rt.MemoryPeak());
	rt.Unref(ref);
	NOX_CHECK_EQ(rt.StackTop(), 0);
}
