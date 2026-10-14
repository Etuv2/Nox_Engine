#include "../reflect/Reflection.h"
#include "LuaComponents.h"

#include "../RayCast.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../reflect/ReflectServices.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstring>
#include <set>
#include <sstream>

namespace NoxLua {

	namespace {
		using nox::reflect::ComponentDesc;
		using nox::reflect::MethodDesc;
		using nox::reflect::PropertyDesc;
		using nox::reflect::PropType;
		using nox::reflect::PropValue;
		using nox::reflect::Registry;

		constexpr const char* kHandleMeta = "nox.entity";       // the handle metatable LuaWorld installs
		constexpr const char* kProxyMeta = "nox.component";
		constexpr double kMaxRayDistance = 100000.0;
		constexpr size_t kMaxScenePath = 256;
		const char* const kAxisKeys[4] = { "x", "y", "z", "w" };
		const char* const kColorKeys[4] = { "r", "g", "b", "a" };

		// A proxy is a userdata holding the checked handle of its entity; the component name is its user value.
		// It never holds a node pointer: every access resolves the handle and the node again.
		struct ComponentProxy {
			EntityHandle handle;
		};

		std::string HandleText(const EntityHandle& h) {
			std::ostringstream out;
			out << "w" << h.world << ":" << h.id << ":" << h.incarnation;
			return out.str();
		}

		const char* TypeName(PropType type) {
			switch (type) {
			case PropType::Bool: return "bool";
			case PropType::Int: return "int";
			case PropType::Float: return "float";
			case PropType::Vec2: return "vec2";
			case PropType::Vec3: return "vec3";
			case PropType::Vec4: return "vec4";
			case PropType::Color3: return "color3";
			case PropType::Color4: return "color4";
			case PropType::String: return "string";
			case PropType::Enum: return "enum";
			case PropType::AssetPath: return "asset_path";
			case PropType::NodeRef: return "node_ref";
			}
			return "?";
		}

		LuaWorld* WorldOf(lua_State* L) {
			return static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		void SetFn(lua_State* L, LuaWorld* w, int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, w);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, tableIdx, name);
		}

		void PushHandle(lua_State* L, const EntityHandle& h) {
			void* mem = lua_newuserdatauv(L, sizeof(EntityHandle), 0);
			std::memcpy(mem, &h, sizeof(EntityHandle));
			luaL_setmetatable(L, kHandleMeta);
		}

		const EntityHandle* TestHandle(lua_State* L, int idx) {
			return static_cast<const EntityHandle*>(luaL_testudata(L, idx, kHandleMeta));
		}

		// Handle argument that must name a live entity of this world (same error as the other ctx functions).
		EntityHandle LiveHandle(lua_State* L, LuaWorld* w, int idx) {
			const EntityHandle* h = TestHandle(L, idx);
			if (!h) luaL_argerror(L, idx, "entity handle expected");
			if (!w->Manager() || !w->Manager()->IsHandleValid(*h)) {
				luaL_error(L, "stale or invalid entity handle (%s)", HandleText(*h).c_str());
			}
			return *h;
		}

		// Writes to components run immediately (like ctx:add), so they are limited to the callbacks that may change
		// the world. Reads are allowed everywhere a ctx is passed.
		void RequireWritable(lua_State* L, LuaWorld* w) {
			if (!w->WorldWritable()) {
				luaL_error(L, "component changes are only available inside a system, behavior or event");
			}
		}

		std::shared_ptr<SceneNode> NodeOf(lua_State* L, LuaWorld* w, const EntityHandle& h) {
			std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id);
			if (!node) {
				luaL_error(L, "entity %s has no scene node (only authored entities have components)", HandleText(h).c_str());
			}
			return node;
		}

		void PushProxy(lua_State* L, const EntityHandle& h, const std::string& component) {
			void* mem = lua_newuserdatauv(L, sizeof(ComponentProxy), 1);
			ComponentProxy proxy{ h };
			std::memcpy(mem, &proxy, sizeof(ComponentProxy));
			lua_pushlstring(L, component.data(), component.size());
			lua_setiuservalue(L, -2, 1);
			luaL_setmetatable(L, kProxyMeta);
		}

		struct ProxyRef {
			EntityHandle handle;
			std::string component;
		};

		ProxyRef ReadProxy(lua_State* L, int idx) {
			const ComponentProxy* proxy = static_cast<const ComponentProxy*>(luaL_checkudata(L, idx, kProxyMeta));
			ProxyRef ref;
			ref.handle = proxy->handle;
			lua_getiuservalue(L, idx, 1);
			if (const char* name = lua_tostring(L, -1)) ref.component = name;
			lua_pop(L, 1);
			return ref;
		}

		// Everything a proxy access needs, resolved fresh from the checked handle.
		struct Target {
			EntityHandle handle;
			std::string component;
			std::shared_ptr<SceneNode> node;
			const ComponentDesc* desc = nullptr;
		};

		Target Resolve(lua_State* L, LuaWorld* w, int idx) {
			ProxyRef ref = ReadProxy(L, idx);
			if (!w->Manager() || !w->Manager()->IsHandleValid(ref.handle)) {
				luaL_error(L, "stale or invalid entity handle (%s)", HandleText(ref.handle).c_str());
			}
			Target t;
			t.handle = ref.handle;
			t.component = ref.component;
			t.node = NodeOf(L, w, ref.handle);
			t.desc = Registry::Get().Find(ref.component);
			if (!t.desc || !t.desc->has || !t.desc->has(*t.node)) {
				luaL_error(L, "component '%s' is not present on entity %s", ref.component.c_str(), HandleText(ref.handle).c_str());
			}
			return t;
		}

		// ---- values -----------------------------------------------------------------------------------

		void PushVector(lua_State* L, const float* comps, int n, bool color) {
			lua_createtable(L, 0, n);
			for (int i = 0; i < n; ++i) {
				lua_pushnumber(L, comps[i]);
				lua_setfield(L, -2, color ? kColorKeys[i] : kAxisKeys[i]);
			}
		}

		// Registry value -> Lua. Enums come out as their names, vectors as {x=, y=, z=} (colours {r=, g=, b=}).
		void PushValue(lua_State* L, const PropertyDesc* prop, const PropValue& value) {
			if (const bool* b = std::get_if<bool>(&value)) {
				lua_pushboolean(L, *b ? 1 : 0);
			}
			else if (const int64_t* i = std::get_if<int64_t>(&value)) {
				if (prop && prop->type == PropType::Enum && *i >= 0 && static_cast<size_t>(*i) < prop->enumNames.size()) {
					lua_pushstring(L, prop->enumNames[static_cast<size_t>(*i)].c_str());
				}
				else {
					lua_pushinteger(L, static_cast<lua_Integer>(*i));
				}
			}
			else if (const double* d = std::get_if<double>(&value)) {
				lua_pushnumber(L, static_cast<lua_Number>(*d));
			}
			else if (const glm::vec2* v2 = std::get_if<glm::vec2>(&value)) {
				const float c[2] = { v2->x, v2->y };
				PushVector(L, c, 2, false);
			}
			else if (const glm::vec3* v3 = std::get_if<glm::vec3>(&value)) {
				const float c[3] = { v3->x, v3->y, v3->z };
				PushVector(L, c, 3, prop && prop->type == PropType::Color3);
			}
			else if (const glm::vec4* v4 = std::get_if<glm::vec4>(&value)) {
				const float c[4] = { v4->x, v4->y, v4->z, v4->w };
				PushVector(L, c, 4, prop && prop->type == PropType::Color4);
			}
			else if (const std::string* s = std::get_if<std::string>(&value)) {
				lua_pushlstring(L, s->data(), s->size());
			}
			else {
				lua_pushnil(L);
			}
		}

		// Reads a vector table: {x=, y=, z=, w=} (colours also {r=, g=, b=, a=}) or a 1-based array.
		bool ReadVector(lua_State* L, int idx, int n, bool color, float* out, std::string& error) {
			idx = lua_absindex(L, idx);
			if (lua_type(L, idx) != LUA_TTABLE) {
				error = "expected a vector table";
				return false;
			}
			for (int i = 0; i < n; ++i) {
				bool found = false;
				const char* keys[2] = { color ? kColorKeys[i] : kAxisKeys[i], kAxisKeys[i] };
				const int tries = color ? 2 : 1;
				for (int t = 0; t < tries && !found; ++t) {
					lua_getfield(L, idx, keys[t]);
					if (lua_type(L, -1) == LUA_TNUMBER) {
						out[i] = static_cast<float>(lua_tonumber(L, -1));
						found = true;
					}
					lua_pop(L, 1);
				}
				if (!found) {
					lua_geti(L, idx, i + 1);
					if (lua_type(L, -1) == LUA_TNUMBER) {
						out[i] = static_cast<float>(lua_tonumber(L, -1));
						found = true;
					}
					lua_pop(L, 1);
				}
				if (!found) {
					error = "expected a vector with " + std::to_string(n) + " numbers";
					return false;
				}
			}
			return true;
		}

		// Lua value -> registry value of the given type. `enumNames` may be null (method arguments: integers only).
		bool ToPropValue(lua_State* L, int idx, PropType type, const std::vector<std::string>* enumNames,
			PropValue& out, std::string& error) {
			switch (type) {
			case PropType::Bool:
				if (!lua_isboolean(L, idx)) { error = "expected boolean"; return false; }
				out = lua_toboolean(L, idx) != 0;
				return true;
			case PropType::Int: {
				if (lua_isinteger(L, idx)) { out = static_cast<int64_t>(lua_tointeger(L, idx)); return true; }
				if (lua_type(L, idx) != LUA_TNUMBER) { error = "expected integer"; return false; }
				const double d = lua_tonumber(L, idx);
				if (!std::isfinite(d) || std::floor(d) != d) { error = "expected integer"; return false; }
				out = static_cast<int64_t>(d);
				return true;
			}
			case PropType::Enum: {
				if (lua_type(L, idx) == LUA_TSTRING) {
					const std::string name = lua_tostring(L, idx);
					if (enumNames) {
						for (size_t i = 0; i < enumNames->size(); ++i) {
							if ((*enumNames)[i] == name) { out = static_cast<int64_t>(i); return true; }
						}
						std::string list;
						for (size_t i = 0; i < enumNames->size(); ++i) list += (i ? ", " : "") + (*enumNames)[i];
						error = "unknown value '" + name + "' (expected one of: " + list + ")";
						return false;
					}
					error = "expected integer";
					return false;
				}
				if (lua_isinteger(L, idx)) { out = static_cast<int64_t>(lua_tointeger(L, idx)); return true; }
				error = "expected an enum name or index";
				return false;
			}
			case PropType::Float:
				if (lua_type(L, idx) != LUA_TNUMBER) { error = "expected number"; return false; }
				out = static_cast<double>(lua_tonumber(L, idx));
				return true;
			case PropType::Vec2:
			case PropType::Vec3:
			case PropType::Color3:
			case PropType::Vec4:
			case PropType::Color4: {
				const int n = (type == PropType::Vec2) ? 2 : (type == PropType::Vec3 || type == PropType::Color3) ? 3 : 4;
				const bool color = type == PropType::Color3 || type == PropType::Color4;
				float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if (!ReadVector(L, idx, n, color, c, error)) return false;
				if (n == 2) out = glm::vec2(c[0], c[1]);
				else if (n == 3) out = glm::vec3(c[0], c[1], c[2]);
				else out = glm::vec4(c[0], c[1], c[2], c[3]);
				return true;
			}
			case PropType::String:
			case PropType::AssetPath:
			case PropType::NodeRef:
				if (lua_type(L, idx) != LUA_TSTRING) { error = "expected string"; return false; }
				out = std::string(lua_tostring(L, idx));
				return true;
			}
			error = "unsupported type";
			return false;
		}

		// ---- proxy metamethods --------------------------------------------------------------------------

		int MethodCall(lua_State* L);

		// proxy.name: property read, or a bound method (proxy:method(...)).
		int ProxyIndex(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const Target t = Resolve(L, w, 1);
			const char* key = luaL_checkstring(L, 2);
			if (const PropertyDesc* prop = t.desc->FindProperty(key)) {
				if (!prop->AppliesTo(*t.node)) {
					return luaL_error(L, "%s.%s does not apply to this node", t.component.c_str(), key);
				}
				nox::reflect::PropValue value;
				if (!nox::reflect::GetProperty(*t.node, t.component, key, value)) {
					return luaL_error(L, "%s.%s cannot be read", t.component.c_str(), key);
				}
				PushValue(L, prop, value);
				return 1;
			}
			if (t.desc->FindMethod(key)) {
				lua_pushlightuserdata(L, w);
				lua_pushstring(L, key);
				lua_pushcclosure(L, MethodCall, 2);
				return 1;
			}
			return luaL_error(L, "%s has no property or method '%s'", t.component.c_str(), key);
		}

		// proxy.name = value: validated property write through the registry.
		int ProxyNewIndex(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireWritable(L, w);
			const Target t = Resolve(L, w, 1);
			const char* key = luaL_checkstring(L, 2);
			const PropertyDesc* prop = t.desc->FindProperty(key);
			if (!prop) return luaL_error(L, "%s has no property '%s'", t.component.c_str(), key);
			if (!prop->AppliesTo(*t.node)) return luaL_error(L, "%s.%s does not apply to this node", t.component.c_str(), key);
			if (!prop->set || (prop->flags & nox::reflect::PropReadOnly)) {
				return luaL_error(L, "%s.%s is read-only", t.component.c_str(), key);
			}
			PropValue value;
			std::string error;
			if (!ToPropValue(L, 3, prop->type, &prop->enumNames, value, error)) {
				return luaL_error(L, "%s.%s: %s", t.component.c_str(), key, error.c_str());
			}
			if (!nox::reflect::SetProperty(*t.node, t.component, key, value, &error)) {
				return luaL_error(L, "%s", error.c_str());
			}
			return 0;
		}

		int ProxyToString(lua_State* L) {
			const ProxyRef ref = ReadProxy(L, 1);
			lua_pushfstring(L, "%s(%s)", ref.component.c_str(), HandleText(ref.handle).c_str());
			return 1;
		}

		// proxy:method(args...): the method name is upvalue 2; argument 1 is the proxy itself.
		int MethodCall(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireWritable(L, w);
			const Target t = Resolve(L, w, 1);
			const char* method = lua_tostring(L, lua_upvalueindex(2));
			const MethodDesc* m = t.desc->FindMethod(method);
			if (!m) return luaL_error(L, "%s has no method '%s'", t.component.c_str(), method);
			const int nargs = lua_gettop(L) - 1;
			if (nargs != static_cast<int>(m->args.size())) {
				return luaL_error(L, "%s:%s expects %d argument(s)", t.component.c_str(), method, static_cast<int>(m->args.size()));
			}
			std::vector<PropValue> args;
			for (int i = 0; i < nargs; ++i) {
				PropValue value;
				std::string error;
				if (!ToPropValue(L, i + 2, m->args[static_cast<size_t>(i)], nullptr, value, error)) {
					return luaL_error(L, "%s:%s: bad argument #%d (%s)", t.component.c_str(), method, i + 1, error.c_str());
				}
				args.push_back(value);
			}
			PropValue result;
			std::string error;
			if (!nox::reflect::InvokeMethod(*t.node, t.component, method, args, result, &error)) {
				return luaL_error(L, "%s", error.c_str());
			}
			if (!m->returnsValue) return 0;
			PushValue(L, nullptr, result);
			return 1;
		}

		// ---- ctx functions ----------------------------------------------------------------------------

		// ctx:component(h, name) -> proxy, or nil when the entity has no such component.
		int CtxComponent(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string name = luaL_checkstring(L, 3);
			const ComponentDesc* desc = Registry::Get().Find(name);
			if (!desc) return luaL_argerror(L, 3, "no descriptor for this component name (see nox.components())");
			std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id);
			if (!node || !desc->has || !desc->has(*node)) {
				lua_pushnil(L);
				return 1;
			}
			PushProxy(L, h, name);
			return 1;
		}

		// ctx:add_component(h, name) and ctx:remove_component(h, name): true when the component is now present
		// (add) or absent (remove); otherwise false and a reason.
		int ChangeComponent(lua_State* L, bool add) {
			LuaWorld* w = WorldOf(L);
			RequireWritable(L, w);
			const EntityHandle h = LiveHandle(L, w, 2);
			const std::string name = luaL_checkstring(L, 3);
			const ComponentDesc* desc = Registry::Get().Find(name);
			if (!desc) return luaL_argerror(L, 3, "no descriptor for this component name (see nox.components())");
			std::shared_ptr<SceneNode> node = NodeOf(L, w, h);
			const bool present = desc->has && desc->has(*node);
			if (present == add) {
				lua_pushboolean(L, 1);
				return 1;
			}
			const auto& change = add ? desc->add : desc->remove;
			if (!change || !change(*node)) {
				lua_pushboolean(L, 0);
				lua_pushfstring(L, "component '%s' cannot be %s this node", name.c_str(), add ? "added to" : "removed from");
				return 2;
			}
			lua_pushboolean(L, 1);
			return 1;
		}
		int CtxAddComponent(lua_State* L) { return ChangeComponent(L, true); }
		int CtxRemoveComponent(lua_State* L) { return ChangeComponent(L, false); }

		// ctx:components(h): names present on the entity; registry (inspector) order first, then native names.
		int CtxComponents(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			std::set<std::string> names;
			if (std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id)) {
				for (const ComponentDesc* desc : Registry::Get().ComponentsOf(*node)) names.insert(desc->name);
			}
			for (const std::string& native : NativeComponentNames()) {
				bool known = false;
				if (NativeComponentPresent(w->Manager(), h.id, native, known)) names.insert(native);
			}
			lua_newtable(L);
			lua_Integer index = 1;
			std::set<std::string> pushed;
			for (const ComponentDesc& desc : Registry::Get().All()) {
				if (!names.count(desc.name)) continue;
				lua_pushlstring(L, desc.name.data(), desc.name.size());
				lua_rawseti(L, -2, index++);
				pushed.insert(desc.name);
			}
			for (const std::string& name : names) {
				if (pushed.count(name)) continue;
				lua_pushlstring(L, name.data(), name.size());
				lua_rawseti(L, -2, index++);
			}
			return 1;
		}

		// ctx:parent(h): handle of the parent node, or nil at the top of the hierarchy.
		int CtxParent(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id);
			std::shared_ptr<SceneNode> parent = node ? node->parentNode.lock() : nullptr;
			if (!parent || parent->GetEntityID() == INVALID_ENTITY) {
				lua_pushnil(L);
				return 1;
			}
			const EntityHandle ph = w->Manager()->MakeHandle(parent->GetEntityID());
			if (!w->Manager()->IsHandleValid(ph)) {
				lua_pushnil(L);
				return 1;
			}
			PushHandle(L, ph);
			return 1;
		}

		// ctx:children(h): array of handles of the child nodes that have entities, in scene order.
		int CtxChildren(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			const EntityHandle h = LiveHandle(L, w, 2);
			lua_newtable(L);
			lua_Integer index = 1;
			if (std::shared_ptr<SceneNode> node = w->FindSceneNode(h.id)) {
				for (const auto& child : node->children) {
					if (!child || child->GetEntityID() == INVALID_ENTITY) continue;
					const EntityHandle ch = w->Manager()->MakeHandle(child->GetEntityID());
					if (!w->Manager()->IsHandleValid(ch)) continue;
					PushHandle(L, ch);
					lua_rawseti(L, -2, index++);
				}
			}
			return 1;
		}

		bool ReadVec3Arg(lua_State* L, int idx, glm::vec3& out, std::string& error) {
			float c[3] = { 0.0f, 0.0f, 0.0f };
			if (!ReadVector(L, idx, 3, false, c, error)) return false;
			out = glm::vec3(c[0], c[1], c[2]);
			if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) {
				error = "must be finite";
				return false;
			}
			return true;
		}

		void SetNumberField(lua_State* L, const char* key, double value) {
			lua_pushnumber(L, value);
			lua_setfield(L, -2, key);
		}

		void SetPointField(lua_State* L, const char* key, const glm::vec3& v) {
			const float c[3] = { v.x, v.y, v.z };
			PushVector(L, c, 3, false);
			lua_setfield(L, -2, key);
		}

		// ctx:raycast(origin, dir, max_dist) -> { entity = handle|nil, name, distance, point = {x,y,z}, normal = {x,y,z} }
		// for the nearest scene node hit within max_dist, or nil. Reads only: valid in any callback.
		int CtxRaycast(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			glm::vec3 origin(0.0f), dir(0.0f);
			std::string error;
			if (!ReadVec3Arg(L, 2, origin, error)) return luaL_argerror(L, 2, error.c_str());
			if (!ReadVec3Arg(L, 3, dir, error)) return luaL_argerror(L, 3, error.c_str());
			const double maxDist = luaL_checknumber(L, 4);
			if (!(maxDist > 0.0) || maxDist > kMaxRayDistance) return luaL_argerror(L, 4, "max_dist must be in (0, 100000]");
			if (!(glm::length(dir) > 1e-6f)) return luaL_argerror(L, 3, "direction must not be zero");
			if (!w->Scene() || !w->Scene()->GetRoot()) {
				lua_pushnil(L);
				return 1;
			}
			const RayCast::HitResult hit = RayCast::RayIntersectScene(RayCast::Ray(origin, dir), w->Scene()->GetRoot());
			if (!hit.hit || !hit.node || static_cast<double>(hit.distance) > maxDist) {
				lua_pushnil(L);
				return 1;
			}
			lua_createtable(L, 0, 5);
			const std::string name = hit.node->GetName();
			lua_pushlstring(L, name.data(), name.size());
			lua_setfield(L, -2, "name");
			if (hit.node->GetEntityID() != INVALID_ENTITY) {
				const EntityHandle eh = w->Manager()->MakeHandle(hit.node->GetEntityID());
				if (w->Manager()->IsHandleValid(eh)) {
					PushHandle(L, eh);
					lua_setfield(L, -2, "entity");
				}
			}
			SetNumberField(L, "distance", hit.distance);
			SetPointField(L, "point", hit.point);
			SetPointField(L, "normal", hit.normal);
			return 1;
		}

		// Logical scene path: scenes/<name>.json with [A-Za-z0-9_./-] only and no parent segments.
		bool ValidScenePath(const std::string& path, std::string& error) {
			if (path.size() > kMaxScenePath) { error = "path is too long"; return false; }
			if (path.rfind("scenes/", 0) != 0 || path.size() < 12 || path.compare(path.size() - 5, 5, ".json") != 0) {
				error = "expected a logical path scenes/<name>.json";
				return false;
			}
			if (path.find("..") != std::string::npos || path.find("//") != std::string::npos) {
				error = "parent or repeated separators are not allowed";
				return false;
			}
			for (char c : path) {
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
					c == '_' || c == '-' || c == '.' || c == '/';
				if (!ok) { error = "only letters, digits, _ - . / are allowed"; return false; }
			}
			return true;
		}

		// ctx:load_scene(path): switches scene at the next frame boundary (the host performs the swap).
		int CtxLoadScene(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			RequireWritable(L, w);
			const std::string path = luaL_checkstring(L, 2);
			std::string error;
			if (!ValidScenePath(path, error)) return luaL_argerror(L, 2, error.c_str());
			const nox::reflect::EngineServices& services = nox::reflect::Services();
			if (!services.requestSceneSwap) return luaL_error(L, "load_scene: scene switching is not available in this host");
			services.requestSceneSwap(path);
			return 0;
		}

		// ---- nox.components() ------------------------------------------------------------------------

		void SetStringField(lua_State* L, const char* key, const std::string& value) {
			lua_pushlstring(L, value.data(), value.size());
			lua_setfield(L, -2, key);
		}

		void SetBoolField(lua_State* L, const char* key, bool value) {
			lua_pushboolean(L, value ? 1 : 0);
			lua_setfield(L, -2, key);
		}

		void PushPropertyDescription(lua_State* L, const PropertyDesc& prop) {
			lua_createtable(L, 0, 12);
			SetStringField(L, "name", prop.name);
			SetStringField(L, "label", prop.label);
			SetStringField(L, "tooltip", prop.tooltip);
			SetStringField(L, "type", TypeName(prop.type));
			SetBoolField(L, "read_only", (prop.flags & nox::reflect::PropReadOnly) != 0 || !prop.set);
			SetBoolField(L, "hidden", (prop.flags & nox::reflect::PropHidden) != 0);
			SetBoolField(L, "no_save", (prop.flags & nox::reflect::PropNoSave) != 0);
			SetBoolField(L, "angle", (prop.flags & nox::reflect::PropAngle) != 0);
			SetBoolField(L, "conditional", static_cast<bool>(prop.applies));
			if (prop.minValue < prop.maxValue) {
				SetNumberField(L, "min", prop.minValue);
				SetNumberField(L, "max", prop.maxValue);
			}
			if (prop.step > 0.0) SetNumberField(L, "step", prop.step);
			if (prop.type == PropType::Enum) {
				lua_createtable(L, static_cast<int>(prop.enumNames.size()), 0);
				lua_Integer i = 1;
				for (const std::string& name : prop.enumNames) {
					lua_pushlstring(L, name.data(), name.size());
					lua_rawseti(L, -2, i++);
				}
				lua_setfield(L, -2, "enum");
			}
			if (!prop.assetFilter.empty()) {
				lua_createtable(L, static_cast<int>(prop.assetFilter.size()), 0);
				lua_Integer i = 1;
				for (const std::string& ext : prop.assetFilter) {
					lua_pushlstring(L, ext.data(), ext.size());
					lua_rawseti(L, -2, i++);
				}
				lua_setfield(L, -2, "asset_filter");
			}
		}

		void PushMethodDescription(lua_State* L, const MethodDesc& method) {
			lua_createtable(L, 0, 5);
			SetStringField(L, "name", method.name);
			SetStringField(L, "tooltip", method.tooltip);
			lua_createtable(L, static_cast<int>(method.args.size()), 0);
			lua_Integer i = 1;
			for (PropType type : method.args) {
				lua_pushstring(L, TypeName(type));
				lua_rawseti(L, -2, i++);
			}
			lua_setfield(L, -2, "args");
			SetBoolField(L, "returns", method.returnsValue);
			SetBoolField(L, "editor_button", method.editorButton);
		}

		// nox.components(): every registered descriptor (names, categories, properties, methods) in inspector order.
		int NoxComponents(lua_State* L) {
			lua_newtable(L);
			lua_Integer index = 1;
			for (const ComponentDesc& desc : Registry::Get().All()) {
				lua_createtable(L, 0, 10);
				SetStringField(L, "name", desc.name);
				SetStringField(L, "category", desc.category);
				SetStringField(L, "tooltip", desc.tooltip);
				SetBoolField(L, "builtin", desc.builtin);
				SetNumberField(L, "order", desc.order);
				SetBoolField(L, "can_add", static_cast<bool>(desc.add));
				SetBoolField(L, "can_remove", static_cast<bool>(desc.remove));
				lua_createtable(L, static_cast<int>(desc.properties.size()), 0);
				lua_Integer p = 1;
				for (const PropertyDesc& prop : desc.properties) {
					PushPropertyDescription(L, prop);
					lua_rawseti(L, -2, p++);
				}
				lua_setfield(L, -2, "properties");
				lua_createtable(L, static_cast<int>(desc.methods.size()), 0);
				lua_Integer m = 1;
				for (const MethodDesc& method : desc.methods) {
					PushMethodDescription(L, method);
					lua_rawseti(L, -2, m++);
				}
				lua_setfield(L, -2, "methods");
				lua_rawseti(L, -2, index++);
			}
			return 1;
		}
	}

	const std::vector<std::string>& NativeComponentNames() {
		static const std::vector<std::string> names = {
			"Transform", "Renderable", "Light", "Camera", "Animation", "Physics", "Collider", "Audio", "LPVVolume"
		};
		return names;
	}

	bool NativeComponentPresent(ComponentManager* cm, EntityID id, const std::string& name, bool& known) {
		known = true;
		if (name == "Transform") return cm->HasTransform(id);
		if (name == "Renderable") return cm->HasRenderable(id);
		if (name == "Light") return cm->HasLight(id);
		if (name == "Camera") return cm->HasCamera(id);
		if (name == "Animation") return cm->HasAnimation(id);
		if (name == "Physics") return cm->HasPhysics(id);
		if (name == "Collider") return cm->HasCollider(id);
		if (name == "Audio") return cm->HasAudio(id);
		if (name == "LPVVolume") return cm->HasLPVVolume(id);
		known = false;
		return false;
	}

	void InstallComponentApi(lua_State* L, int ctxIdx, LuaWorld* w) {
		nox::reflect::RegisterBuiltinComponents();
		ctxIdx = lua_absindex(L, ctxIdx);

		// Component proxy metatable (one per VM); the world is the upvalue of every metamethod.
		luaL_newmetatable(L, kProxyMeta);
		const int mtIdx = lua_gettop(L);
		SetFn(L, w, mtIdx, "__index", ProxyIndex);
		SetFn(L, w, mtIdx, "__newindex", ProxyNewIndex);
		lua_pushcfunction(L, ProxyToString);
		lua_setfield(L, -2, "__tostring");
		lua_pushstring(L, kProxyMeta);
		lua_setfield(L, -2, "__name");
		lua_pop(L, 1);

		SetFn(L, w, ctxIdx, "component", CtxComponent);
		SetFn(L, w, ctxIdx, "add_component", CtxAddComponent);
		SetFn(L, w, ctxIdx, "remove_component", CtxRemoveComponent);
		SetFn(L, w, ctxIdx, "components", CtxComponents);
		SetFn(L, w, ctxIdx, "parent", CtxParent);
		SetFn(L, w, ctxIdx, "children", CtxChildren);
		SetFn(L, w, ctxIdx, "raycast", CtxRaycast);
		SetFn(L, w, ctxIdx, "load_scene", CtxLoadScene);

		lua_getglobal(L, "nox");
		lua_pushcfunction(L, NoxComponents);
		lua_setfield(L, -2, "components");
		lua_pop(L, 1);
	}
}
