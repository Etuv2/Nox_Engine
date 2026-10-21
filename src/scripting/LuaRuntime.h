#pragma once
// Owned Lua 5.5 state with a sandbox, a memory cap, an instruction budget and protected calls.
//
// Error model (see docs/lua_api.md): Lua is compiled as C++ (external/lua-5.5.1 under nox_lua), so
// lua_error unwinds with C++ exceptions and destructors run. Every entry into Lua goes through
// CallRef (lua_pcall with a traceback handler); the stack height is restored on every path.
// Allocation failure inside Lua surfaces as LUA_ERRMEM, which is a normal, reported call error.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace NoxLua {

	// Source of Lua module text, addressed by logical path ("projects/x/scripts/vehicle/nitrous.lua").
	// LuaProject.cpp implements this over AssetIO; tests implement it over memory.
	class ScriptSource {
	public:
		virtual ~ScriptSource() = default;
		// False with a reason when the path does not exist or cannot be read.
		virtual bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) = 0;
	};

	struct RuntimeLimits {
		// Total bytes allocated by this VM (Lua objects, strings, tables). Exceeding it is a clean OOM.
		size_t memoryBytes = 32u * 1024u * 1024u;
		// VM instructions allowed per protected call (checked every kHookInterval instructions).
		uint64_t instructionBudget = 2000000;
	};

	// Failure details of one protected call. message includes the Lua location prefix ("file:line:")
	// when the error came from Lua code, and the traceback after the first line.
	// A module that the VM read through ScriptSource (entry or require). Hash is FNV-1a 64 of the text.
	struct ModuleRecord {
		std::string path;
		uint64_t hash = 0;
		size_t size = 0;
	};

	struct RunError {
		std::string message;
		bool budgetExceeded = false;
		bool memoryExceeded = false;   // LUA_ERRMEM
		bool nonStringError = false;   // error(value) where value is not a string
	};

	class Runtime {
	public:
		using LogSink = std::function<void(const std::string&)>;

		Runtime(const RuntimeLimits& limits, ScriptSource* source);
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		bool Ok() const { return m_state != nullptr; }
		const std::string& InitError() const { return m_initError; }
		lua_State* State() const { return m_state; }

		// Logical directories searched by require, in order ("projects/x/scripts").
		void SetScriptRoots(std::vector<std::string> roots) { m_scriptRoots = std::move(roots); }
		const std::vector<std::string>& ScriptRoots() const { return m_scriptRoots; }
		void SetLogSink(LogSink sink) { m_log = std::move(sink); }
		const RuntimeLimits& Limits() const { return m_limits; }

		// Compiles text (mode "t" only) and returns a registry reference. chunkName is a logical path;
		// diagnostics show it as the file name. Returns false with the compiler message on error.
		bool Compile(std::string_view text, const std::string& chunkName, int& outRef, std::string& error);
		void Unref(int ref);

		// Calls the registry function `ref`. pushArgs pushes its arguments and returns their count; it
		// runs inside the protected region, so its allocation failures are reported, not thrown past
		// the caller. On success exactly `nresults` values are left on the stack; on failure none are.
		// Each call gets a fresh instruction budget.
		bool CallRef(int ref, const std::function<int(lua_State*)>& pushArgs, int nresults, RunError* error);

		// Protected call of a C function with one lightuserdata argument (installers, bootstrap helpers).
		bool ProtectedCall(int (*fn)(lua_State*), void* arg, RunError* error);

		// Resumes a coroutine thread created on this state (LuaWorld tasks). Same instruction budget as CallRef.
		// True when the thread yielded (nresults values on its stack) or finished; on error the failure is in *error
		// with the coroutine's traceback, and the thread is dead.
		bool Resume(lua_State* thread, int nargs, int& nresults, RunError* error);

		// Modules read so far, in first-read order (the set a reload must stage).
		const std::vector<ModuleRecord>& LoadedModules() const { return m_modules; }
		// Process-wide count of live runtimes (leak checks).
		static int LiveCount();

		// Stack height of the state (used by tests and assertions).
		
		int StackTop() const;

		// Memory accounting.
		size_t MemoryUsed() const { return m_alloc.used; }
		size_t MemoryPeak() const { return m_alloc.peak; }
		size_t AllocationFailures() const { return m_alloc.failures; }
		uint64_t LastCallInstructions() const { return m_lastCallInstructions; }

		// Module search (used by the require implementation; exposed for tests).
		static bool IsValidModuleName(std::string_view name);
		static std::string ModulePath(const std::string& root, std::string_view name);

		// Internal hooks used by the C entry points in LuaRuntime.cpp.
		bool ReadSource(const std::string& logicalPath, std::string& text, std::string& error);
		void Print(const std::string& line);
		// nox.log_raw: the line as written, without the "[Lua] " prefix (same sink as Print).
		void PrintRaw(const std::string& line);

	private:
		struct Allocator {
			size_t used = 0;
			size_t peak = 0;
			size_t limit = 0;
			size_t failures = 0;
		};

		static void* LimitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize);
		static int PanicHandler(lua_State* L);
		static void CountHook(lua_State* L, lua_Debug* ar);

		lua_State* m_state = nullptr;
		Allocator m_alloc;
		RuntimeLimits m_limits;
		ScriptSource* m_source = nullptr;
		std::vector<std::string> m_scriptRoots;
		std::vector<ModuleRecord> m_modules;
		LogSink m_log;
		std::string m_initError;

		// Instruction budget of the call in progress.
		int64_t m_budgetLeft = 0;
		bool m_budgetTripped = false;
		bool m_budgetArmed = false;
		uint64_t m_lastCallInstructions = 0;
		uint64_t m_instructionsThisCall = 0;
	};

	// Traceback message handler (exposed for the binding layer's own protected calls).
	int TracebackHandler(lua_State* L);
}
