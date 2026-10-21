#include "../reflect/Reflection.h"
#include "LuaWorld.h"
#include "LuaComponents.h"
#include "LuaPhysics.h"
#include "LuaAnimation.h"
#include "LuaAudio.h"
#include "LuaDebug.h"
#include "LuaInputBindings.h"
#include "LuaTime.h"
#include "LuaRender.h"
#include "LuaScene.h"

#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../SliceTask.h"
#include "../TransformSystem.h"
#include "../reflect/ReflectServices.h"
#include "../assets/AssetIO.h"
#include "../ui/Pointer.h"
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <cstring>
#include <sstream>

namespace NoxLua {

	namespace {
		constexpr const char* kHandleMeta = "nox.entity";
		constexpr int64_t kApiVersion = 1;

		// Upvalue-free helpers for the visitor pattern on stored values.
		struct PushValueVisitor {
			lua_State* L;
			void operator()(int64_t v) const { lua_pushinteger(L, static_cast<lua_Integer>(v)); }
			void operator()(double v) const { lua_pushnumber(L, static_cast<lua_Number>(v)); }
			void operator()(bool v) const { lua_pushboolean(L, v ? 1 : 0); }
			void operator()(const std::string& v) const { lua_pushlstring(L, v.data(), v.size()); }
		};

		const char* FieldTypeName(FieldType type) {
			switch (type) {
			case FieldType::Number: return "number";
			case FieldType::Integer: return "integer";
			case FieldType::Boolean: return "boolean";
			case FieldType::String: return "string";
			}
			return "?";
		}

		bool ParseFieldType(const char* name, FieldType& out) {
			if (std::strcmp(name, "number") == 0) out = FieldType::Number;
			else if (std::strcmp(name, "integer") == 0) out = FieldType::Integer;
			else if (std::strcmp(name, "boolean") == 0) out = FieldType::Boolean;
			else if (std::strcmp(name, "string") == 0) out = FieldType::String;
			else return false;
			return true;
		}

		bool IsIdentifier(const std::string& name) {
			if (name.empty()) return false;
			for (size_t i = 0; i < name.size(); ++i) {
				const char c = name[i];
				const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
				const bool digit = c >= '0' && c <= '9';
				if (i == 0 ? !alpha : !(alpha || digit)) return false;
			}
			return true;
		}

		// Does a stored value match the declared type?
		bool ValueMatches(const Value& value, FieldType type) {
			switch (type) {
			case FieldType::Number: return std::holds_alternative<int64_t>(value) || std::holds_alternative<double>(value);
			case FieldType::Integer: return std::holds_alternative<int64_t>(value);
			case FieldType::Boolean: return std::holds_alternative<bool>(value);
			case FieldType::String: return std::holds_alternative<std::string>(value);
			}
			return false;
		}

		// Lua stack value -> declared value. Errors describe the expectation.
		bool ReadLuaValue(lua_State* L, int idx, FieldType type, Value& out, std::string& error) {
			switch (type) {
			case FieldType::Number:
				if (lua_isinteger(L, idx)) {
					out = static_cast<int64_t>(lua_tointeger(L, idx));
					return true;
				}
				if (lua_type(L, idx) == LUA_TNUMBER) {
					const double d = lua_tonumber(L, idx);
					if (!std::isfinite(d)) {
						error = "non-finite number";
						return false;
					}
					out = d;
					return true;
				}
				error = "expected number";
				return false;
			case FieldType::Integer:
				if (lua_isinteger(L, idx)) {
					out = static_cast<int64_t>(lua_tointeger(L, idx));
					return true;
				}
				error = "expected integer";
				return false;
			case FieldType::Boolean:
				if (lua_isboolean(L, idx)) {
					out = lua_toboolean(L, idx) != 0;
					return true;
				}
				error = "expected boolean";
				return false;
			case FieldType::String:
				if (lua_type(L, idx) == LUA_TSTRING) {
					size_t len = 0;
					const char* s = lua_tolstring(L, idx, &len);
					out = std::string(s, len);
					return true;
				}
				error = "expected string";
				return false;
			}
			error = "unsupported field type";
			return false;
		}

		// JSON authored value -> declared value.
		bool ReadJsonValue(const nlohmann::json& json, FieldType type, Value& out, std::string& error) {
			switch (type) {
			case FieldType::Number:
				if (json.is_number_integer()) { out = json.get<int64_t>(); return true; }
				if (json.is_number_float()) {
					const double d = json.get<double>();
					if (!std::isfinite(d)) { error = "non-finite number"; return false; }
					out = d;
					return true;
				}
				error = "expected number";
				return false;
			case FieldType::Integer:
				if (json.is_number_integer()) { out = json.get<int64_t>(); return true; }
				error = "expected integer";
				return false;
			case FieldType::Boolean:
				if (json.is_boolean()) { out = json.get<bool>(); return true; }
				error = "expected boolean";
				return false;
			case FieldType::String:
				if (json.is_string()) { out = json.get<std::string>(); return true; }
				error = "expected string";
				return false;
			}
			error = "unsupported field type";
			return false;
		}

		std::string HandleText(const EntityHandle& h) {
			std::ostringstream out;
			out << "w" << h.world << ":" << h.id << ":" << h.incarnation;
			return out.str();
		}

		// Parses "file:line: message" (Lua's location prefix). Logical paths have no drive colons.
		void SplitLocation(const std::string& firstLine, std::string& file, int& line, std::string& message) {
			file.clear();
			line = 0;
			message = firstLine;
			const size_t colon = firstLine.find(':');
			if (colon == std::string::npos || colon == 0) return;
			size_t i = colon + 1;
			int value = 0;
			bool digits = false;
			while (i < firstLine.size() && firstLine[i] >= '0' && firstLine[i] <= '9') {
				value = value * 10 + (firstLine[i] - '0');
				digits = true;
				++i;
			}
			if (!digits || i >= firstLine.size() || firstLine[i] != ':') return;
			file = firstLine.substr(0, colon);
			line = value;
			size_t start = i + 1;
			while (start < firstLine.size() && firstLine[start] == ' ') ++start;
			message = firstLine.substr(start);
		}
	}

	// C entry points of the nox API. Each takes the LuaWorld as its first upvalue.
	struct LuaApiBinder {
		static LuaWorld* WorldOf(lua_State* L) {
			return static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		static void SetFn(lua_State* L, LuaWorld* w, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, w);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, -2, name);
		}

		static void PushHandle(lua_State* L, const EntityHandle& h) {
			void* mem = lua_newuserdatauv(L, sizeof(EntityHandle), 0);
			std::memcpy(mem, &h, sizeof(EntityHandle));
			luaL_setmetatable(L, kHandleMeta);
		}

		static const EntityHandle* TestHandle(lua_State* L, int idx) {
			return static_cast<const EntityHandle*>(luaL_testudata(L, idx, kHandleMeta));
		}

		// Handle argument that must name a live entity of this world.
		static EntityHandle LiveHandle(lua_State* L, LuaWorld* w, int idx) {
			const EntityHandle* h = TestHandle(L, idx);
			if (!h) luaL_argerror(L, idx, "entity handle expected");
			if (!w->Manager() || !w->Manager()->IsHandleValid(*h)) {
				luaL_error(L, "stale or invalid entity handle (%s)", HandleText(*h).c_str());
			}
			return *h;
		}

		static void RequireCallback(lua_State* L, LuaWorld* w, const char* fn) {
			if (!w->InCallback()) luaL_error(L, "ctx:%s is only available inside a system, behavior or event", fn);
		}

		static void RequireLoading(lua_State* L, LuaWorld* w, const char* fn) {
			if (!w->Loading()) luaL_error(L, "nox.%s is only available while the project loads", fn);
		}

		static void PushNumberTriple(lua_State* L, const glm::vec3& v) {
			lua_pushnumber(L, v.x);
			lua_pushnumber(L, v.y);
			lua_pushnumber(L, v.z);
		}

		// ---- handle metatable ----
		static int HandleEq(lua_State* L) {
			const EntityHandle* a = TestHandle(L, 1);
			const EntityHandle* b = TestHandle(L, 2);
			lua_pushboolean(L, (a && b && *a == *b) ? 1 : 0);
			return 1;
		}
		static int HandleToString(lua_State* L) {
			const EntityHandle* h = TestHandle(L, 1);
			if (!h) return luaL_error(L, "entity handle expected");
			lua_pushfstring(L, "entity(%s)", HandleText(*h).c_str());
			return 1;
		}

		// ---- nox.* (load phase) ----
		static int NoxSchema(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "schema");
			const std::string name = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TTABLE);
			Schema schema;
			schema.name = name;
			lua_pushnil(L);
			while (lua_next(L, 2) != 0) {
				if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "schema '%s': field names must be strings", name.c_str());
				const std::string field = lua_tostring(L, -2);
				FieldType type = FieldType::Number;
				if (lua_type(L, -1) != LUA_TSTRING || !ParseFieldType(lua_tostring(L, -1), type)) {
					return luaL_error(L, "schema '%s': field '%s' needs a type: number, integer, boolean or string",
						name.c_str(), field.c_str());
				}
				if (!IsIdentifier(field)) return luaL_error(L, "schema '%s': invalid field name '%s'", name.c_str(), field.c_str());
				schema.fields[field] = type;
				lua_pop(L, 1);
			}
			if (!lua_isnoneornil(L, 3)) {
				luaL_checktype(L, 3, LUA_TTABLE);
				lua_pushnil(L);
				while (lua_next(L, 3) != 0) {
					const std::string field = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : std::string();
					const auto it = schema.fields.find(field);
					if (it == schema.fields.end()) {
						return luaL_error(L, "schema '%s': default for unknown field '%s'", name.c_str(), field.c_str());
					}
					Value value;
					std::string error;
					if (!ReadLuaValue(L, -1, it->second, value, error)) {
						return luaL_error(L, "schema '%s': default '%s' %s", name.c_str(), field.c_str(), error.c_str());
					}
					schema.defaults[field] = value;
					lua_pop(L, 1);
				}
			}
			if (!lua_isnoneornil(L, 4)) {
				if (!lua_isinteger(L, 4) || lua_tointeger(L, 4) < 1) {
					return luaL_error(L, "schema '%s': version must be an integer >= 1", name.c_str());
				}
				schema.version = static_cast<int>(lua_tointeger(L, 4));
			}
			std::string error;
			if (!w->AddSchema(std::move(schema), error)) return luaL_error(L, "%s", error.c_str());
			return 0;
		}

		// Reads a required string/function/integer option from a table argument.
		static std::string TableString(lua_State* L, int tableIdx, const char* key, bool required) {
			lua_getfield(L, tableIdx, key);
			std::string value;
			if (lua_type(L, -1) == LUA_TSTRING) {
				value = lua_tostring(L, -1);
			}
			else if (required) {
				luaL_error(L, "option '%s' must be a string", key);
			}
			lua_pop(L, 1);
			return value;
		}

		static int NoxSystem(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "system");
			luaL_checktype(L, 1, LUA_TTABLE);
			const std::string name = TableString(L, 1, "name", true);
			if (name.empty()) return luaL_error(L, "system needs a non-empty name");
			lua_getfield(L, 1, "order");
			int order = 0;
			if (!lua_isnil(L, -1)) {
				if (!lua_isinteger(L, -1)) return luaL_error(L, "system '%s': order must be an integer", name.c_str());
				order = static_cast<int>(lua_tointeger(L, -1));
			}
			lua_pop(L, 1);
			lua_getfield(L, 1, "update");
			if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "system '%s' needs an update function", name.c_str());
			bool fixed = false;
			lua_getfield(L, 1, "fixed");
			if (!lua_isnil(L, -1)) fixed = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			w->AddSystem(name, order, ref, fixed);
			return 0;
		}

		// Optional function field of a behaviour table; returns its registry reference (0 when absent).
		static int OptionalHook(lua_State* L, int tableIdx, const char* key, const std::string& name) {
			lua_getfield(L, tableIdx, key);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				return 0;
			}
			if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "behavior '%s': %s must be a function", name.c_str(), key);
			return luaL_ref(L, LUA_REGISTRYINDEX);
		}

		// nox.behavior{ name, update = fn(ctx, entity, dt), fixed_update = fn(ctx, entity, dt),
		//               on_start = fn(ctx, entity), on_destroy = fn(ctx, entity) }. At least one function is required.
		static int NoxBehavior(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "behavior");
			luaL_checktype(L, 1, LUA_TTABLE);
			const std::string name = TableString(L, 1, "name", true);
			if (name.empty()) return luaL_error(L, "behavior needs a non-empty name");
			LuaWorld::BehaviorRefs refs;
			refs.update = OptionalHook(L, 1, "update", name);
			refs.fixed = OptionalHook(L, 1, "fixed_update", name);
			refs.start = OptionalHook(L, 1, "on_start", name);
			refs.destroy = OptionalHook(L, 1, "on_destroy", name);
			if (!refs.update && !refs.fixed && !refs.start && !refs.destroy) {
				return luaL_error(L, "behavior '%s' needs update, fixed_update, on_start or on_destroy", name.c_str());
			}
			w->AddBehavior(name, refs);
			return 0;
		}

		static int NoxOn(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "on");
			const std::string event = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			lua_pushvalue(L, 2);
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			w->AddHandler(event, ref);
			return 0;
		}

		// nox.editor_panel{ id = "name", title = "Title", draw = function(ctx, ed) ... end }
		static int NoxEditorPanel(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "editor_panel");
			luaL_checktype(L, 1, LUA_TTABLE);
			const std::string id = TableString(L, 1, "id", true);
			if (id.empty() || id.size() > 64) return luaL_error(L, "editor_panel needs an id of 1..64 characters");
			std::string title = TableString(L, 1, "title", false);
			if (title.empty()) title = id;
			lua_getfield(L, 1, "draw");
			if (lua_type(L, -1) != LUA_TFUNCTION) return luaL_error(L, "editor_panel '%s' needs a draw function", id.c_str());
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			w->AddEditorPanel(id, title, ref);
			return 0;
		}

		// nox.editor_viewport(function(ctx, ed) ... end): draws viewport handles (ed.handle) every frame.
		static int NoxEditorViewport(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "editor_viewport");
			luaL_checktype(L, 1, LUA_TFUNCTION);
			lua_pushvalue(L, 1);
			w->SetEditorViewport(luaL_ref(L, LUA_REGISTRYINDEX));
			return 0;
		}

		// nox.on_draw(function(ctx, ui, dt) ... end): registered while loading; run by DrawFrame.
		static int NoxOnDraw(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "on_draw");
			luaL_checktype(L, 1, LUA_TFUNCTION);
			lua_pushvalue(L, 1);
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			w->AddDrawHandler(ref);
			return 0;
		}

		// nox.on_configure(function(game, scene) ... end): once per load of the world (scene load, Reconfigure, reload).
		static int NoxOnConfigure(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "on_configure");
			luaL_checktype(L, 1, LUA_TFUNCTION);
			if (w->m_configureRef != 0) return luaL_error(L, "nox.on_configure can be registered once per project");
			lua_pushvalue(L, 1);
			w->AddConfigureHook(luaL_ref(L, LUA_REGISTRYINDEX));
			return 0;
		}

		// nox.on_save_game(function() return value end): the authoring JSON that SaveGameJson returns.
		static int NoxOnSaveGame(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "on_save_game");
			luaL_checktype(L, 1, LUA_TFUNCTION);
			if (w->m_saveGameRef != 0) return luaL_error(L, "nox.on_save_game can be registered once per project");
			lua_pushvalue(L, 1);
			w->AddSaveGameHook(luaL_ref(L, LUA_REGISTRYINDEX));
			return 0;
		}

		static int NoxMigrate(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireLoading(L, w, "migrate");
			const std::string schema = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			lua_pushvalue(L, 2);
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			std::string error;
			if (!w->AddMigration(schema, ref, error)) {
				luaL_unref(L, LUA_REGISTRYINDEX, ref);
				return luaL_error(L, "%s", error.c_str());
			}
			return 0;
		}

		static int NoxLog(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			std::string line;
			const int n = lua_gettop(L);
			for (int i = 1; i <= n; ++i) {
				if (i > 1) line += '\t';
				size_t len = 0;
				const char* s = luaL_tolstring(L, i, &len);
				line.append(s, len);
				lua_pop(L, 1);
			}
			w->Log(line);
			return 0;
		}

		// ---- ctx (callbacks) ----
		static int CtxIsValid(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle* h = TestHandle(L, 2);
			lua_pushboolean(L, (h && w->Manager() && w->Manager()->IsHandleValid(*h)) ? 1 : 0);
			return 1;
		}

		static int CtxKey(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			lua_pushstring(L, HandleText(h).c_str());
			return 1;
		}

		static int CtxSame(lua_State* L) {
			const EntityHandle* a = TestHandle(L, 2);
			const EntityHandle* b = TestHandle(L, 3);
			lua_pushboolean(L, (a && b && *a == *b) ? 1 : 0);
			return 1;
		}

		static int CtxFind(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const std::string name = luaL_checkstring(L, 2);
			const EntityHandle h = w->FindByName(name);
			if (h.id == INVALID_ENTITY) {
				lua_pushnil(L);
			}
			else {
				PushHandle(L, h);
			}
			return 1;
		}

		static int CtxName(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string name = w->Manager()->GetEntityName(h.id);
			lua_pushlstring(L, name.data(), name.size());
			return 1;
		}

		static int CtxEntities(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			ComponentManager* cm = w->Manager();
			std::vector<EntityID> ids;
			for (const auto& entry : cm->GetTransformPool()) ids.push_back(entry.entity);
			std::sort(ids.begin(), ids.end());
			lua_newtable(L);
			lua_Integer index = 1;
			for (EntityID id : ids) {
				const EntityHandle h = cm->MakeHandle(id);
				if (!cm->IsHandleValid(h)) continue;
				PushHandle(L, h);
				lua_rawseti(L, -2, index++);
			}
			return 1;
		}

		static int CtxHasComponent(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string name = luaL_checkstring(L, 3);
			ComponentManager* cm = w->Manager();
			// Registry descriptors (src/reflect) check the scene node; native pools answer for the rest.
			bool has = false;
			bool known = false;
			if (const nox::reflect::ComponentDesc* desc = nox::reflect::Registry::Get().Find(name)) {
				known = true;
				if (std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id)) {
					has = desc->has && desc->has(*node);
				}
			}
			bool nativeKnown = false;
			has = NativeComponentPresent(cm, h.id, name, nativeKnown) || has;
			if (!known && !nativeKnown) return luaL_argerror(L, 3, "unknown native component name (see nox.components())");
			lua_pushboolean(L, has ? 1 : 0);
			return 1;
		}

		static int CtxPosition(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			PushNumberTriple(L, w->m_scene->GetTransformSystem()->GetWorldPosition(h.id));
			return 3;
		}

		static int CtxScale(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			PushNumberTriple(L, w->m_scene->GetTransformSystem()->GetWorldScale(h.id));
			return 3;
		}

		static int CtxQuery(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const std::string schema = luaL_checkstring(L, 2);
			if (!w->FindSchema(schema)) return luaL_argerror(L, 2, "unknown schema");
			lua_newtable(L);
			lua_Integer index = 1;
			// The declared-data map is ordered by handle: snapshot in ascending entity ID.
			for (const EntityHandle& h : w->DataHandlesWithSchema(schema)) {
				PushHandle(L, h);
				lua_rawseti(L, -2, index++);
			}
			return 1;
		}

		static int CtxHas(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schema = luaL_checkstring(L, 3);
			lua_pushboolean(L, w->HasDeclared(h, schema) ? 1 : 0);
			return 1;
		}

		static int CtxAdd(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "add");
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schemaName = luaL_checkstring(L, 3);
			const Schema* schema = w->FindSchema(schemaName);
			if (!schema) return luaL_argerror(L, 3, "unknown schema");
			FieldMap values = schema->defaults;
			if (!lua_isnoneornil(L, 4)) {
				luaL_checktype(L, 4, LUA_TTABLE);
				lua_pushnil(L);
				while (lua_next(L, 4) != 0) {
					const std::string field = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : std::string();
					const auto it = schema->fields.find(field);
					if (it == schema->fields.end()) return luaL_error(L, "schema '%s' has no field '%s'", schemaName.c_str(), field.c_str());
					Value value;
					std::string error;
					if (!ReadLuaValue(L, -1, it->second, value, error)) {
						return luaL_error(L, "%s.%s: %s", schemaName.c_str(), field.c_str(), error.c_str());
					}
					values[field] = value;
					lua_pop(L, 1);
				}
			}
			for (const auto& [field, type] : schema->fields) {
				(void)type;
				if (values.count(field) == 0) return luaL_error(L, "schema '%s': field '%s' has no value and no default", schemaName.c_str(), field.c_str());
			}
			w->SetDeclaredRecord(h, schemaName, values);
			return 0;
		}

		static int CtxRemove(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "remove");
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schema = luaL_checkstring(L, 3);
			w->RemoveDeclaredRecord(h, schema);
			return 0;
		}

		static int CtxGet(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schema = luaL_checkstring(L, 3);
			const FieldMap* fields = w->FindDeclaredRecord(h, schema);
			if (!fields) {
				lua_pushnil(L);
				return 1;
			}
			lua_newtable(L);
			for (const auto& [field, value] : *fields) {
				std::visit(PushValueVisitor{ L }, value);
				lua_setfield(L, -2, field.c_str());
			}
			return 1;
		}

		static int CtxGetField(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schema = luaL_checkstring(L, 3);
			const std::string field = luaL_checkstring(L, 4);
			Value value;
			if (!w->GetDeclaredField(h, schema, field, value)) {
				lua_pushnil(L);
				return 1;
			}
			std::visit(PushValueVisitor{ L }, value);
			return 1;
		}

		static int CtxSetField(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "set_field");
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string schemaName = luaL_checkstring(L, 3);
			const std::string field = luaL_checkstring(L, 4);
			const Schema* schema = w->FindSchema(schemaName);
			if (!schema) return luaL_argerror(L, 3, "unknown schema");
			const auto it = schema->fields.find(field);
			if (it == schema->fields.end()) return luaL_error(L, "schema '%s' has no field '%s'", schemaName.c_str(), field.c_str());
			Value value;
			std::string error;
			if (!ReadLuaValue(L, 5, it->second, value, error)) {
				return luaL_error(L, "%s.%s: %s", schemaName.c_str(), field.c_str(), error.c_str());
			}
			if (!w->SetDeclaredField(h, schemaName, field, value, error)) return luaL_error(L, "%s", error.c_str());
			return 0;
		}

		static int CtxAttach(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "attach");
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string behavior = luaL_checkstring(L, 3);
			std::string error;
			if (!w->AttachBehavior(h, behavior, error)) return luaL_error(L, "%s", error.c_str());
			return 0;
		}

		static int CtxDetach(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "detach");
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string behavior = luaL_checkstring(L, 3);
			w->DetachBehavior(h, behavior);
			return 0;
		}

		static int CtxSpawn(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "spawn");
			const std::string name = luaL_checkstring(L, 2);
			if (name.empty()) return luaL_argerror(L, 2, "name must not be empty");
			LuaWorld::Command cmd;
			cmd.kind = LuaWorld::Command::Kind::Spawn;
			cmd.name = name;
			cmd.token = w->NextSpawnToken();
			w->QueueCommand(cmd);
			lua_pushinteger(L, static_cast<lua_Integer>(cmd.token));
			return 1;
		}

		static int CtxResolveSpawn(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const lua_Integer token = luaL_checkinteger(L, 2);
			EntityHandle h;
			if (w->ResolveSpawn(static_cast<int64_t>(token), h)) {
				PushHandle(L, h);
			}
			else {
				lua_pushnil(L);
			}
			return 1;
		}

		static int CtxDestroy(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "destroy");
			const EntityHandle* h = TestHandle(L, 2);
			if (!h) return luaL_argerror(L, 2, "entity handle expected");
			LuaWorld::Command cmd;
			cmd.kind = LuaWorld::Command::Kind::Destroy;
			cmd.target = *h;
			w->QueueCommand(cmd);
			return 0;
		}

		static int QueueMove(lua_State* L, LuaWorld::Command::Kind kind, const char* fn) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, fn);
			const EntityHandle* h = TestHandle(L, 2);
			if (!h) return luaL_argerror(L, 2, "entity handle expected");
			LuaWorld::Command cmd;
			cmd.kind = kind;
			cmd.target = *h;
			cmd.value = glm::vec3(static_cast<float>(luaL_checknumber(L, 3)),
				static_cast<float>(luaL_checknumber(L, 4)),
				static_cast<float>(luaL_checknumber(L, 5)));
			w->QueueCommand(cmd);
			return 0;
		}
		static int CtxMove(lua_State* L) { return QueueMove(L, LuaWorld::Command::Kind::Move, "move"); }
		static int CtxTeleport(lua_State* L) { return QueueMove(L, LuaWorld::Command::Kind::Teleport, "teleport"); }
		static int CtxSetScale(lua_State* L) { return QueueMove(L, LuaWorld::Command::Kind::SetScale, "set_scale"); }

		// ---- Timers and coroutines (docs/lua_api.md, "Lifecycle, timers and coroutines") -------------------
		static constexpr const char* kTaskMeta = "nox.task";

		static void PushTask(lua_State* L, uint64_t id) {
			uint64_t* slot = static_cast<uint64_t*>(lua_newuserdatauv(L, sizeof(uint64_t), 0));
			*slot = id;
			luaL_setmetatable(L, kTaskMeta);
		}

		// Optional entity argument (nil or absent: global scope). Validated before any registry reference is made.
		static bool OptionalEntity(lua_State* L, LuaWorld* w, int idx, EntityHandle& out) {
			if (lua_isnoneornil(L, idx)) return false;
			out = LiveHandle(L, w, idx);
			return true;
		}

		static void RequireTaskScope(lua_State* L, LuaWorld* w, const char* fn) {
			RequireCallback(L, w, fn);
			if (!w->Playing()) luaL_error(L, "ctx:%s is only available while the game is playing", fn);
			if (w->TasksFull()) luaL_error(L, "too many timers and coroutines (limit %d)", static_cast<int>(LuaWorld::kMaxTasks));
		}

		static double CheckSeconds(lua_State* L, int idx) {
			const double seconds = luaL_checknumber(L, idx);
			if (!std::isfinite(seconds) || seconds < 0.0) luaL_argerror(L, idx, "seconds must be a finite number >= 0");
			return seconds;
		}

		// ctx:after(seconds, fn [, entity]) and ctx:every(seconds, fn [, entity]): fn(ctx, entity) runs on the play clock.
		static int AddTimer(lua_State* L, bool repeat) {
			LuaWorld* w = WorldOf(L);
			const char* fn = repeat ? "every" : "after";
			RequireTaskScope(L, w, fn);
			const double seconds = CheckSeconds(L, 2);
			if (repeat && seconds <= 0.0) return luaL_argerror(L, 2, "every needs an interval > 0");
			luaL_checktype(L, 3, LUA_TFUNCTION);
			LuaWorld::Task task;
			task.kind = LuaWorld::Task::Kind::Timer;
			task.label = fn;
			task.hasEntity = OptionalEntity(L, w, 4, task.entity);
			task.due = w->PlayTime() + seconds;
			task.interval = repeat ? seconds : 0.0;
			lua_pushvalue(L, 3);
			task.ref = luaL_ref(L, LUA_REGISTRYINDEX);
			PushTask(L, w->AddTask(std::move(task)));
			return 1;
		}
		static int CtxAfter(lua_State* L) { return AddTimer(L, false); }
		static int CtxEvery(lua_State* L) { return AddTimer(L, true); }

		// ctx:start_coroutine(fn [, entity]): fn(ctx, entity) runs in a Lua coroutine, resumed by the host each frame.
		static int CtxStartCoroutine(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireTaskScope(L, w, "start_coroutine");
			luaL_checktype(L, 2, LUA_TFUNCTION);
			LuaWorld::Task task;
			task.kind = LuaWorld::Task::Kind::Coroutine;
			task.label = "coroutine";
			task.hasEntity = OptionalEntity(L, w, 3, task.entity);
			task.thread = lua_newthread(L);
			task.ref = luaL_ref(L, LUA_REGISTRYINDEX);   // anchors the thread (luaL_ref pops it)
			lua_pushvalue(L, 2);
			lua_xmove(L, task.thread, 1);
			PushTask(L, w->AddTask(std::move(task)));
			return 1;
		}

		// Wait calls yield (kind, value) to the host. Valid only inside a coroutine that the host is resuming.
		static void RequireCoroutine(lua_State* L, LuaWorld* w, const char* fn) {
			const bool isMain = lua_pushthread(L) == 1;
			lua_pop(L, 1);
			if (isMain || !w->InCoroutineSegment()) {
				luaL_error(L, "ctx:%s is only available inside a coroutine started with ctx:start_coroutine", fn);
			}
		}
		static int CtxWait(lua_State* L) {
			RequireCoroutine(L, WorldOf(L), "wait");
			const double seconds = CheckSeconds(L, 2);
			lua_pushliteral(L, "time");
			lua_pushnumber(L, seconds);
			return lua_yield(L, 2);
		}
		static int CtxWaitFrames(lua_State* L) {
			RequireCoroutine(L, WorldOf(L), "wait_frames");
			const lua_Integer frames = luaL_checkinteger(L, 2);
			if (frames < 0) return luaL_argerror(L, 2, "frames must be >= 0");
			lua_pushliteral(L, "frames");
			lua_pushinteger(L, frames);
			return lua_yield(L, 2);
		}
		static int CtxWaitUntil(lua_State* L) {
			RequireCoroutine(L, WorldOf(L), "wait_until");
			luaL_checktype(L, 2, LUA_TFUNCTION);
			lua_pushliteral(L, "until");
			lua_pushvalue(L, 2);
			return lua_yield(L, 2);
		}

		// ctx:scene_payload(): a fresh copy of the payload this scene was loaded with, or nil.
		static int CtxScenePayload(lua_State* L) {
			const nlohmann::json* payload = WorldOf(L)->ScenePayloadPtr();
			if (!payload) lua_pushnil(L);
			else PushJsonValue(L, *payload);
			return 1;
		}

		// Handle methods (nox.task): task:cancel(), task:active().
		static LuaWorld::Task* TaskOf(lua_State* L, LuaWorld* w) {
			const uint64_t* id = static_cast<const uint64_t*>(luaL_testudata(L, 1, kTaskMeta));
			if (!id) luaL_argerror(L, 1, "timer or coroutine handle expected (use task:cancel())");
			return w->FindTask(*id);
		}
		static int TaskCancel(lua_State* L) {
			if (LuaWorld::Task* task = TaskOf(L, WorldOf(L))) task->cancelled = true;
			return 0;
		}
		static int TaskActive(lua_State* L) {
			const LuaWorld::Task* task = TaskOf(L, WorldOf(L));
			lua_pushboolean(L, (task && !task->cancelled) ? 1 : 0);
			return 1;
		}

		// ---- Input (project action map, sampled by the module once per frame) ----------------------------
		static const InputMap& Inputs(LuaWorld* w) {
			static const InputMap kNone;
			return w->m_inputs ? *w->m_inputs : kNone;
		}
		// Optional player index (0..kMaxPads-1, default 0): ctx:action_down(name, player).
		static int PlayerArg(lua_State* L) {
			const lua_Integer player = luaL_optinteger(L, 3, 0);
			if (player < 0 || player >= kMaxPads) luaL_error(L, "input player must be 0..%d", kMaxPads - 1);
			return static_cast<int>(player);
		}
		// which: 0 = down, 1 = pressed, 2 = released
		static int ActionQuery(lua_State* L, int which) {
			LuaWorld* w = WorldOf(L);
			const char* name = luaL_checkstring(L, 2);
			const int player = PlayerArg(L);
			bool known = false;
			const InputMap& map = Inputs(w);
			const bool value = which == 0 ? map.Down(name, &known, player) : which == 1 ? map.Pressed(name, &known, player) : map.Released(name, &known, player);
			if (!known) return luaL_error(L, "unknown input action '%s' (declare it in input.json)", name);
			lua_pushboolean(L, value ? 1 : 0);
			return 1;
		}
		static int CtxActionDown(lua_State* L) { return ActionQuery(L, 0); }
		static int CtxActionPressed(lua_State* L) { return ActionQuery(L, 1); }
		static int CtxActionReleased(lua_State* L) { return ActionQuery(L, 2); }
		static int CtxAxis(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const char* name = luaL_checkstring(L, 2);
			const int player = PlayerArg(L);
			bool known = false;
			const float value = Inputs(w).Axis(name, &known, player);
			if (!known) return luaL_error(L, "unknown input action '%s' (declare it in input.json)", name);
			lua_pushnumber(L, value);
			return 1;
		}

		// ---- Pointer (src/ui/Pointer.h, window coordinates) -----------------------------------------------
		static bool ParseButton(lua_State* L, int idx, ui::PointerButton& out) {
			const char* s = luaL_checkstring(L, idx);
			if (std::strcmp(s, "left") == 0) out = ui::PointerButton::Left;
			else if (std::strcmp(s, "right") == 0) out = ui::PointerButton::Right;
			else if (std::strcmp(s, "middle") == 0) out = ui::PointerButton::Middle;
			else {
				luaL_error(L, "pointer button must be left, right or middle");
				return false;
			}
			return true;
		}
		static int CtxPointerPosition(lua_State* L) {
			const glm::vec2 p = ui::Pointer::Get().Position();
			lua_pushnumber(L, p.x);
			lua_pushnumber(L, p.y);
			return 2;
		}
		static int CtxPointerDown(lua_State* L) {
			ui::PointerButton b = ui::PointerButton::Left;
			if (!ParseButton(L, 2, b)) return 0;
			lua_pushboolean(L, ui::Pointer::Get().Down(b) ? 1 : 0);
			return 1;
		}
		static int CtxPointerPressed(lua_State* L) {
			ui::PointerButton b = ui::PointerButton::Left;
			if (!ParseButton(L, 2, b)) return 0;
			lua_pushboolean(L, ui::Pointer::Get().Pressed(b) ? 1 : 0);
			return 1;
		}
		static int CtxPointerReleased(lua_State* L) {
			ui::PointerButton b = ui::PointerButton::Left;
			if (!ParseButton(L, 2, b)) return 0;
			lua_pushboolean(L, ui::Pointer::Get().Released(b) ? 1 : 0);
			return 1;
		}
		static int CtxPointerWheel(lua_State* L) {
			lua_pushnumber(L, ui::Pointer::Get().Wheel());
			return 1;
		}
		static int CtxOverUi(lua_State* L) {
			lua_pushboolean(L, ui::Pointer::Get().OverUI() ? 1 : 0);
			return 1;
		}
		static int CtxWantsPointer(lua_State* L) {
			lua_pushboolean(L, WorldOf(L)->m_wantsPointer ? 1 : 0);
			return 1;
		}
		static int CtxSetWantsPointer(lua_State* L) {
			luaL_checktype(L, 2, LUA_TBOOLEAN);
			WorldOf(L)->m_wantsPointer = lua_toboolean(L, 2) != 0;
			return 0;
		}

		static int CtxUiConsumed(lua_State* L) {
			lua_pushboolean(L, WorldOf(L)->m_nav.Consumed() ? 1 : 0);
			return 1;
		}

		static int CtxUiBackPressed(lua_State* L) {
			lua_pushboolean(L, WorldOf(L)->m_nav.Back() ? 1 : 0);
			return 1;
		}

		static int CtxEmit(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireCallback(L, w, "emit");
			const std::string name = luaL_checkstring(L, 2);
			int payloadRef = LUA_REFNIL;
			if (lua_gettop(L) >= 3) {
				lua_pushvalue(L, 3);
				payloadRef = luaL_ref(L, LUA_REGISTRYINDEX);   // nil yields LUA_REFNIL
			}
			if (!w->QueueEvent(name, payloadRef)) {
				if (payloadRef != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, payloadRef);
				return luaL_error(L, "event queue is full (more than %d events in one frame)", static_cast<int>(LuaWorld::kMaxEventsPerFrame));
			}
			return 0;
		}

		static int CtxInputEnabled(lua_State* L) {
			lua_pushboolean(L, WorldOf(L)->InputEnabled() ? 1 : 0);
			return 1;
		}

		static int CtxTime(lua_State* L) {
			lua_pushnumber(L, WorldOf(L)->PlayTime());
			return 1;
		}

		static int CtxSubstep(lua_State* L) {
			lua_pushinteger(L, static_cast<lua_Integer>(WorldOf(L)->m_substepIndex));
			return 1;
		}

		static int CtxFrame(lua_State* L) {
			lua_pushinteger(L, static_cast<lua_Integer>(WorldOf(L)->Frame()));
			return 1;
		}

		static int CtxLog(lua_State* L) {
			lua_remove(L, 1);   // method call: drop the ctx argument
			return NoxLog(L);
		}

		// nox.log_raw(line): the line as written (no "[Lua] " prefix). One argument, a string.
		static int NoxLogRaw(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			size_t len = 0;
			const char* s = luaL_checklstring(L, 1, &len);
			w->m_runtime->PrintRaw(std::string(s, len));
			return 0;
		}

		// Runs inside ProtectedCall during Load: installs the metatable, `nox` and `ctx`.
		static int InstallInner(lua_State* L) {
			LuaWorld* w = static_cast<LuaWorld*>(lua_touserdata(L, 1));

			luaL_newmetatable(L, kHandleMeta);
			lua_pushcfunction(L, HandleEq);
			lua_setfield(L, -2, "__eq");
			lua_pushcfunction(L, HandleToString);
			lua_setfield(L, -2, "__tostring");
			lua_pushstring(L, kHandleMeta);
			lua_setfield(L, -2, "__name");
			lua_pop(L, 1);

			lua_newtable(L);
			SetFn(L, w, "schema", NoxSchema);
			SetFn(L, w, "system", NoxSystem);
			SetFn(L, w, "behavior", NoxBehavior);
			SetFn(L, w, "on", NoxOn);
			SetFn(L, w, "migrate", NoxMigrate);
			SetFn(L, w, "on_draw", NoxOnDraw);
			SetFn(L, w, "on_configure", NoxOnConfigure);
			SetFn(L, w, "on_save_game", NoxOnSaveGame);
			SetFn(L, w, "editor_panel", NoxEditorPanel);
			SetFn(L, w, "editor_viewport", NoxEditorViewport);
			SetFn(L, w, "log", NoxLog);
			SetFn(L, w, "log_raw", NoxLogRaw);
			lua_pushinteger(L, static_cast<lua_Integer>(kApiVersion));
			lua_setfield(L, -2, "api_version");
			lua_setglobal(L, "nox");

			lua_newtable(L);
			SetFn(L, w, "is_valid", CtxIsValid);
			SetFn(L, w, "key", CtxKey);
			SetFn(L, w, "same", CtxSame);
			SetFn(L, w, "find", CtxFind);
			SetFn(L, w, "name", CtxName);
			SetFn(L, w, "entities", CtxEntities);
			SetFn(L, w, "has_component", CtxHasComponent);
			SetFn(L, w, "position", CtxPosition);
			SetFn(L, w, "scale", CtxScale);
			SetFn(L, w, "query", CtxQuery);
			SetFn(L, w, "add", CtxAdd);
			SetFn(L, w, "remove", CtxRemove);
			SetFn(L, w, "has", CtxHas);
			SetFn(L, w, "get", CtxGet);
			SetFn(L, w, "get_field", CtxGetField);
			SetFn(L, w, "set_field", CtxSetField);
			SetFn(L, w, "attach", CtxAttach);
			SetFn(L, w, "detach", CtxDetach);
			SetFn(L, w, "spawn", CtxSpawn);
			SetFn(L, w, "resolve_spawn", CtxResolveSpawn);
			SetFn(L, w, "destroy", CtxDestroy);
			SetFn(L, w, "move", CtxMove);
			SetFn(L, w, "teleport", CtxTeleport);
			SetFn(L, w, "set_scale", CtxSetScale);
			SetFn(L, w, "emit", CtxEmit);
			SetFn(L, w, "after", CtxAfter);
			SetFn(L, w, "every", CtxEvery);
			SetFn(L, w, "start_coroutine", CtxStartCoroutine);
			SetFn(L, w, "wait", CtxWait);
			SetFn(L, w, "wait_frames", CtxWaitFrames);
			SetFn(L, w, "wait_until", CtxWaitUntil);
			SetFn(L, w, "scene_payload", CtxScenePayload);
			SetFn(L, w, "input_enabled", CtxInputEnabled);
			SetFn(L, w, "time", CtxTime);
			SetFn(L, w, "frame", CtxFrame);
			SetFn(L, w, "substep", CtxSubstep);
			SetFn(L, w, "log", CtxLog);
			SetFn(L, w, "action_down", CtxActionDown);
			SetFn(L, w, "action_pressed", CtxActionPressed);
			SetFn(L, w, "action_released", CtxActionReleased);
			SetFn(L, w, "axis", CtxAxis);
			SetFn(L, w, "pointer_position", CtxPointerPosition);
			SetFn(L, w, "pointer_down", CtxPointerDown);
			SetFn(L, w, "pointer_pressed", CtxPointerPressed);
			SetFn(L, w, "pointer_released", CtxPointerReleased);
			SetFn(L, w, "pointer_wheel", CtxPointerWheel);
			SetFn(L, w, "over_ui", CtxOverUi);
			SetFn(L, w, "ui_consumed", CtxUiConsumed);
			SetFn(L, w, "ui_back_pressed", CtxUiBackPressed);
			SetFn(L, w, "wants_pointer", CtxWantsPointer);
			SetFn(L, w, "set_wants_pointer", CtxSetWantsPointer);
			// nox.task metatable: timer and coroutine handles (cancel, active). Its methods capture the world.
			luaL_newmetatable(L, kTaskMeta);
			lua_newtable(L);
			SetFn(L, w, "cancel", TaskCancel);
			SetFn(L, w, "active", TaskActive);
			lua_setfield(L, -2, "__index");
			lua_pop(L, 1);
			// Component proxies, add/remove/list, nox.components, parent/children, raycast, load_scene (LuaComponents.cpp).
			InstallComponentApi(L, lua_gettop(L), w);
			// Physics queries (ctx:physics_raycast, overlap_sphere, overlap_box) and physics events (LuaPhysics.cpp).
			InstallPhysicsApi(L, lua_gettop(L), w);
			// nox.physics (world tuning: settings, get, set, reset, set_collision, collides).
			InstallPhysicsSettingsApi(L);
			// ctx:time_scale (LuaTime.cpp): the ctx table is on top here, like for the component functions.
			InstallTimeApi(L, lua_gettop(L));
			// ctx:screen_to_ray (LuaRender.cpp).
			InstallRenderApi(L, lua_gettop(L), w);
			// ctx:instantiate (prefab instances, LuaScene.cpp).
			InstallSceneApi(L, lua_gettop(L), w);
			// camera/effects/save functions join the ctx table; nav loaders join the nox table.
			lua_getglobal(L, "nox");
			InstallServicesApi(L, -2, -1, &w->m_services);
			lua_pop(L, 1);
			// nox.json / project / game / scene / save_authoring_json; ctx.env / request_exit.
			lua_getglobal(L, "nox");
			InstallDataApi(L, -2, -1, &w->m_dataApi);
			lua_pop(L, 1);
			NoxLua::InstallVehicleApi(L, lua_absindex(L, -1), w->m_vehicles.get());
			w->m_contextRef = luaL_ref(L, LUA_REGISTRYINDEX);
			// ui table (drawing, text measure, viewport, overlay pointer); passed to draw handlers as their second argument.
			w->m_uiRef = InstallUiApi(L, &w->m_ui);
			InstallEditorApi(L, &w->m_edBinding);
			// Globals: audio (LuaAudio.cpp), debug draw (LuaDebug.cpp), input control (LuaInputBindings.cpp).
			InstallAudioApi(L);
			InstallDebugApi(L);
			InstallInputBindingsApi(L, w);
			return 0;
		}
	};

	// ---------------------------------------------------------------------------------------------
	// Construction, loading and lifecycle
	// ---------------------------------------------------------------------------------------------

	LuaWorld::LuaWorld(const RuntimeLimits& limits, ScriptSource* source, std::unique_ptr<ScriptSource> owned)
		: m_ownedSource(std::move(owned))
		, m_runtime(std::make_unique<Runtime>(limits, m_ownedSource ? m_ownedSource.get() : source))
		, m_vehicles(std::make_unique<VehicleHost>(*m_runtime, m_ownedSource ? m_ownedSource.get() : source)) {
		m_ui.data = &m_dataApi;
		m_ui.nav = &m_nav;
		m_edBinding.undo = std::make_shared<EditorUndoRegistry>();
		m_edBinding.undo->world = this;
	}

	LuaWorld::~LuaWorld() {
		// Undo steps from ed.undo: their closures see no world from now on (see EditorUndoRegistry).
		if (m_edBinding.undo) {
			EditorUndoRegistry& undo = *m_edBinding.undo;
			if (m_runtime && m_runtime->Ok()) {
				for (const auto& [id, refs] : undo.entries) {
					(void)id;
					m_runtime->Unref(refs.first);
					m_runtime->Unref(refs.second);
				}
			}
			undo.entries.clear();
			undo.world = nullptr;
		}
		// Release registry references before the VM closes.
		if (m_runtime && m_runtime->Ok()) {
			if (m_configureRef) m_runtime->Unref(m_configureRef);
			if (m_saveGameRef) m_runtime->Unref(m_saveGameRef);
			for (SystemEntry& s : m_systems) m_runtime->Unref(s.ref);
			for (BehaviorEntry& b : m_behaviors) {
				for (int ref : { b.ref, b.fixedRef, b.startRef, b.destroyRef }) if (ref) m_runtime->Unref(ref);
			}
			for (Task& t : m_tasks) ReleaseTask(t);
			for (auto& [name, list] : m_handlers) {
				(void)name;
				for (HandlerEntry& h : list) m_runtime->Unref(h.ref);
			}
			for (PendingEvent& e : m_events) m_runtime->Unref(e.payloadRef);
			for (auto& [schema, ref] : m_migrationRefs) {
				(void)schema;
				m_runtime->Unref(ref);
			}
			m_runtime->Unref(m_contextRef);
		}
	}

	ComponentManager* LuaWorld::Manager() const {
		return m_scene ? m_scene->GetComponentManager() : nullptr;
	}

	void LuaWorld::Log(const std::string& line) {
		m_runtime->Print(line);
	}

	void LuaWorld::AddDiagnostic(Diagnostic diagnostic) {
		// Errors also go to the log: a standalone or packaged run has no editor panel that shows them.
		if (diagnostic.severity == Diagnostic::Severity::Error) {
			std::cerr << "[Lua] error in " << diagnostic.source << " (frame " << diagnostic.frame << "): "
				<< (diagnostic.file.empty() ? std::string() : diagnostic.file + ":" + std::to_string(diagnostic.line) + ": ")
				<< diagnostic.message << std::endl;
		}
		if (m_diagnostics.size() >= kMaxDiagnostics) {
			m_diagnostics.erase(m_diagnostics.begin());
		}
		m_diagnostics.push_back(std::move(diagnostic));
	}

	void LuaWorld::DiagnoseRunError(const std::string& source, const RunError& error, Diagnostic::Severity severity) {
		Diagnostic d;
		d.severity = severity;
		d.source = source;
		d.frame = m_frame;
		const size_t newline = error.message.find('\n');
		const std::string firstLine = error.message.substr(0, newline);
		if (newline != std::string::npos) d.traceback = error.message.substr(newline + 1);
		SplitLocation(firstLine, d.file, d.line, d.message);
		if (error.budgetExceeded) {
			d.message = "instruction budget exceeded: " + d.message;
		}
		if (error.memoryExceeded) {
			d.message = "out of memory (Lua memory limit " + std::to_string(m_runtime->Limits().memoryBytes) + " bytes)";
		}
		AddDiagnostic(std::move(d));
	}

	void LuaWorld::SetSceneDocuments(const nlohmann::json& game, const nlohmann::json& root) {
		m_dataApi.game = game;
		m_dataApi.root = root;
		// game.collision.files: glTF files the scene names; nox.collision_file accepts them (logical paths).
		m_services.sceneCollisionFiles.clear();
		if (game.is_object() && game.contains("collision") && game["collision"].is_object() &&
			game["collision"].contains("files") && game["collision"]["files"].is_array()) {
			for (const nlohmann::json& file : game["collision"]["files"]) {
				if (!file.is_string()) continue;
				std::string error;
				if (const auto logical = AssetIO::Normalize(file.get<std::string>(), &error)) {
					m_services.sceneCollisionFiles.push_back(*logical);
				}
			}
		}
	}

	bool LuaWorld::RunConfigureHook(std::string& error) {
		if (m_configureRef == 0) return true;
		if (m_configureFaulted) {
			error = "nox.on_configure failed earlier (see diagnostics)";
			return false;
		}
		const bool ok = RunCallback(m_configureRef, "on_configure", [this](lua_State* L) {
			PushJsonValue(L, m_dataApi.game);
			PushJsonValue(L, m_dataApi.root);
			return 2;
		}, &m_configureFaulted);
		if (!ok) error = "nox.on_configure failed (see diagnostics)";
		return ok;
	}

	bool LuaWorld::SaveGameJson(nlohmann::json& out, std::string& error) {
		if (m_saveGameRef == 0) {
			error = "no nox.on_save_game hook";
			return false;
		}
		if (m_saveGameFaulted) {
			error = "nox.on_save_game failed earlier (see diagnostics)";
			return false;
		}
		m_commands.clear();
		m_inCallback = true;
		RunError runError;
		const bool called = m_runtime->CallRef(m_saveGameRef, [](lua_State*) { return 0; }, 1, &runError);
		m_inCallback = false;
		if (!called) {
			m_saveGameFaulted = true;
			m_commands.clear();
			DiagnoseRunError("save_game", runError, Diagnostic::Severity::Error);
			error = "nox.on_save_game failed (see diagnostics)";
			return false;
		}
		if (!m_commands.empty()) {
			AddDiagnosticNote(Diagnostic::Severity::Warning, "save_game", "save hooks cannot change the world; queued commands were discarded");
			m_commands.clear();
		}
		lua_State* L = m_runtime->State();
		const bool converted = LuaValueToJson(L, -1, out, error);
		lua_pop(L, 1);
		return converted;
	}

	bool LuaWorld::Load(const ProjectContent& project, SceneGraph& scene, const nlohmann::json& authoredEntities) {
		ResetServices(m_services, project.roads, project.collision);
		m_scene = &scene;
		m_nodeCache.clear();
		{
			std::vector<std::string> roadFiles;
			for (const DeclaredFile& road : project.roads) roadFiles.push_back(road.path);
			ApplyProjectPolicy(m_dataApi, project.projectDir, project.scriptRoots, roadFiles, project.authoringRoots, project.descriptor);
		}
		// Rig slots in vehicle definitions name scene nodes; resolve them in this world's scene.
		m_vehicles->SetNodeResolver([this](const std::string& name) -> std::shared_ptr<SceneNode> {
			if (!m_scene || !m_scene->GetRoot()) return nullptr;
			std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> find =
				[&](const std::shared_ptr<SceneNode>& node) -> std::shared_ptr<SceneNode> {
				if (!node) return nullptr;
				if (node->GetName() == name) return node;
				for (const auto& child : node->children) if (auto hit = find(child)) return hit;
				return nullptr;
			};
			return find(m_scene->GetRoot());
		});
		m_loaded = false;
		m_diagnostics.clear();
		if (!m_runtime->Ok()) {
			Diagnostic d;
			d.source = "load";
			d.message = m_runtime->InitError();
			AddDiagnostic(std::move(d));
			return false;
		}
		m_runtime->SetScriptRoots(project.scriptRoots);
		std::string error;
		SliceTask::YieldIfOverBudget();  // slice point: before the API install (a load slice may pause here)
		if (!InstallApi(error)) {
			Diagnostic d;
			d.source = "load";
			d.message = error;
			AddDiagnostic(std::move(d));
			return false;
		}
		SliceTask::YieldIfOverBudget();  // slice point: after the API install, before the entry script
		m_phase = Phase::Loading;
		const bool entryOk = RunEntry(project, error);
		m_phase = Phase::Running;
		if (!entryOk) return false;

		std::stable_sort(m_systems.begin(), m_systems.end(), [](const SystemEntry& a, const SystemEntry& b) {
			if (a.order != b.order) return a.order < b.order;
			return a.sequence < b.sequence;
		});
		ApplyAuthored(scene, authoredEntities);
		m_authoredData = m_data;
		m_authoredAttachments = m_attachments;
		m_loaded = true;
		return true;
	}

	bool LuaWorld::InstallApi(std::string& error) {
		RunError runError;
		if (!m_runtime->ProtectedCall(LuaApiBinder::InstallInner, this, &runError)) {
			error = runError.message;
			return false;
		}
		return true;
	}

	bool LuaWorld::RunEntry(const ProjectContent& project, std::string& error) {
		std::string text;
		if (!m_runtime->ReadSource(project.entry, text, error)) {
			Diagnostic d;
			d.source = "load";
			d.file = project.entry;
			d.message = "cannot read entry module: " + error;
			AddDiagnostic(std::move(d));
			return false;
		}
		int ref = 0;
		if (!m_runtime->Compile(text, project.entry, ref, error)) {
			RunError compileError;
			compileError.message = error;
			DiagnoseRunError("load", compileError, Diagnostic::Severity::Error);
			return false;
		}
		RunError runError;
		const bool ok = m_runtime->CallRef(ref, nullptr, 0, &runError);
		m_runtime->Unref(ref);
		if (!ok) {
			DiagnoseRunError("load", runError, Diagnostic::Severity::Error);
			return false;
		}
		return true;
	}

	void LuaWorld::ApplyAuthored(SceneGraph& scene, const nlohmann::json& entities) {
		(void)scene;
		if (!entities.is_object()) return;
		for (const auto& [nodeName, schemas] : entities.items()) {
			const EntityHandle h = FindByName(nodeName);
			if (h.id == INVALID_ENTITY) {
				Diagnostic d;
				d.severity = Diagnostic::Severity::Warning;
				d.source = "authored";
				d.message = "no scene node named '" + nodeName + "' for authored data";
				AddDiagnostic(std::move(d));
				continue;
			}
			if (!schemas.is_object()) continue;
			for (const auto& [schemaName, fields] : schemas.items()) {
				const Schema* schema = FindSchema(schemaName);
				if (!schema || !fields.is_object()) {
					Diagnostic d;
					d.source = "authored";
					d.message = "entity '" + nodeName + "': unknown schema '" + schemaName + "'";
					AddDiagnostic(std::move(d));
					continue;
				}
				FieldMap values = schema->defaults;
				for (const auto& [field, json] : fields.items()) {
					const auto it = schema->fields.find(field);
					Value value;
					std::string error;
					if (it == schema->fields.end()) {
						error = "unknown field";
					}
					else if (!ReadJsonValue(json, it->second, value, error)) {
						// error set by ReadJsonValue
					}
					else {
						values[field] = value;
						continue;
					}
					Diagnostic d;
					d.source = "authored";
					d.message = "entity '" + nodeName + "' " + schemaName + "." + field + ": " + error;
					AddDiagnostic(std::move(d));
				}
				SetDeclaredRecord(h, schemaName, values);
			}
		}
	}

	void LuaWorld::BeginPlay() {
		if (!m_loaded || m_playing) return;
		m_playing = true;
		m_frame = 0;
		m_playTime = 0.0;
		m_teleportPending = false;
		// begin_play(ctx, payload): the scene payload (plain data) or nil. The table is fresh for every play.
		int payloadRef = LUA_REFNIL;
		if (m_scenePayload) {
			lua_State* L = m_runtime->State();
			PushJsonValue(L, *m_scenePayload);
			payloadRef = luaL_ref(L, LUA_REGISTRYINDEX);
		}
		DispatchEvents({ PendingEvent{ "begin_play", payloadRef } });
	}

	void LuaWorld::EndPlay() {
		if (!m_playing) return;
		DispatchEvents({ PendingEvent{ "end_play", LUA_REFNIL } });
		// on_destroy for every entity whose behaviours started, while the entities still exist. Stop restores the scene.
		std::vector<EntityHandle> started;
		for (const auto& [handle, names] : m_started) {
			(void)names;
			started.push_back(handle);
		}
		for (const EntityHandle& handle : started) {
			if (Live(handle)) EntityGoing(handle);
		}
		m_playing = false;
		for (Task& t : m_tasks) t.cancelled = true;
		SweepTasks();
		m_started.clear();
		for (PendingEvent& e : m_events) m_runtime->Unref(e.payloadRef);
		m_events.clear();
		m_commands.clear();
		DestroySpawned();
		RestoreTouched();
		m_data = m_authoredData;
		m_attachments = m_authoredAttachments;
		m_resolvedSpawns.clear();
		m_teleportPending = false;
	}

	void LuaWorld::RunSystems(bool fixed, float dt) {
		for (size_t i = 0; i < m_systems.size(); ++i) {
			SystemEntry& system = m_systems[i];
			if (system.faulted || system.fixed != fixed) continue;
			const int ref = system.ref;
			RunCallback(ref, "system '" + system.name + "'",
				[this, dt](lua_State* L) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
					lua_pushnumber(L, dt);
					return 2;
				},
				&m_systems[i].faulted);
		}
	}

	void LuaWorld::FixedUpdate(float dt, int index) {
		if (!m_loaded || !m_playing) return;
		m_substepIndex = index;
		++m_fixedSteps;
		RunSystems(true, dt);
		RunBehaviorPhase(true, dt);
		m_substepIndex = 0;
		SweepDead();
	}

	void LuaWorld::Update(float dt, bool inputEnabled) {
		m_services.fxEmitsThisFrame = 0;
		if (!m_loaded || !m_playing) return;
		++m_frame;
		m_playTime += dt;
		m_inputEnabled = inputEnabled;
		RunTimers();
		RunCoroutines();
		RunSystems(false, dt);
		PumpPhysicsEvents(*this);
		PumpAnimationEvents(*this);
		DeliverEvents();
		RunBehaviors(dt);
		SweepDead();
		SweepTasks();
	}

	bool LuaWorld::ConsumeTeleport() {
		const bool value = m_teleportPending || m_services.cameraCut;
		m_teleportPending = false;
		m_services.cameraCut = false;
		return value;
	}

	std::vector<std::string> LuaWorld::SystemNames() const {
		std::vector<std::string> names;
		for (const SystemEntry& s : m_systems) names.push_back(s.name);
		return names;
	}

	std::vector<std::string> LuaWorld::FaultedNames() const {
		std::vector<std::string> names;
		for (const SystemEntry& s : m_systems) if (s.faulted) names.push_back("system:" + s.name);
		for (const BehaviorEntry& b : m_behaviors) if (b.faulted) names.push_back("behavior:" + b.name);
		for (const auto& [event, list] : m_handlers) {
			for (const HandlerEntry& h : list) if (h.faulted) names.push_back("event:" + event);
		}
		return names;
	}

	void LuaWorld::RetryFaulted() {
		for (SystemEntry& s : m_systems) s.faulted = false;
		for (BehaviorEntry& b : m_behaviors) b.faulted = false;
		for (auto& [event, list] : m_handlers) {
			(void)event;
			for (HandlerEntry& h : list) h.faulted = false;
		}
		for (DrawEntry& d : m_draws) d.faulted = false;
		for (EditorPanelDecl& p : m_editorPanels) p.faulted = false;
		m_editorViewportFaulted = false;
	}

	// ---------------------------------------------------------------------------------------------
	// Callbacks, commands and events
	// ---------------------------------------------------------------------------------------------

	void LuaWorld::AddEditorPanel(const std::string& id, const std::string& title, int ref) {
		for (EditorPanelDecl& panel : m_editorPanels) {
			if (panel.id == id) {
				// Same id declared twice in one project: the last declaration wins (the earlier draw function is released).
				luaL_unref(m_runtime->State(), LUA_REGISTRYINDEX, panel.ref);
				panel.title = title;
				panel.ref = ref;
				panel.faulted = false;
				return;
			}
		}
		if (m_editorPanels.size() >= editor_limits::kMaxPanels) {
			luaL_unref(m_runtime->State(), LUA_REGISTRYINDEX, ref);
			AddDiagnosticNote(Diagnostic::Severity::Warning, "editor_panel", "too many editor panels (max " + std::to_string(editor_limits::kMaxPanels) + ")");
			return;
		}
		EditorPanelDecl panel;
		panel.id = id;
		panel.title = title;
		panel.ref = ref;
		m_editorPanels.push_back(panel);
	}

	void LuaWorld::SetEditorViewport(int ref) {
		if (m_editorViewportRef != 0) luaL_unref(m_runtime->State(), LUA_REGISTRYINDEX, m_editorViewportRef);
		m_editorViewportRef = ref;
		m_editorViewportFaulted = false;
	}

	bool LuaWorld::RunEditorCallback(int ref, const std::string& label, bool* faultFlag, EditorHost* host, EditorHandles* handles, bool inViewport) {
		m_edBinding.host = host;
		m_edBinding.handles = handles;
		m_edBinding.inViewport = inViewport;
		m_edBinding.treeDepth = 0;
		m_inCallback = true;
		m_dataApi.editorWrite = true;   // nox.save_authoring_json is allowed inside editor callbacks only
		RunError error;
		const bool ok = m_runtime->CallRef(ref, [this](lua_State* L) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
			lua_rawgeti(L, LUA_REGISTRYINDEX, m_edBinding.tableRef);
			return 2;
		}, 0, &error);
		m_dataApi.editorWrite = false;
		m_inCallback = false;
		m_edBinding.host = nullptr;
		m_edBinding.handles = nullptr;
		m_edBinding.inViewport = false;
		if (!ok) {
			if (faultFlag) *faultFlag = true;
			m_commands.clear();
			DiagnoseRunError(label, error, Diagnostic::Severity::Error);
			return false;
		}
		if (!m_commands.empty()) {
			AddDiagnosticNote(Diagnostic::Severity::Warning, label, "editor code cannot change the world; queued commands were discarded");
			m_commands.clear();
		}
		return true;
	}

	bool LuaWorld::DrawEditorPanel(const std::string& id, EditorHost* host, EditorHandles* handles) {
		for (EditorPanelDecl& panel : m_editorPanels) {
			if (panel.id != id) continue;
			if (panel.faulted || !m_loaded) return false;
			return RunEditorCallback(panel.ref, "editor panel '" + id + "'", &panel.faulted, host, handles, false);
		}
		return false;
	}

	bool LuaWorld::DrawEditorViewport(EditorHost* host, EditorHandles* handles) {
		if (m_editorViewportRef == 0 || m_editorViewportFaulted || !m_loaded) return false;
		return RunEditorCallback(m_editorViewportRef, "editor viewport", &m_editorViewportFaulted, host, handles, true);
	}

	void LuaWorld::RunEditorUndo(int ref, const std::string& label) {
		if (ref == 0 || !m_runtime || !m_runtime->Ok()) return;
		m_commands.clear();
		m_inCallback = true;
		RunError error;
		const bool ok = m_runtime->CallRef(ref, [](lua_State*) { return 0; }, 0, &error);
		m_inCallback = false;
		if (!ok) {
			m_commands.clear();
			DiagnoseRunError("editor undo (" + label + ")", error, Diagnostic::Severity::Error);
			return;
		}
		if (!m_commands.empty()) {
			AddDiagnosticNote(Diagnostic::Severity::Warning, "editor undo", "undo steps cannot change the world; queued commands were discarded");
			m_commands.clear();
		}
	}

	void LuaWorld::AddDrawHandler(int ref) {
		DrawEntry entry;
		entry.ref = ref;
		entry.index = m_draws.size() + 1;
		m_draws.push_back(entry);
	}

	void LuaWorld::BeginUiInput(const InputDevices& devices, bool enabled) {
		m_nav.Sample(devices, enabled);
		ui::Pointer& pointer = ui::Pointer::Get();
		const bool pressed = pointer.Pressed(ui::PointerButton::Left) || pointer.Pressed(ui::PointerButton::Right) ||
			pointer.Pressed(ui::PointerButton::Middle);
		m_nav.Resolve(pointer.Position(), enabled && pointer.InWindow(), pointer.Moved(), enabled && pressed);
		// Escape / pad B reaches the handlers of "ui.back" on the next update (and ctx:ui_back_pressed() at once).
		if (m_nav.Back() && m_playing) QueueEvent("ui.back", LUA_REFNIL);
	}

	void LuaWorld::AddNavigationWidget(uint32_t id, const glm::vec2& min, const glm::vec2& max) {
		UiNavigator::Widget w;
		w.id = id;
		w.min = min;
		w.max = max;
		m_nav.AppendWidget(w);
	}

	bool LuaWorld::QueueUiEvent(const std::string& name, const std::string& id, const std::string& action, int entered) {
		if (!m_runtime || !m_runtime->Ok()) return false;
		if (m_events.size() >= kMaxEventsPerFrame) return false;
		lua_State* L = m_runtime->State();
		lua_newtable(L);
		lua_pushlstring(L, id.data(), id.size());
		lua_setfield(L, -2, "id");
		if (entered >= 0) {
			lua_pushboolean(L, entered != 0);
			lua_setfield(L, -2, "entered");
		}
		else {
			lua_pushlstring(L, action.data(), action.size());
			lua_setfield(L, -2, "action");
		}
		const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
		return QueueEvent(name, ref);
	}

	void LuaWorld::DrawFrame(float dt, const glm::vec2& viewport, const glm::vec2& pointerOverlay, UiRecorder& out) {
		out.Begin();
		// The widget layout of this draw becomes the navigation layout of the next frame (empty when nothing is drawn).
		m_nav.BeginLayout();
		m_ui.origin = m_services.overlayOrigin;
		if (!m_loaded || m_draws.empty()) {
			m_nav.EndLayout();
			return;
		}
		m_ui.recorder = &out;
		m_ui.viewport = viewport;
		m_ui.pointerOverlay = pointerOverlay;
		for (DrawEntry& entry : m_draws) {
			if (entry.faulted) continue;
			const std::string label = "draw handler #" + std::to_string(entry.index);
			m_inCallback = true;
			RunError error;
			const bool ok = m_runtime->CallRef(entry.ref, [this, dt](lua_State* L) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
				lua_rawgeti(L, LUA_REGISTRYINDEX, m_uiRef);
				lua_pushnumber(L, dt);
				return 3;
			}, 0, &error);
			m_inCallback = false;
			if (!ok) {
				entry.faulted = true;
				m_commands.clear();
				DiagnoseRunError(label, error, Diagnostic::Severity::Error);
				continue;
			}
			if (!m_commands.empty()) {
				// Draw handlers read the world; their queued writes are dropped, reported once until they stop.
				if (!entry.warnedCommands) {
					AddDiagnosticNote(Diagnostic::Severity::Warning, label,
						"draw handlers cannot change the world; queued commands were discarded");
					entry.warnedCommands = true;
				}
				m_commands.clear();
			}
			else {
				entry.warnedCommands = false;
			}
		}
		m_ui.recorder = nullptr;
		m_nav.EndLayout();
		if (out.Dropped() > 0) {
			if (!m_drawDropReported) {
				AddDiagnosticNote(Diagnostic::Severity::Warning, "draw",
					"ui command budget exceeded (" + std::to_string(ui_limits::kMaxCommands) + " commands, " +
					std::to_string(ui_limits::kMaxPoints) + " points per frame); extra commands were dropped");
				m_drawDropReported = true;
			}
		}
		else {
			m_drawDropReported = false;
		}
	}

	bool LuaWorld::RunCallback(int ref, const std::string& label, const std::function<int(lua_State*)>& pushArgs,
		bool* faultFlag) {
		m_commands.clear();
		m_inCallback = true;
		m_worldWritable = true;
		RunError error;
		const bool ok = m_runtime->CallRef(ref, pushArgs, 0, &error);
		m_worldWritable = false;
		m_inCallback = false;
		if (!ok) {
			// The failing callback's native commands are discarded; Lua table changes are not rolled back.
			m_commands.clear();
			if (faultFlag) *faultFlag = true;
			DiagnoseRunError(label, error, Diagnostic::Severity::Error);
			return false;
		}
		ApplyCommands();
		return true;
	}

	void LuaWorld::QueueCommand(Command command) {
		m_commands.push_back(std::move(command));
	}

	void LuaWorld::PushInputDeviceEvents(const std::vector<InputMap::DeviceEvent>& events) {
		constexpr size_t kMaxDeviceLog = 32;
		for (const InputMap::DeviceEvent& e : events) {
			m_deviceLog.push_back(e);
			if (m_deviceLog.size() > kMaxDeviceLog) m_deviceLog.erase(m_deviceLog.begin());
			if (!m_loaded || !m_playing || !m_runtime || !m_runtime->Ok()) continue;
			lua_State* L = m_runtime->State();
			lua_createtable(L, 0, 3);
			lua_pushboolean(L, e.connected ? 1 : 0);
			lua_setfield(L, -2, "connected");
			lua_pushinteger(L, e.player);
			lua_setfield(L, -2, "player");
			lua_pushstring(L, e.name.c_str());
			lua_setfield(L, -2, "name");
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			if (!QueueEvent("input.device", ref)) m_runtime->Unref(ref);
		}
	}

	std::vector<InputMap::DeviceEvent> LuaWorld::TakeInputDeviceLog() {
		std::vector<InputMap::DeviceEvent> out;
		out.swap(m_deviceLog);
		return out;
	}

	bool LuaWorld::QueueEvent(const std::string& name, int payloadRef) {
		if (m_events.size() >= kMaxEventsPerFrame) return false;
		m_events.push_back(PendingEvent{ name, payloadRef });
		return true;
	}

	bool LuaWorld::ResolveSpawn(int64_t token, EntityHandle& out) const {
		const auto it = m_resolvedSpawns.find(token);
		if (it == m_resolvedSpawns.end()) return false;
		if (!Manager() || !Manager()->IsHandleValid(it->second)) return false;
		out = it->second;
		return true;
	}

	void LuaWorld::TouchTransform(const EntityHandle& handle) {
		if (m_touched.count(handle)) return;
		const TransformComponent* transform = Manager()->GetTransform(handle.id);
		if (transform) m_touched[handle] = transform->localTransform;
	}

	void LuaWorld::RestoreTouched() {
		if (m_scene) {
			TransformSystem* ts = m_scene->GetTransformSystem();
			for (const auto& [handle, local] : m_touched) {
				if (Manager() && Manager()->IsHandleValid(handle)) ts->SetLocalTransform(handle.id, local);
			}
		}
		m_touched.clear();
	}

	void LuaWorld::DestroySpawned() {
		ComponentManager* cm = Manager();
		if (cm) {
			for (const EntityHandle& h : m_spawned) {
				if (cm->IsHandleValid(h)) cm->DestroyEntity(h.id);
			}
		}
		m_spawned.clear();
		SweepDead();
	}

	void LuaWorld::ApplyCommands() {
		std::vector<Command> commands;
		commands.swap(m_commands);
		ComponentManager* cm = Manager();
		if (!cm || !m_scene) return;
		TransformSystem* ts = m_scene->GetTransformSystem();

		auto warn = [this](const std::string& message) {
			Diagnostic d;
			d.severity = Diagnostic::Severity::Warning;
			d.source = "command";
			d.message = message;
			d.frame = m_frame;
			AddDiagnostic(std::move(d));
		};

		for (const Command& cmd : commands) {
			switch (cmd.kind) {
			case Command::Kind::Spawn: {
				const EntityHandle existing = FindByName(cmd.name);
				if (existing.id != INVALID_ENTITY) {
					warn("spawn: a node named '" + cmd.name + "' already exists");
					break;
				}
				if (m_spawned.size() >= kMaxSpawnedPerPlay) {
					warn("spawn: limit of " + std::to_string(kMaxSpawnedPerPlay) + " script entities reached");
					break;
				}
				const EntityID id = cm->CreateEntity(cmd.name);
				const EntityHandle h = cm->MakeHandle(id);
				m_spawned.push_back(h);
				m_resolvedSpawns[cmd.token] = h;
				break;
			}
			case Command::Kind::Destroy: {
				if (!cm->IsHandleValid(cmd.target)) {
					warn("destroy: target handle is stale");
					break;
				}
				if (std::find(m_spawned.begin(), m_spawned.end(), cmd.target) == m_spawned.end()) {
					// Scene nodes (authored or instantiated during play) are removed by the host. Stop restores authored
					// nodes; nodes created during play are removed for good. on_destroy runs while the subtree still exists.
					const nox::reflect::EngineServices& services = nox::reflect::Services();
					std::shared_ptr<SceneNode> node = FindSceneNode(cmd.target.id);
					if (node && services.destroyNode) {
						std::vector<EntityHandle> subtree;
						CollectSubtree(node, subtree);
						for (const EntityHandle& h : subtree) EntityGoing(h);
						if (!cm->IsHandleValid(cmd.target)) break;   // a hook destroyed it already
						node = FindSceneNode(cmd.target.id);
						if (node && services.destroyNode(node)) {
							m_data.erase(cmd.target);
							m_attachments.erase(cmd.target);
							break;
						}
					}
					warn("destroy: the target is neither a script-spawned entity nor a scene node the host can remove");
					break;
				}
				EntityGoing(cmd.target);
				if (!cm->IsHandleValid(cmd.target)) break;
				const auto spawned = std::find(m_spawned.begin(), m_spawned.end(), cmd.target);
				if (spawned != m_spawned.end()) m_spawned.erase(spawned);
				m_data.erase(cmd.target);
				m_attachments.erase(cmd.target);
				cm->DestroyEntity(cmd.target.id);
				break;
			}
			case Command::Kind::Move:
			case Command::Kind::Teleport:
			case Command::Kind::SetScale: {
				if (!cm->IsHandleValid(cmd.target)) {
					warn("transform write: target handle is stale");
					break;
				}
				TouchTransform(cmd.target);
				if (cmd.kind == Command::Kind::SetScale) {
					ts->SetScale(cmd.target.id, cmd.value);
				}
				else {
					ts->SetPosition(cmd.target.id, cmd.value);
					if (cmd.kind == Command::Kind::Teleport) m_teleportPending = true;
				}
				break;
			}
			}
		}
	}

	void LuaWorld::DispatchEvents(const std::vector<PendingEvent>& events) {
		m_lastEventCount = events.size();
		for (const PendingEvent& event : events) {
			const auto it = m_handlers.find(event.name);
			if (it != m_handlers.end()) {
				std::vector<HandlerEntry>& list = it->second;
				for (size_t i = 0; i < list.size(); ++i) {
					if (list[i].faulted) continue;
					const int payload = event.payloadRef;
					RunCallback(list[i].ref, "event '" + event.name + "'",
						[this, payload](lua_State* L) {
							lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
							if (payload == LUA_REFNIL) lua_pushnil(L);
							else lua_rawgeti(L, LUA_REGISTRYINDEX, payload);
							return 2;
						},
						&list[i].faulted);
				}
			}
		}
		for (const PendingEvent& event : events) m_runtime->Unref(event.payloadRef);
	}

	void LuaWorld::DeliverEvents() {
		std::vector<PendingEvent> events;
		events.swap(m_events);
		DispatchEvents(events);
	}

	void LuaWorld::RunBehaviors(float dt) {
		RunBehaviorPhase(false, dt);
	}

	// update (frame) or fixed_update (substep) of every attached behaviour. on_start runs first, once per entity.
	void LuaWorld::RunBehaviorPhase(bool fixed, float dt) {
		for (size_t b = 0; b < m_behaviors.size(); ++b) {
			if (m_behaviors[b].faulted) continue;
			const std::string name = m_behaviors[b].name;
			const int ref = fixed ? m_behaviors[b].fixedRef : m_behaviors[b].ref;
			std::vector<EntityHandle> targets;
			for (const auto& [handle, names] : m_attachments) {
				if (names.count(name) && Live(handle)) targets.push_back(handle);
			}
			for (const EntityHandle& target : targets) {
				if (m_behaviors[b].faulted) break;
				if (!Live(target)) continue;
				if (!EnsureStarted(b, target)) continue;
				if (m_behaviors[b].faulted) break;
				if (ref == 0) continue;
				RunCallback(ref, "behavior '" + name + "'",
					[this, target, dt](lua_State* L) {
						lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
						LuaApiBinder::PushHandle(L, target);
						lua_pushnumber(L, dt);
						return 3;
					},
					&m_behaviors[b].faulted);
			}
		}
	}

	// Runs on_start once for (behaviour, entity). The entity is marked before the call, so it never runs twice.
	bool LuaWorld::EnsureStarted(size_t b, const EntityHandle& target) {
		const std::string name = m_behaviors[b].name;
		std::set<std::string>& started = m_started[target];
		if (started.count(name)) return true;
		started.insert(name);
		if (m_behaviors[b].startRef == 0) return true;
		RunCallback(m_behaviors[b].startRef, "behavior '" + name + "' on_start",
			[this, target](lua_State* L) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
				LuaApiBinder::PushHandle(L, target);
				return 2;
			},
			&m_behaviors[b].faulted);
		return !m_behaviors[b].faulted;
	}

	// on_destroy for the behaviours that started on a live entity, then its timers and coroutines are cancelled.
	void LuaWorld::EntityGoing(const EntityHandle& handle) {
		CancelTasksOf(handle);
		const auto started = m_started.find(handle);
		if (started == m_started.end()) return;
		const std::vector<std::string> names(started->second.begin(), started->second.end());
		m_started.erase(started);   // first: a hook that destroys the entity again runs nothing twice
		for (const std::string& name : names) {
			if (!Live(handle)) return;
			const int index = FindBehavior(name);
			if (index < 0 || m_behaviors[index].destroyRef == 0 || m_behaviors[index].faulted) continue;
			RunCallback(m_behaviors[index].destroyRef, "behavior '" + name + "' on_destroy",
				[this, handle](lua_State* L) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
					LuaApiBinder::PushHandle(L, handle);
					return 2;
				},
				&m_behaviors[index].faulted);
		}
	}

	void LuaWorld::CollectSubtree(const std::shared_ptr<SceneNode>& node, std::vector<EntityHandle>& out) const {
		if (!node) return;
		if (node->GetEntityID() != INVALID_ENTITY && Manager()) {
			const EntityHandle h = Manager()->MakeHandle(node->GetEntityID());
			if (Manager()->IsHandleValid(h)) out.push_back(h);
		}
		for (const auto& child : node->children) CollectSubtree(child, out);
	}

	// ---------------------------------------------------------------------------------------------
	// Timers and coroutines. The clock is the play clock (m_playTime): it advances in Update only, so pause freezes it
	// and NOX_FIXED_DT makes it deterministic. Order inside Update: timers, coroutines, systems, events, behaviors.
	// ---------------------------------------------------------------------------------------------

	namespace {
		constexpr double kClockEpsilon = 1e-9;   // dt is a float: a due time reached within this margin fires
	}

	bool LuaWorld::Live(const EntityHandle& handle) const {
		return Manager() && Manager()->IsHandleValid(handle);
	}

	int LuaWorld::FindBehavior(const std::string& name) const {
		for (size_t i = 0; i < m_behaviors.size(); ++i) {
			if (m_behaviors[i].name == name) return static_cast<int>(i);
		}
		return -1;
	}

	uint64_t LuaWorld::AddTask(Task task) {
		task.id = m_nextTaskId++;
		m_tasks.push_back(std::move(task));
		return m_tasks.back().id;
	}

	LuaWorld::Task* LuaWorld::FindTask(uint64_t id) {
		for (Task& t : m_tasks) {
			if (t.id == id) return &t;
		}
		return nullptr;
	}

	void LuaWorld::CancelTasksOf(const EntityHandle& handle) {
		for (Task& t : m_tasks) {
			if (t.hasEntity && t.entity == handle) t.cancelled = true;
		}
	}

	void LuaWorld::ReleaseTask(Task& task) {
		if (m_runtime && m_runtime->Ok()) {
			if (task.ref) m_runtime->Unref(task.ref);
			if (task.predicateRef) m_runtime->Unref(task.predicateRef);
		}
		task.ref = 0;
		task.predicateRef = 0;
		task.thread = nullptr;
	}

	// Removes cancelled tasks and those whose entity is gone. Only called between callbacks.
	void LuaWorld::SweepTasks() {
		for (Task& t : m_tasks) {
			if (t.hasEntity && !Live(t.entity)) t.cancelled = true;
		}
		for (Task& t : m_tasks) {
			if (t.cancelled) ReleaseTask(t);
		}
		m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [](const Task& t) { return t.cancelled; }), m_tasks.end());
	}

	// Timers created by these callbacks first fire in the next frame (the loop is bounded by the count at entry).
	void LuaWorld::RunTimers() {
		const size_t count = m_tasks.size();
		for (size_t i = 0; i < count && i < m_tasks.size(); ++i) {
			if (m_tasks[i].kind != Task::Kind::Timer || m_tasks[i].cancelled) continue;
			if (m_tasks[i].hasEntity && !Live(m_tasks[i].entity)) {
				m_tasks[i].cancelled = true;
				continue;
			}
			if (m_playTime + kClockEpsilon < m_tasks[i].due) continue;
			const int ref = m_tasks[i].ref;
			const bool scoped = m_tasks[i].hasEntity;
			const EntityHandle entity = m_tasks[i].entity;
			if (m_tasks[i].interval > 0.0) {
				m_tasks[i].due += m_tasks[i].interval;
				if (m_tasks[i].due <= m_playTime) m_tasks[i].due = m_playTime + m_tasks[i].interval;
			}
			else {
				m_tasks[i].cancelled = true;   // a one-shot timer is spent before its callback runs
			}
			bool faulted = false;
			RunCallback(ref, "timer",
				[this, scoped, entity](lua_State* L) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_contextRef);
					if (scoped) LuaApiBinder::PushHandle(L, entity);
					else lua_pushnil(L);
					return 2;
				},
				&faulted);
			if (faulted) m_tasks[i].cancelled = true;   // a failing timer stops; the diagnostic says why
		}
	}

	// wait_until predicates run as callbacks (fn(ctx, entity) -> boolean). A failure cancels the coroutine.
	void LuaWorld::RunCoroutines() {
		const size_t count = m_tasks.size();
		for (size_t i = 0; i < count && i < m_tasks.size(); ++i) {
			if (m_tasks[i].kind != Task::Kind::Coroutine || m_tasks[i].cancelled) continue;
			if (m_tasks[i].hasEntity && !Live(m_tasks[i].entity)) {
				m_tasks[i].cancelled = true;
				continue;
			}
			if (m_tasks[i].started) {
				bool ready = false;
				switch (m_tasks[i].wait) {
				case Task::Wait::None:
					ready = true;
					break;
				case Task::Wait::Time:
					ready = m_playTime + kClockEpsilon >= m_tasks[i].until;
					break;
				case Task::Wait::Frames:
					ready = --m_tasks[i].framesLeft <= 0;
					break;
				case Task::Wait::Until: {
					const int predicate = m_tasks[i].predicateRef;
					const bool scoped = m_tasks[i].hasEntity;
					const EntityHandle entity = m_tasks[i].entity;
					lua_State* L = m_runtime->State();
					m_commands.clear();
					m_inCallback = true;
					m_worldWritable = true;
					RunError error;
					const bool ok = m_runtime->CallRef(predicate,
						[this, scoped, entity](lua_State* state) {
							lua_rawgeti(state, LUA_REGISTRYINDEX, m_contextRef);
							if (scoped) LuaApiBinder::PushHandle(state, entity);
							else lua_pushnil(state);
							return 2;
						},
						1, &error);
					m_worldWritable = false;
					m_inCallback = false;
					if (!ok) {
						m_commands.clear();
						m_tasks[i].cancelled = true;
						DiagnoseRunError("coroutine wait_until", error, Diagnostic::Severity::Error);
						continue;
					}
					ready = lua_toboolean(L, -1) != 0;
					lua_pop(L, 1);
					ApplyCommands();
					break;
				}
				}
				if (!ready) continue;
			}
			ResumeCoroutine(i);
		}
	}

	// Runs one segment of a coroutine: up to its next wait, or to its end.
	void LuaWorld::ResumeCoroutine(size_t index) {
		lua_State* co = m_tasks[index].thread;
		if (!co) {
			m_tasks[index].cancelled = true;
			return;
		}
		int nargs = 0;
		if (!m_tasks[index].started) {
			m_tasks[index].started = true;
			lua_rawgeti(co, LUA_REGISTRYINDEX, m_contextRef);
			if (m_tasks[index].hasEntity) LuaApiBinder::PushHandle(co, m_tasks[index].entity);
			else lua_pushnil(co);
			nargs = 2;
		}
		m_commands.clear();
		m_inCallback = true;
		m_worldWritable = true;
		++m_resumeDepth;
		int nres = 0;
		RunError error;
		const bool ok = m_runtime->Resume(co, nargs, nres, &error);
		--m_resumeDepth;
		m_worldWritable = false;
		m_inCallback = false;
		if (!ok) {
			m_commands.clear();
			m_tasks[index].cancelled = true;
			DiagnoseRunError("coroutine", error, Diagnostic::Severity::Error);
			return;
		}
		if (lua_status(co) == LUA_YIELD) {
			const bool wellFormed = nres >= 2 && lua_type(co, -nres) == LUA_TSTRING;
			const std::string kind = wellFormed ? lua_tostring(co, -nres) : std::string();
			Task& t = m_tasks[index];
			if (kind == "time") {
				t.wait = Task::Wait::Time;
				t.until = m_playTime + lua_tonumber(co, -nres + 1);
			}
			else if (kind == "frames") {
				t.wait = Task::Wait::Frames;
				t.framesLeft = static_cast<int64_t>(lua_tointeger(co, -nres + 1));
			}
			else if (kind == "until") {
				t.wait = Task::Wait::Until;
				if (t.predicateRef) m_runtime->Unref(t.predicateRef);
				lua_pushvalue(co, -nres + 1);
				t.predicateRef = luaL_ref(co, LUA_REGISTRYINDEX);
			}
			else {
				Diagnostic d;
				d.severity = Diagnostic::Severity::Error;
				d.source = "coroutine";
				d.message = "coroutine yielded without ctx:wait, ctx:wait_frames or ctx:wait_until";
				d.frame = m_frame;
				AddDiagnostic(std::move(d));
				t.cancelled = true;
			}
			lua_pop(co, nres);
		}
		else {
			m_tasks[index].cancelled = true;   // finished
		}
		ApplyCommands();
	}

	void LuaWorld::SweepDead() {
		ComponentManager* cm = Manager();
		if (!cm) return;
		for (auto it = m_data.begin(); it != m_data.end();) {
			it = cm->IsHandleValid(it->first) ? std::next(it) : m_data.erase(it);
		}
		for (auto it = m_attachments.begin(); it != m_attachments.end();) {
			it = cm->IsHandleValid(it->first) ? std::next(it) : m_attachments.erase(it);
		}
		for (auto it = m_started.begin(); it != m_started.end();) {
			it = cm->IsHandleValid(it->first) ? std::next(it) : m_started.erase(it);
		}
		m_spawned.erase(std::remove_if(m_spawned.begin(), m_spawned.end(),
			[cm](const EntityHandle& h) { return !cm->IsHandleValid(h); }), m_spawned.end());
	}

	// ---------------------------------------------------------------------------------------------
	// Registration (load phase) and declared data
	// ---------------------------------------------------------------------------------------------

	void LuaWorld::AddSystem(const std::string& name, int order, int ref, bool fixed) {
		SystemEntry entry;
		entry.name = name;
		entry.order = order;
		entry.sequence = m_sequence++;
		entry.ref = ref;
		entry.fixed = fixed;
		m_systems.push_back(entry);
	}

	void LuaWorld::AddBehavior(const std::string& name, const BehaviorRefs& refs) {
		BehaviorEntry entry;
		entry.name = name;
		entry.ref = refs.update;
		entry.fixedRef = refs.fixed;
		entry.startRef = refs.start;
		entry.destroyRef = refs.destroy;
		m_behaviors.push_back(entry);
	}

	void LuaWorld::AddHandler(const std::string& eventName, int ref) {
		HandlerEntry entry;
		entry.ref = ref;
		m_handlers[eventName].push_back(entry);
	}

	bool LuaWorld::AddSchema(Schema schema, std::string& error) {
		if (m_schemas.count(schema.name)) {
			error = "schema '" + schema.name + "' is already declared";
			return false;
		}
		m_schemas[schema.name] = std::move(schema);
		return true;
	}

	const Schema* LuaWorld::FindSchema(const std::string& name) const {
		const auto it = m_schemas.find(name);
		return it == m_schemas.end() ? nullptr : &it->second;
	}

	bool LuaWorld::HasBehavior(const std::string& name) const {
		for (const BehaviorEntry& b : m_behaviors) if (b.name == name) return true;
		return false;
	}

	bool LuaWorld::AttachBehavior(const EntityHandle& handle, const std::string& behavior, std::string& error) {
		if (!HasBehavior(behavior)) {
			error = "unknown behavior '" + behavior + "'";
			return false;
		}
		m_attachments[handle].insert(behavior);
		return true;
	}

	bool LuaWorld::DetachBehavior(const EntityHandle& handle, const std::string& behavior) {
		const auto it = m_attachments.find(handle);
		if (it == m_attachments.end()) return false;
		return it->second.erase(behavior) > 0;
	}

	std::shared_ptr<SceneNode> LuaWorld::FindSceneNode(EntityID id) {
		if (!m_scene || id == INVALID_ENTITY) return nullptr;
		const SceneNode* root = m_scene->GetRoot().get();
		// A node is in the scene when its parent chain reaches the root.
		auto inScene = [root](std::shared_ptr<SceneNode> node) {
			while (node) {
				if (node.get() == root) return true;
				node = node->parentNode.lock();
			}
			return false;
		};
		const auto cached = m_nodeCache.find(id);
		if (cached != m_nodeCache.end()) {
			std::shared_ptr<SceneNode> node = cached->second.lock();
			if (node && node->GetEntityID() == id && inScene(node)) return node;
			m_nodeCache.erase(cached);
		}
		std::shared_ptr<SceneNode> node = m_scene->FindNodeByEntity(id);
		if (node) m_nodeCache[id] = node;
		return node;
	}

	EntityHandle LuaWorld::FindByName(const std::string& name) const {
		EntityHandle h;
		ComponentManager* cm = Manager();
		if (!cm || name.empty()) return h;
		// "Folder/Child": a path of node names from the scene root (folders are path segments).
		if (name.find('/') != std::string::npos) {
			if (!m_scene) return h;
			std::shared_ptr<SceneNode> node = m_scene->FindNodeByPath(name);
			if (!node || node->GetEntityID() == INVALID_ENTITY) return h;
			const EntityHandle candidate = cm->MakeHandle(node->GetEntityID());
			return cm->IsHandleValid(candidate) ? candidate : h;
		}
		const EntityID id = cm->FindEntityByName(name);
		if (id == INVALID_ENTITY) return h;
		const EntityHandle candidate = cm->MakeHandle(id);
		return cm->IsHandleValid(candidate) ? candidate : h;
	}

	bool LuaWorld::SetDeclaredField(const EntityHandle& handle, const std::string& schema, const std::string& field,
		const Value& value, std::string& error) {
		if (!Manager() || !Manager()->IsHandleValid(handle)) {
			error = "stale or invalid entity handle";
			return false;
		}
		const Schema* decl = FindSchema(schema);
		if (!decl) {
			error = "unknown schema '" + schema + "'";
			return false;
		}
		const auto typeIt = decl->fields.find(field);
		if (typeIt == decl->fields.end()) {
			error = "schema '" + schema + "' has no field '" + field + "'";
			return false;
		}
		if (!ValueMatches(value, typeIt->second)) {
			error = schema + "." + field + " expects " + FieldTypeName(typeIt->second);
			return false;
		}
		const auto recordIt = m_data.find(handle);
		if (recordIt == m_data.end() || !recordIt->second.count(schema)) {
			error = "entity has no '" + schema + "' component";
			return false;
		}
		recordIt->second[schema][field] = value;
		return true;
	}

	bool LuaWorld::GetDeclaredField(const EntityHandle& handle, const std::string& schema, const std::string& field, Value& out) const {
		const auto recordIt = m_data.find(handle);
		if (recordIt == m_data.end()) return false;
		const auto schemaIt = recordIt->second.find(schema);
		if (schemaIt == recordIt->second.end()) return false;
		const auto fieldIt = schemaIt->second.find(field);
		if (fieldIt == schemaIt->second.end()) return false;
		out = fieldIt->second;
		return true;
	}

	bool LuaWorld::HasDeclared(const EntityHandle& handle, const std::string& schema) const {
		const auto recordIt = m_data.find(handle);
		return recordIt != m_data.end() && recordIt->second.count(schema) != 0;
	}

	void LuaWorld::SetDeclaredRecord(const EntityHandle& handle, const std::string& schema, const FieldMap& values) {
		m_data[handle][schema] = values;
	}

	void LuaWorld::RemoveDeclaredRecord(const EntityHandle& handle, const std::string& schema) {
		const auto recordIt = m_data.find(handle);
		if (recordIt == m_data.end()) return;
		recordIt->second.erase(schema);
		if (recordIt->second.empty()) m_data.erase(recordIt);
	}

	const FieldMap* LuaWorld::FindDeclaredRecord(const EntityHandle& handle, const std::string& schema) const {
		const auto recordIt = m_data.find(handle);
		if (recordIt == m_data.end()) return nullptr;
		const auto schemaIt = recordIt->second.find(schema);
		return schemaIt == recordIt->second.end() ? nullptr : &schemaIt->second;
	}

	std::vector<EntityHandle> LuaWorld::DataHandlesWithSchema(const std::string& schema) const {
		std::vector<EntityHandle> result;
		for (const auto& [handle, schemas] : m_data) {
			if (schemas.count(schema) && Manager() && Manager()->IsHandleValid(handle)) result.push_back(handle);
		}
		return result;
	}

	// ---------------------------------------------------------------------------------------------
	// Reload support: migration, snapshot, import
	// ---------------------------------------------------------------------------------------------

	bool LuaWorld::AddMigration(const std::string& schema, int ref, std::string& error) {
		if (!FindSchema(schema)) {
			error = "nox.migrate: unknown schema '" + schema + "' (declare it with nox.schema first)";
			return false;
		}
		if (m_migrationRefs.count(schema)) {
			error = "nox.migrate: schema '" + schema + "' already has a migration";
			return false;
		}
		m_migrationRefs[schema] = ref;
		return true;
	}

	bool LuaWorld::MigrateRecord(const std::string& schema, int fromVersion, const FieldMap& in, FieldMap& out, std::string& error) {
		const Schema* decl = FindSchema(schema);
		if (!decl) {
			error = "schema '" + schema + "' is not declared";
			return false;
		}
		bool changed = fromVersion != decl->version;
		for (const auto& [field, value] : in) {
			const auto it = decl->fields.find(field);
			if (it == decl->fields.end() || !ValueMatches(value, it->second)) changed = true;
		}
		out.clear();
		if (!changed) {
			out = in;
			for (const auto& [field, type] : decl->fields) {
				(void)type;
				if (out.count(field)) continue;
				const auto def = decl->defaults.find(field);
				if (def == decl->defaults.end()) {
					error = "schema '" + schema + "': field '" + field + "' has no value and no default";
					return false;
				}
				out[field] = def->second;
			}
			return true;
		}

		const auto migration = m_migrationRefs.find(schema);
		if (migration == m_migrationRefs.end()) {
			error = "schema '" + schema + "' changed (version " + std::to_string(fromVersion) + " -> " +
				std::to_string(decl->version) + " or its fields) and has no nox.migrate";
			return false;
		}
		lua_State* L = m_runtime->State();
		const int base = lua_gettop(L);
		RunError runError;
		const bool ok = m_runtime->CallRef(migration->second,
			[&in, fromVersion](lua_State* state) {
				lua_newtable(state);
				for (const auto& [field, value] : in) {
					std::visit(PushValueVisitor{ state }, value);
					lua_setfield(state, -2, field.c_str());
				}
				lua_pushinteger(state, static_cast<lua_Integer>(fromVersion));
				return 2;
			}, 1, &runError);
		if (!ok) {
			error = "migration of '" + schema + "' failed: " + runError.message;
			lua_settop(L, base);
			return false;
		}
		if (!lua_istable(L, -1)) {
			error = "migration of '" + schema + "' must return a table";
			lua_settop(L, base);
			return false;
		}
		for (const auto& [field, type] : decl->fields) {
			lua_getfield(L, -1, field.c_str());
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				const auto def = decl->defaults.find(field);
				if (def == decl->defaults.end()) {
					error = "migration of '" + schema + "' did not produce field '" + field + "'";
					lua_settop(L, base);
					return false;
				}
				out[field] = def->second;
				continue;
			}
			Value value;
			std::string readError;
			if (!ReadLuaValue(L, -1, type, value, readError)) {
				error = "migration of '" + schema + "' field '" + field + "': " + readError;
				lua_settop(L, base);
				return false;
			}
			lua_pop(L, 1);
			out[field] = value;
		}
		lua_settop(L, base);
		return true;
	}

	size_t StateSnapshot::ValueCount() const {
		size_t count = 0;
		for (const auto& [handle, schemas] : data) {
			(void)handle;
			for (const auto& [schema, fields] : schemas) count += 1 + fields.size();
		}
		for (const auto& [handle, schemas] : authored) {
			(void)handle;
			for (const auto& [schema, fields] : schemas) count += 1 + fields.size();
		}
		return count;
	}

	bool LuaWorld::TakeSnapshot(StateSnapshot& out, std::string& error) const {
		out = StateSnapshot();
		for (const auto& [name, schema] : m_schemas) out.schemaVersions[name] = schema.version;
		out.data = m_data;
		out.attachments = m_attachments;
		out.authored = m_authoredData;
		out.authoredAttachments = m_authoredAttachments;
		out.spawned = m_spawned;
		out.touched = m_touched;
		out.frame = m_frame;
		out.playTime = m_playTime;
		out.playing = m_playing;
		const size_t values = out.ValueCount();
		if (values > StateSnapshot::kMaxValues) {
			error = "declared state too large for a reload snapshot (" + std::to_string(values) + " values, limit " +
				std::to_string(StateSnapshot::kMaxValues) + ")";
			return false;
		}
		return true;
	}

	void LuaWorld::AddDiagnosticNote(Diagnostic::Severity severity, const std::string& source, const std::string& message) {
		Diagnostic d;
		d.severity = severity;
		d.source = source;
		d.message = message;
		d.frame = m_frame;
		AddDiagnostic(std::move(d));
	}

	bool LuaWorld::ImportSnapshot(const StateSnapshot& snap, std::string& reason) {
		if (snap.format != StateSnapshot::kFormat) {
			reason = "unsupported snapshot format " + std::to_string(snap.format);
			return false;
		}
		ComponentManager* cm = Manager();
		if (!cm || !m_loaded) {
			reason = "candidate world is not loaded";
			return false;
		}
		size_t dropped = 0;
		auto migrateAll = [&](const std::map<EntityHandle, SchemaMap>& from, std::map<EntityHandle, SchemaMap>& to) -> bool {
			for (const auto& [handle, schemas] : from) {
				if (!cm->IsHandleValid(handle)) {
					++dropped;
					continue;
				}
				for (const auto& [schemaName, fields] : schemas) {
					const auto versionIt = snap.schemaVersions.find(schemaName);
					const int fromVersion = versionIt != snap.schemaVersions.end() ? versionIt->second : 1;
					if (!FindSchema(schemaName)) {
						reason = "schema '" + schemaName + "' has stored data but the new code does not declare it";
						return false;
					}
					FieldMap migrated;
					std::string error;
					if (!MigrateRecord(schemaName, fromVersion, fields, migrated, error)) {
						reason = "entity " + HandleText(handle) + ": " + error;
						return false;
					}
					to[handle][schemaName] = std::move(migrated);
				}
			}
			return true;
		};
		std::map<EntityHandle, SchemaMap> newData;
		std::map<EntityHandle, SchemaMap> newAuthored;
		if (!migrateAll(snap.data, newData) || !migrateAll(snap.authored, newAuthored)) return false;
		m_data = std::move(newData);
		m_authoredData = std::move(newAuthored);

		auto keepAttachments = [&](const std::map<EntityHandle, std::set<std::string>>& from,
			std::map<EntityHandle, std::set<std::string>>& to) {
			to.clear();
			for (const auto& [handle, names] : from) {
				if (!cm->IsHandleValid(handle)) continue;
				for (const std::string& name : names) {
					if (HasBehavior(name)) to[handle].insert(name);
				}
			}
		};
		keepAttachments(snap.attachments, m_attachments);
		keepAttachments(snap.authoredAttachments, m_authoredAttachments);
		// Reload does not run on_start again: the imported attachments count as started.
		m_started.clear();
		for (const auto& [handle, names] : m_attachments) m_started[handle] = names;

		m_spawned.clear();
		for (const EntityHandle& h : snap.spawned) {
			if (cm->IsHandleValid(h)) m_spawned.push_back(h);
		}
		m_touched.clear();
		for (const auto& [handle, local] : snap.touched) {
			if (cm->IsHandleValid(handle)) m_touched[handle] = local;
		}
		m_resolvedSpawns.clear();
		m_teleportPending = false;
		m_frame = snap.frame;
		m_playTime = snap.playTime;
		m_playing = snap.playing;
		if (dropped > 0) {
			AddDiagnosticNote(Diagnostic::Severity::Warning, "reload",
				std::to_string(dropped) + " stored record(s) belonged to entities that no longer exist and were dropped");
		}
		return true;
	}
}
