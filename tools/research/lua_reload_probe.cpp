// Headless research fixture, not a NOX runtime implementation.
// Links against the supplied Lua 5.4 DLL. See docs/lua_scripting_research.md.
#include <lua.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct World {
    std::uint64_t epoch = 1;
    std::uint32_t generation = 1;
    bool alive = true;
    double position = 0;
};
struct Handle {
    std::uint64_t epoch;
    std::uint32_t id;
    std::uint32_t generation;
};
struct StackScope {
    lua_State* state;
    int top;
    explicit StackScope(lua_State* value) : state(value), top(lua_gettop(value)) {}
    ~StackScope() { lua_settop(state, top); }
};
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Runtime {
public:
    explicit Runtime(World& value) : world(value), state(luaL_newstate()) {
        Check(state != nullptr, "Cannot allocate Lua state");
        luaL_requiref(state, "_G", luaopen_base, 1);
        lua_pop(state, 1);
        // No filesystem loading or dynamically loaded native modules in this fixture.
        for (const char* name : {"dofile", "loadfile", "load"}) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        luaL_newmetatable(state, "Probe.Entity");
        lua_pop(state, 1);
        lua_newtable(state);
        Bind("entity", Entity);
        Bind("move", Move);
        lua_setglobal(state, "engine");
    }
    ~Runtime() { lua_close(state); }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Prepare(std::string_view source, lua_Integer previousCount) {
        StackScope stack(state);
        if (luaL_loadbufferx(state, source.data(), source.size(), "@scripts/probe.lua", "t") != LUA_OK)
            return CaptureError();
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) return CaptureError();
        if (!PlainTable(-1)) return Fail("Module must return a plain table");
        lua_getfield(state, -1, "update");
        const bool hasUpdate = lua_isfunction(state, -1);
        lua_pop(state, 1);
        if (!hasUpdate) return Fail("Missing update function");
        module = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_rawgeti(state, LUA_REGISTRYINDEX, module);
        lua_getfield(state, -1, "migrate");
        lua_remove(state, -2);
        if (!lua_isfunction(state, -1)) return Fail("Missing migrate function");
        // Deliberately narrow schema: one integer. No cross-VM tables or references.
        lua_newtable(state);
        lua_pushinteger(state, previousCount);
        lua_setfield(state, -2, "count");
        if (lua_pcall(state, 1, 1, 0) != LUA_OK) return CaptureError();
        if (!PlainTable(-1)) return Fail("Migration must return a plain state table");
        lua_getfield(state, -1, "count");
        const bool validCount = lua_isinteger(state, -1);
        lua_pop(state, 1);
        if (!validCount) return Fail("State count must be an integer");
        data = luaL_ref(state, LUA_REGISTRYINDEX);
        return true;
    }

    bool Tick() {
        StackScope stack(state);
        staging = false;
        pendingMove = 0;
        lua_rawgeti(state, LUA_REGISTRYINDEX, module);
        lua_getfield(state, -1, "update");
        lua_remove(state, -2);
        lua_rawgeti(state, LUA_REGISTRYINDEX, data);
        const int status = lua_pcall(state, 1, 0, 0);
        staging = true;
        if (status != LUA_OK) {
            pendingMove = 0;
            return CaptureError();
        }
        // This single command fixture has no real component/physics synchronization.
        world.position += pendingMove;
        pendingMove = 0;
        return true;
    }

    lua_Integer Count() {
        StackScope stack(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, data);
        lua_getfield(state, -1, "count");
        return lua_tointeger(state, -1);
    }
    int StackTop() const { return lua_gettop(state); }
    const std::string& Error() const { return error; }

private:
    World& world;
    lua_State* state;
    int module = LUA_NOREF;
    int data = LUA_NOREF;
    bool staging = true;
    double pendingMove = 0;
    std::string error;

    void Bind(const char* name, lua_CFunction function) {
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, function, 1);
        lua_setfield(state, -2, name);
    }
    static Runtime* Owner(lua_State* value) {
        return static_cast<Runtime*>(lua_touserdata(value, lua_upvalueindex(1)));
    }
    static int Entity(lua_State* value) {
        const Runtime* owner = Owner(value);
        auto* handle = static_cast<Handle*>(lua_newuserdatauv(value, sizeof(Handle), 0));
        *handle = {owner->world.epoch, 1, owner->world.generation};
        luaL_setmetatable(value, "Probe.Entity");
        return 1;
    }
    static int Move(lua_State* value) {
        Runtime* owner = Owner(value);
        const auto* handle = static_cast<Handle*>(luaL_checkudata(value, 1, "Probe.Entity"));
        const double distance = luaL_checknumber(value, 2);
        // Only trivial locals span calls that may use Lua's longjmp error path.
        if (!owner->world.alive || handle->epoch != owner->world.epoch || handle->id != 1 ||
            handle->generation != owner->world.generation)
            return luaL_error(value, "Stale entity handle");
        if (owner->staging) return luaL_error(value, "World writes forbidden during reload staging");
        if (!std::isfinite(distance) || !std::isfinite(owner->pendingMove + distance))
            return luaL_error(value, "Invalid move distance");
        owner->pendingMove += distance;
        return 0;
    }
    bool PlainTable(int index) {
        if (!lua_istable(state, index)) return false;
        if (!lua_getmetatable(state, index)) return true;
        lua_pop(state, 1);
        return false;
    }
    bool Fail(const char* message) { error = message; return false; }
    bool CaptureError() {
        error = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "Non-string Lua error";
        return false;
    }
};

bool Reload(std::unique_ptr<Runtime>& active, World& world, std::string_view source) {
    auto candidate = std::make_unique<Runtime>(world);
    const lua_Integer count = active ? active->Count() : 0;
    if (!candidate->Prepare(source, count)) {
        Check(candidate->StackTop() == 0, "Failed reload leaked stack entries");
        std::cout << "Rejected: " << candidate->Error() << '\n';
        return false;
    }
    Check(candidate->StackTop() == 0, "Successful reload leaked stack entries");
    active = std::move(candidate);
    return true;
}

constexpr std::string_view version1 = R"lua(
local entity = engine.entity()
return {
    migrate = function(old) return {count = old.count} end,
    update = function(state)
        state.count = state.count + 1
        engine.move(entity, 1)
    end
}
)lua";
constexpr std::string_view version2 = R"lua(
local entity = engine.entity()
return {
    migrate = function(old) return {count = old.count} end,
    update = function(state)
        state.count = state.count + 10
        engine.move(entity, 2)
    end
}
)lua";
} // namespace

int main(int argc, char** argv) {
    try {
        World world;
        std::unique_ptr<Runtime> active;
        if (argc == 2 && std::string_view(argv[1]) != "--self-test") {
            std::ifstream file(argv[1], std::ios::binary);
            Check(file.is_open(), "Cannot read Lua module");
            const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            Check(Reload(active, world, source), "External module failed to load");
            Check(active->Tick(), "External module update failed");
            std::cout << "count=" << active->Count() << " position=" << world.position << '\n';
            return 0;
        }
        Check(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--self-test"),
              "Usage: lua_reload_probe.exe [--self-test | module.lua]");
        Check(Reload(active, world, version1), "Initial module failed");
        Check(active->Tick() && active->Count() == 1 && world.position == 1, "Version 1 failed");
        Check(Reload(active, world, version2), "Valid reload failed");
        Check(active->Count() == 1, "Declared state lost on reload");
        Check(active->Tick() && active->Count() == 11 && world.position == 3, "New code did not run");
        Runtime* working = active.get();
        Check(!Reload(active, world, "return { broken = "), "Syntax error accepted");
        Check(!Reload(active, world, "return {}"), "Missing exports accepted");
        Check(!Reload(active, world, R"lua(return {
            migrate = function(old) return {count = true} end,
            update = function(state) end
        })lua"), "Invalid declared state accepted");
        Check(!Reload(active, world, R"lua(return {
            migrate = function(old) old.count = 999; error('migration failed') end,
            update = function(state) end
        })lua"), "Migration error accepted");
        Check(!Reload(active, world, "engine.move(engine.entity(), 500); return {}"), "Staging wrote world");
        Check(!Reload(active, world, R"lua(return {
            migrate = function(old) engine.move(engine.entity(), 500); return old end,
            update = function(state) end
        })lua"), "Migration wrote world");
        Check(active.get() == working && active->Count() == 11 && world.position == 3,
              "Failed candidate changed active state or world");
        Check(active->Tick() && active->Count() == 21 && world.position == 5, "Old code stopped after failure");
        Check(Reload(active, world, R"lua(local entity = engine.entity(); return {
            migrate = function(old) return {count = old.count} end,
            update = function(state) engine.move(entity, 500); error('update failed') end
        })lua"), "Runtime-error fixture failed to load");
        Check(!active->Tick() && world.position == 5 && active->StackTop() == 0,
              "Failed update committed native command or leaked stack");
        Check(Reload(active, world, R"lua(return {
            migrate = function(old) return {count = old.count} end,
            update = function(state) error({message = 'non-string error'}) end
        })lua"), "Non-string error fixture failed to load");
        Check(!active->Tick() && active->Error() == "Non-string Lua error" && active->StackTop() == 0,
              "Non-string error was not contained");
        Check(Reload(active, world, version1), "Handle fixture failed to load");
        world.alive = false;
        Check(!active->Tick() && world.position == 5, "Destroyed entity accepted");
        world.alive = true;
        ++world.generation; // Same ID now represents a different entity.
        Check(!active->Tick() && world.position == 5, "Recycled entity ID accepted");
        Check(Reload(active, world, version1), "New incarnation failed to bind");
        ++world.epoch; // A scene replacement invalidates even matching entity generations.
        Check(!active->Tick() && world.position == 5, "Replaced world accepted");
        Check(Reload(active, world, version2), "Fresh world handle failed to bind");
        for (int i = 0; i < 1000; ++i) {
            Check(active->Tick(), "Repeated update failed");
            Check(active->StackTop() == 0, "Repeated update leaked stack");
        }
        for (int i = 0; i < 100; ++i) {
            const lua_Integer count = active->Count();
            Check(Reload(active, world, i % 2 ? version1 : version2), "Repeated reload failed");
            Check(active->Count() == count, "Repeated reload lost state");
            Check(active->Tick() && active->StackTop() == 0, "Repeated reload/update failed");
        }
        std::cout << "PASS: protected loads/updates, export/schema validation, declared state migration, "
                     "failed reload retention, non-string error containment, "
                     "staging guard, command discard, entity generation/world epoch checks, "
                     "1000 updates and 100 reload cycles. " << LUA_RELEASE << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
