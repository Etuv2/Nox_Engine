#include "LuaRuntime.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>

namespace NoxLua {
	namespace {
		std::atomic<int> s_liveRuntimes{ 0 };
		constexpr int kHookInterval = 1000;   // instructions between budget checks
		constexpr const char* kLoadedKey = "nox.loaded";
		constexpr const char* kLoadingKey = "nox.loading";
		constexpr const char* kErrorObjectPrefix = "(error object is a ";

		Runtime* RuntimeOf(lua_State* L) {
			return *static_cast<Runtime**>(lua_getextraspace(L));
		}

		// Body of one protected call: pushes the function from the registry and its arguments, then calls
		// it. Running argument pushes inside pcall keeps allocation failures on the reported path.
		struct CallThunk {
			const std::function<int(lua_State*)>* pushArgs;
			int ref;
			int nresults;
		};

		int ProtectedInvoke(lua_State* L) {
			auto* thunk = static_cast<CallThunk*>(lua_touserdata(L, 1));
			lua_rawgeti(L, LUA_REGISTRYINDEX, thunk->ref);
			const int nargs = (thunk->pushArgs && *thunk->pushArgs) ? (*thunk->pushArgs)(L) : 0;
			lua_call(L, nargs, thunk->nresults);
			return thunk->nresults;
		}

		struct CompileThunk {
			const char* text;
			size_t length;
			const char* chunk;
			int ref;
		};

		int CompileInvoke(lua_State* L) {
			auto* thunk = static_cast<CompileThunk*>(lua_touserdata(L, 1));
			if (luaL_loadbufferx(L, thunk->text, thunk->length, thunk->chunk, "t") != LUA_OK) {
				return lua_error(L);   // message is the compiler diagnostic
			}
			thunk->ref = luaL_ref(L, LUA_REGISTRYINDEX);
			return 0;
		}

		// print(...): writes through the runtime's log sink.
		int L_Print(lua_State* L) {
			const int n = lua_gettop(L);
			std::string line;
			for (int i = 1; i <= n; ++i) {
				if (i > 1) line += '\t';
				size_t len = 0;
				const char* s = luaL_tolstring(L, i, &len);
				line.append(s, len);
				lua_pop(L, 1);
			}
			RuntimeOf(L)->Print(line);
			return 0;
		}

		// load(chunk [, chunkname [, mode [, env]]]): text chunks only, string input only, explicit env.
		int L_Load(lua_State* L) {
			size_t length = 0;
			const char* text = luaL_checklstring(L, 1, &length);
			const char* name = luaL_optstring(L, 2, "=(load)");
			const char* mode = luaL_optstring(L, 3, "t");
			if (std::strcmp(mode, "t") != 0) {
				return luaL_error(L, "load: only text chunks are allowed (mode '%s' refused)", mode);
			}
			if (lua_isnoneornil(L, 4)) {
				return luaL_error(L, "load: an explicit environment table is required");
			}
			luaL_checktype(L, 4, LUA_TTABLE);
			if (luaL_loadbufferx(L, text, length, name, "t") != LUA_OK) {
				lua_pushnil(L);
				lua_insert(L, -2);
				return 2;
			}
			lua_pushvalue(L, 4);
			lua_setupvalue(L, -2, 1);   // first upvalue of a main chunk is _ENV
			return 1;
		}

		// require(name): Lua-only modules from the project's script roots, through ScriptSource.
		int L_Require(lua_State* L) {
			Runtime* rt = RuntimeOf(L);
			size_t nameLength = 0;
			const char* cname = luaL_checklstring(L, 1, &nameLength);
			const std::string name(cname, nameLength);
			if (!Runtime::IsValidModuleName(name)) {
				return luaL_error(L, "require: invalid module name '%s'", name.c_str());
			}

			// Stack: 1 = name, 2 = loaded table, 3 = loading table.
			luaL_getsubtable(L, LUA_REGISTRYINDEX, kLoadedKey);
			lua_getfield(L, 2, name.c_str());
			if (!lua_isnil(L, -1)) return 1;
			lua_pop(L, 1);
			luaL_getsubtable(L, LUA_REGISTRYINDEX, kLoadingKey);
			lua_getfield(L, 3, name.c_str());
			if (!lua_isnil(L, -1)) {
				lua_pop(L, 1);
				return luaL_error(L, "require: circular require of '%s'", name.c_str());
			}
			lua_pop(L, 1);
			lua_pushboolean(L, 1);
			lua_setfield(L, 3, name.c_str());

			auto clearLoading = [&]() {
				lua_pushnil(L);
				lua_setfield(L, 3, name.c_str());
			};

			std::string text;
			std::string logical;
			std::string searched;
			bool found = false;
			for (const std::string& root : rt->ScriptRoots()) {
				logical = Runtime::ModulePath(root, name);
				std::string readError;
				if (rt->ReadSource(logical, text, readError)) {
					found = true;
					break;
				}
				searched += "\n\tno file '" + logical + "'";
			}
			if (!found) {
				clearLoading();
				return luaL_error(L, "module '%s' not found:%s", name.c_str(), searched.c_str());
			}

			int ref = 0;
			std::string compileError;
			if (!rt->Compile(text, logical, ref, compileError)) {
				clearLoading();
				return luaL_error(L, "%s", compileError.c_str());
			}

			// Run the module body under a traceback handler; a failure keeps its traceback and is
			// re-raised after the loading marker is cleared (nested require errors stay diagnosable).
			lua_pushcfunction(L, TracebackHandler);
			const int handler = lua_gettop(L);
			lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
			rt->Unref(ref);
			const int status = lua_pcall(L, 0, 1, handler);
			lua_remove(L, handler);
			if (status != LUA_OK) {
				clearLoading();
				return lua_error(L);
			}
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				lua_pushboolean(L, 1);
			}
			lua_pushvalue(L, -1);
			lua_setfield(L, 2, name.c_str());
			clearLoading();
			return 1;
		}

		// Runs inside lua_pcall during construction: opens the allowed libraries and removes escapes.
		int OpenSandbox(lua_State* L) {
			Runtime* rt = static_cast<Runtime*>(lua_touserdata(L, 1));
			luaL_requiref(L, "_G", luaopen_base, 1);
			lua_pop(L, 1);
			luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
			lua_pop(L, 1);
			luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
			lua_pop(L, 1);
			luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
			lua_pop(L, 1);
			luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
			lua_pop(L, 1);
			luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
			lua_pop(L, 1);

			// Base escapes: no filesystem loading, no binary chunks, no bytecode dumping.
			lua_pushnil(L);
			lua_setglobal(L, "dofile");
			lua_pushnil(L);
			lua_setglobal(L, "loadfile");
			lua_getglobal(L, LUA_STRLIBNAME);
			lua_pushnil(L);
			lua_setfield(L, -2, "dump");
			lua_pop(L, 1);

			lua_pushcfunction(L, L_Load);
			lua_setglobal(L, "load");
			lua_pushlightuserdata(L, rt);
			lua_pushcclosure(L, L_Print, 1);
			lua_setglobal(L, "print");
			lua_pushlightuserdata(L, rt);
			lua_pushcclosure(L, L_Require, 1);
			lua_setglobal(L, "require");

			luaL_getsubtable(L, LUA_REGISTRYINDEX, kLoadedKey);
			lua_pop(L, 1);
			luaL_getsubtable(L, LUA_REGISTRYINDEX, kLoadingKey);
			lua_pop(L, 1);
			return 0;
		}
	}

	int TracebackHandler(lua_State* L) {
		if (lua_type(L, 1) != LUA_TSTRING && lua_type(L, 1) != LUA_TNUMBER) {
			lua_pushfstring(L, "%s%s value)", kErrorObjectPrefix, luaL_typename(L, 1));
			lua_replace(L, 1);
		}
		const char* msg = lua_tostring(L, 1);
		if (msg && std::strstr(msg, "stack traceback:")) return 1;   // already annotated
		luaL_traceback(L, L, msg ? msg : "(error)", 1);
		return 1;
	}

	bool Runtime::IsValidModuleName(std::string_view name) {
		if (name.empty()) return false;
		bool segmentStart = true;
		for (char c : name) {
			if (c == '.') {
				if (segmentStart) return false;
				segmentStart = true;
				continue;
			}
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
			const bool digit = c >= '0' && c <= '9';
			if (segmentStart) {
				if (!alpha) return false;
				segmentStart = false;
			}
			else if (!alpha && !digit) {
				return false;
			}
		}
		return !segmentStart;
	}

	std::string Runtime::ModulePath(const std::string& root, std::string_view name) {
		std::string path = root.empty() ? std::string() : root + "/";
		for (char c : name) path += (c == '.') ? '/' : c;
		path += ".lua";
		return path;
	}

	void* Runtime::LimitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
		auto* alloc = static_cast<Allocator*>(ud);
		if (nsize == 0) {
			// With ptr == NULL, osize is a type tag, not a size: only account for real blocks.
			if (ptr) alloc->used -= osize;
			std::free(ptr);
			return nullptr;
		}
		const size_t oldSize = ptr ? osize : 0;
		const size_t newUsed = alloc->used - oldSize + nsize;
		if (newUsed > alloc->limit) {
			++alloc->failures;
			return nullptr;   // Lua turns this into LUA_ERRMEM
		}
		void* block = std::realloc(ptr, nsize);
		if (!block) {
			++alloc->failures;
			return nullptr;
		}
		alloc->used = newUsed;
		if (alloc->used > alloc->peak) alloc->peak = alloc->used;
		return block;
	}

	int Runtime::PanicHandler(lua_State* L) {
		// Every entry is protected, so this only sees failures outside CallRef/Compile/construction.
		const char* msg = lua_tostring(L, -1);
		std::fprintf(stderr, "[Lua] unprotected error: %s\n", msg ? msg : "(non-string)");
		return 0;   // Lua then aborts
	}

	void Runtime::CountHook(lua_State* L, lua_Debug*) {
		Runtime* rt = RuntimeOf(L);
		if (!rt->m_budgetArmed) return;   // between calls: no budget
		rt->m_instructionsThisCall += kHookInterval;
		rt->m_budgetLeft -= kHookInterval;
		if (rt->m_budgetLeft < 0) {
			rt->m_budgetTripped = true;
			luaL_error(L, "instruction budget exceeded (more than %llu instructions in one call)",
				static_cast<unsigned long long>(rt->m_limits.instructionBudget));
		}
	}

	Runtime::Runtime(const RuntimeLimits& limits, ScriptSource* source)
		: m_limits(limits)
		, m_source(source) {
		++s_liveRuntimes;
		m_alloc.limit = limits.memoryBytes;
		// Fixed seed: string hashing (and therefore table iteration order) is repeatable across runs.
		m_state = lua_newstate(LimitedAlloc, &m_alloc, 0x4E4F5821u);
		if (!m_state) {
			m_initError = "could not allocate the Lua state within the memory limit";
			return;
		}
		*static_cast<Runtime**>(lua_getextraspace(m_state)) = this;
		lua_atpanic(m_state, PanicHandler);

		lua_pushcfunction(m_state, OpenSandbox);
		lua_pushlightuserdata(m_state, this);
		if (lua_pcall(m_state, 1, 0, 0) != LUA_OK) {
			const char* msg = lua_tostring(m_state, -1);
			m_initError = std::string("sandbox setup failed: ") + (msg ? msg : "unknown error");
			lua_close(m_state);
			m_state = nullptr;
			return;
		}
		// Installed once on the main thread: coroutines copy the hook when they are created, so they
		// are budgeted too. CountHook ignores instructions outside CallRef (m_budgetArmed).
		lua_sethook(m_state, CountHook, LUA_MASKCOUNT, kHookInterval);
	}

	Runtime::~Runtime() {
		if (m_state) lua_close(m_state);
		--s_liveRuntimes;
	}

	int Runtime::LiveCount() {
		return s_liveRuntimes.load();
	}

	bool Runtime::Compile(std::string_view text, const std::string& chunkName, int& outRef, std::string& error) {
		if (!m_state) {
			error = m_initError;
			return false;
		}
		const std::string chunk = "@" + chunkName;
		CompileThunk thunk{ text.data(), text.size(), chunk.c_str(), LUA_NOREF };
		lua_State* L = m_state;
		const int base = lua_gettop(L);
		lua_pushcfunction(L, CompileInvoke);
		lua_pushlightuserdata(L, &thunk);
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			const char* msg = lua_tostring(L, -1);
			error = msg ? msg : "compile error";
			lua_settop(L, base);
			return false;
		}
		outRef = thunk.ref;
		lua_settop(L, base);
		return true;
	}

	void Runtime::Unref(int ref) {
		if (m_state && ref != LUA_NOREF && ref != LUA_REFNIL) luaL_unref(m_state, LUA_REGISTRYINDEX, ref);
	}

	bool Runtime::CallRef(int ref, const std::function<int(lua_State*)>& pushArgs, int nresults, RunError* error) {
		if (!m_state) {
			if (error) error->message = m_initError;
			return false;
		}
		lua_State* L = m_state;
		const int base = lua_gettop(L);
		CallThunk thunk{ &pushArgs, ref, nresults };

		lua_pushcfunction(L, TracebackHandler);
		const int handler = lua_gettop(L);
		lua_pushcfunction(L, ProtectedInvoke);
		lua_pushlightuserdata(L, &thunk);

		m_budgetLeft = static_cast<int64_t>(m_limits.instructionBudget);
		m_budgetTripped = false;
		m_instructionsThisCall = 0;
		m_budgetArmed = true;
		const int status = lua_pcall(L, 1, nresults, handler);
		m_budgetArmed = false;
		m_lastCallInstructions = m_instructionsThisCall;

		if (status == LUA_OK) {
			lua_remove(L, handler);
			return true;
		}
		if (error) {
			const char* msg = lua_tostring(L, -1);
			error->message = msg ? msg : "(error)";
			error->budgetExceeded = m_budgetTripped;
			error->memoryExceeded = (status == LUA_ERRMEM);
			error->nonStringError = error->message.compare(0, std::strlen(kErrorObjectPrefix), kErrorObjectPrefix) == 0;
		}
		lua_settop(L, base);
		return false;
	}

	bool Runtime::ProtectedCall(int (*fn)(lua_State*), void* arg, RunError* error) {
		if (!m_state) {
			if (error) error->message = m_initError;
			return false;
		}
		lua_State* L = m_state;
		const int base = lua_gettop(L);
		lua_pushcfunction(L, TracebackHandler);
		const int handler = lua_gettop(L);
		lua_pushcfunction(L, fn);
		lua_pushlightuserdata(L, arg);
		m_budgetLeft = static_cast<int64_t>(m_limits.instructionBudget);
		m_budgetTripped = false;
		m_instructionsThisCall = 0;
		m_budgetArmed = true;
		const int status = lua_pcall(L, 1, 0, handler);
		m_budgetArmed = false;
		if (status == LUA_OK) {
			lua_settop(L, base);
			return true;
		}
		if (error) {
			const char* msg = lua_tostring(L, -1);
			error->message = msg ? msg : "(error)";
			error->budgetExceeded = m_budgetTripped;
			error->memoryExceeded = (status == LUA_ERRMEM);
			error->nonStringError = error->message.compare(0, std::strlen(kErrorObjectPrefix), kErrorObjectPrefix) == 0;
		}
		lua_settop(L, base);
		return false;
	}

	int Runtime::StackTop() const {
		return m_state ? lua_gettop(m_state) : 0;
	}

	bool Runtime::ReadSource(const std::string& logicalPath, std::string& text, std::string& error) {
		if (!m_source) {
			error = "no script source";
			return false;
		}
		if (!m_source->ReadText(logicalPath, text, error)) return false;
		bool known = false;
		for (const ModuleRecord& m : m_modules) known = known || m.path == logicalPath;
		if (!known) {
			ModuleRecord record;
			record.path = logicalPath;
			record.size = text.size();
			uint64_t hash = 1469598103934665603ull;   // FNV-1a 64
			for (unsigned char c : text) {
				hash ^= c;
				hash *= 1099511628211ull;
			}
			record.hash = hash;
			m_modules.push_back(record);
		}
		return true;
	}

	void Runtime::Print(const std::string& line) {
		if (m_log) {
			m_log(line);
		}
		else {
			std::cout << "[Lua] " << line << std::endl;
		}
	}

	void Runtime::PrintRaw(const std::string& line) {
		if (m_log) {
			m_log(line);
		}
		else {
			std::cout << line << std::endl;
		}
	}
}
