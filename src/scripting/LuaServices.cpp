#include "LuaServices.h"

#include "../Camera.h"
#include "../VehicleFX.h"
#include "../app/UserSettings.h"
#include "../assets/AssetIO.h"
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace NoxLua {

	namespace {
		constexpr const char* kServicesKey = "nox.services";
		constexpr const char* kRoadMeta = "nox.roads";
		constexpr const char* kCollisionMeta = "nox.collision";

		LuaServices* ServicesOf(lua_State* L) {
			lua_getfield(L, LUA_REGISTRYINDEX, kServicesKey);
			LuaServices* services = static_cast<LuaServices*>(lua_touserdata(L, -1));
			lua_pop(L, 1);
			return services;
		}

		double CheckFinite(lua_State* L, int idx) {
			const double v = luaL_checknumber(L, idx);
			if (!std::isfinite(v)) luaL_error(L, "argument #%d must be a finite number", idx);
			return v;
		}

		float OptFinite(lua_State* L, int idx, float fallback) {
			return lua_isnoneornil(L, idx) ? fallback : static_cast<float>(CheckFinite(L, idx));
		}

		glm::vec3 CheckVec3(lua_State* L, int idx) {
			return glm::vec3(static_cast<float>(CheckFinite(L, idx)), static_cast<float>(CheckFinite(L, idx + 1)),
				static_cast<float>(CheckFinite(L, idx + 2)));
		}

		void PushVec3(lua_State* L, const glm::vec3& v) {
			lua_pushnumber(L, v.x);
			lua_pushnumber(L, v.y);
			lua_pushnumber(L, v.z);
		}

		// ---- Camera ------------------------------------------------------------------------------------

		Camera& CameraOf(lua_State* L, LuaServices* s) {
			if (!s->camera) luaL_error(L, "no module camera is available in this scene");
			return *s->camera;
		}

		// ctx:camera_look_at(px, py, pz, tx, ty, tz): pose from a position and a target (pitch is clamped to +-89).
		int CtxCameraLookAt(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			Camera& camera = CameraOf(L, s);
			const glm::vec3 position = CheckVec3(L, 2);
			const glm::vec3 target = CheckVec3(L, 5);
			glm::vec3 dir = target - position;
			const float length = glm::length(dir);
			if (!(length > 1e-4f)) return luaL_error(L, "camera_look_at: the target is at the camera position");
			dir /= length;
			const float yaw = glm::degrees(std::atan2(dir.z, dir.x));
			const float pitch = glm::degrees(std::asin(glm::clamp(dir.y, -1.0f, 1.0f)));
			camera.SetPose(position, glm::vec3(0.0f, 1.0f, 0.0f), yaw, pitch);
			return 0;
		}

		int CtxCameraPosition(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			PushVec3(L, CameraOf(L, s).GetCameraPosition());
			return 3;
		}

		// ctx:camera_set_fov(degrees)
		int CtxCameraSetFov(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			Camera& camera = CameraOf(L, s);
			const double fov = CheckFinite(L, 2);
			if (fov < 10.0 || fov > 150.0) return luaL_error(L, "camera_set_fov: fov must be 10..150 degrees");
			camera.SetPerspective(static_cast<float>(fov), s->aspect, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
			return 0;
		}

		// ctx:camera_set_clip(near, far)
		int CtxCameraSetClip(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			Camera& camera = CameraOf(L, s);
			const double nearPlane = CheckFinite(L, 2), farPlane = CheckFinite(L, 3);
			if (nearPlane < 0.01 || farPlane > 100000.0 || !(nearPlane < farPlane)) {
				return luaL_error(L, "camera_set_clip: need 0.01 <= near < far <= 100000");
			}
			camera.SetPerspective(camera.GetCameraFov(), s->aspect, static_cast<float>(nearPlane), static_cast<float>(farPlane));
			return 0;
		}

		// ctx:camera_cut(): the next ConsumeTeleport reports a discontinuous move (temporal history resets).
		int CtxCameraCut(lua_State* L) {
			ServicesOf(L)->cameraCut = true;
			return 0;
		}

		// ctx:camera_project(x, y, z) -> nx, ny, distance, visible
		// nx, ny: the point in the game overlay, 0..1 with y down (projection at the current overlay aspect).
		// visible is false behind the camera or more than a quarter of the view outside it; then nx, ny are 0.5.
		int CtxCameraProject(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			Camera& camera = CameraOf(L, s);
			const glm::vec3 p = CheckVec3(L, 2);
			const auto [view, proj] = camera.GetUpdatedViewProjectionMatrix(s->aspect, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
			const glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
			const float distance = glm::length(p - camera.GetCameraPosition());
			bool visible = false;
			float nx = 0.5f, ny = 0.5f;
			if (clip.w > 1e-4f) {
				const float ndcX = clip.x / clip.w, ndcY = clip.y / clip.w;
				const float ox = ndcX * 0.5f + 0.5f, oy = 0.5f - ndcY * 0.5f;
				if (ox >= -0.25f && ox <= 1.25f && oy >= -0.25f && oy <= 1.25f) {
					visible = true;
					nx = ox;
					ny = oy;
				}
			}
			lua_pushnumber(L, nx);
			lua_pushnumber(L, ny);
			lua_pushnumber(L, distance);
			lua_pushboolean(L, visible ? 1 : 0);
			return 4;
		}

		// ctx:overlay_rect() -> x, y, w, h of the game overlay in window coordinates (last drawn frame; zero before).
		int CtxOverlayRect(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			lua_pushnumber(L, s->overlayOrigin.x);
			lua_pushnumber(L, s->overlayOrigin.y);
			lua_pushnumber(L, s->overlaySize.x);
			lua_pushnumber(L, s->overlaySize.y);
			return 4;
		}

		// ---- Effects (native VehicleFX pools; Lua only emits) ------------------------------------------

		// ctx:fx_smoke(x, y, z, vx, vy, vz, intensity, dt [, floorY]) and ctx:fx_spray(...). Returns false when the
		// per-frame emitter budget is used up (the call is dropped).
		int EmitCommon(lua_State* L, bool spray) {
			LuaServices* s = ServicesOf(L);
			const glm::vec3 pos = CheckVec3(L, 2);
			const glm::vec3 velocity = CheckVec3(L, 5);
			const double intensity = CheckFinite(L, 8);
			const double dt = CheckFinite(L, 9);
			const float floorY = OptFinite(L, 10, -1e30f);
			if (intensity < 0.0 || intensity > 1.0) return luaL_error(L, "fx intensity must be 0..1");
			if (dt < 0.0 || dt > 1.0) return luaL_error(L, "fx dt must be 0..1 seconds");
			if (s->fxEmitsThisFrame >= services_limits::kMaxFxEmitsPerFrame) {
				lua_pushboolean(L, 0);
				return 1;
			}
			++s->fxEmitsThisFrame;
			s->fxUsed = true;
			if (spray) VehicleFX::EmitSpray(pos, velocity, static_cast<float>(intensity), static_cast<float>(dt), floorY);
			else VehicleFX::EmitSmoke(pos, velocity, static_cast<float>(intensity), static_cast<float>(dt), floorY);
			lua_pushboolean(L, 1);
			return 1;
		}
		int CtxFxSmoke(lua_State* L) { return EmitCommon(L, false); }
		int CtxFxSpray(lua_State* L) { return EmitCommon(L, true); }

		int CtxFxSetEnabled(lua_State* L) {
			luaL_checktype(L, 2, LUA_TBOOLEAN);
			VehicleFX::SetEnabled(lua_toboolean(L, 2) != 0);
			return 0;
		}

		int CtxFxClear(lua_State* L) {
			(void)L;
			VehicleFX::Clear();
			return 0;
		}

		// ctx:fx_count() -> live native particles (all pools); a diagnostic count for logs.
		int CtxFxCount(lua_State* L) {
			lua_pushinteger(L, static_cast<lua_Integer>(VehicleFX::ParticleCount()));
			return 1;
		}

		// ---- Save data (user directory, validated names, bounded JSON) ---------------------------------

		// ctx:load_user_json(name) -> value | nil, reason
		int CtxLoadUserJson(lua_State* L) {
			const std::string name = luaL_checkstring(L, 2);
			std::string error;
			if (!ValidateUserFileName(name, error)) {
				lua_pushnil(L);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			if (const auto path = UserSettings::UserPath(name)) {
				std::error_code ec;
				const auto size = std::filesystem::file_size(*path, ec);
				if (!ec && size > services_limits::kMaxUserFileBytes) {
					lua_pushnil(L);
					lua_pushstring(L, "file is larger than the user file limit");
					return 2;
				}
			}
			const auto value = UserSettings::LoadUserJson(name);
			if (!value) {
				lua_pushnil(L);
				lua_pushstring(L, "missing or not valid JSON");
				return 2;
			}
			PushJsonValue(L, *value);
			return 1;
		}

		// ctx:save_user_json(name, value) -> true | false, reason
		int CtxSaveUserJson(lua_State* L) {
			const std::string name = luaL_checkstring(L, 2);
			luaL_checkany(L, 3);
			std::string error;
			if (!ValidateUserFileName(name, error)) {
				lua_pushboolean(L, 0);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			nlohmann::json value;
			if (!LuaValueToJson(L, 3, value, error)) {
				lua_pushboolean(L, 0);
				lua_pushstring(L, error.c_str());
				return 2;
			}
			if (!UserSettings::SaveUserJson(name, value)) {
				lua_pushboolean(L, 0);
				lua_pushstring(L, "could not write the user file");
				return 2;
			}
			lua_pushboolean(L, 1);
			return 1;
		}

		// ---- Audio (procedural vehicle synth parameters and cues; the module publishes them on the gameplay thread) ----

		// ctx:audio_set({ rpm, rpm_max, throttle, speed01, slip01, boost, limiter, master_volume, active, paused })
		// Partial update: fields not given keep their last value. Numbers are clamped to their ranges.
		int CtxAudioSet(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			luaL_checktype(L, 2, LUA_TTABLE);
			VehicleAudioState& audio = s->audio;
			lua_pushnil(L);
			while (lua_next(L, 2) != 0) {
				if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "audio_set: parameter names must be strings");
				const std::string key = lua_tostring(L, -2);
				auto number = [L, &key](float lo, float hi) {
					if (!lua_isnumber(L, -1)) luaL_error(L, "audio parameter '%s' must be a number", key.c_str());
					const double v = lua_tonumber(L, -1);
					if (!std::isfinite(v)) luaL_error(L, "audio parameter '%s' must be finite", key.c_str());
					return std::clamp(static_cast<float>(v), lo, hi);
				};
				auto flag = [L, &key]() {
					if (!lua_isboolean(L, -1)) luaL_error(L, "audio parameter '%s' must be a boolean", key.c_str());
					return lua_toboolean(L, -1) != 0;
				};
				if (key == "rpm") audio.rpm = number(0.0f, 30000.0f);
				else if (key == "rpm_max") audio.rpmMax = number(1000.0f, 30000.0f);
				else if (key == "throttle") audio.throttle = number(0.0f, 1.0f);
				else if (key == "speed01") audio.speed01 = number(0.0f, 1.0f);
				else if (key == "slip01") audio.slip01 = number(0.0f, 1.0f);
				else if (key == "master_volume") audio.masterVolume = number(0.0f, 1.0f);
				else if (key == "boost") audio.boost = flag();
				else if (key == "limiter") audio.limiter = flag();
				else if (key == "active") audio.active = flag();
				else if (key == "paused") audio.paused = flag();
				else return luaL_error(L, "unknown audio parameter '%s'", key.c_str());
				lua_pop(L, 1);
			}
			s->audioUsed = true;
			return 0;
		}

		// ctx:audio_cue(name) -> true, or false when the frame's cue budget is used up
		int CtxAudioCue(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const std::string name = luaL_checkstring(L, 2);
			AudioCue cue = AudioCue::Select;
			if (!AudioCueFromName(name, cue)) {
				return luaL_error(L, "unknown audio cue '%s' (select, back, tick, start, marker, complete, fail, record, gear_shift)", name.c_str());
			}
			s->audioUsed = true;
			if (s->audioCues.size() >= services_limits::kMaxAudioCuesPerFrame) {
				lua_pushboolean(L, 0);
				return 1;
			}
			s->audioCues.push_back(cue);
			lua_pushboolean(L, 1);
			return 1;
		}

		// ---- Declared files: nox.roads(name), nox.collision(name) -------------------------------------

		const DeclaredFile* FindDeclared(const std::vector<DeclaredFile>& list, const std::string& name) {
			for (const DeclaredFile& file : list) {
				if (file.name == name) return &file;
			}
			return nullptr;
		}

		void PushHandle(lua_State* L, const char* meta, size_t index) {
			size_t* handle = static_cast<size_t*>(lua_newuserdatauv(L, sizeof(size_t), 0));
			*handle = index;
			luaL_setmetatable(L, meta);
		}

		size_t HandleIndex(lua_State* L, int idx, const char* meta) {
			return *static_cast<size_t*>(luaL_checkudata(L, idx, meta));
		}

		// Networks that are still loaded (released slots are null).
		size_t LiveRoads(const LuaServices& s) {
			size_t live = 0;
			for (const auto& network : s.roads) {
				if (network) ++live;
			}
			return live;
		}

		// nox.roads(name): road network declared in project.json "roads". Loaded once per world.
		int NoxRoads(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const std::string name = luaL_checkstring(L, 1);
			const DeclaredFile* decl = FindDeclared(s->declaredRoads, name);
			if (!decl) return luaL_error(L, "road network '%s' is not declared in project.json (roads)", name.c_str());
			size_t index = 0;
			const auto known = s->roadIndex.find(name);
			if (known != s->roadIndex.end()) {
				index = known->second;
			}
			else {
				if (LiveRoads(*s) >= services_limits::kMaxRoads) return luaL_error(L, "too many road networks (max %d)", static_cast<int>(services_limits::kMaxRoads));
				if (s->roads.size() >= services_limits::kMaxRoadSlots) return luaL_error(L, "too many road network slots (max %d per world)", static_cast<int>(services_limits::kMaxRoadSlots));
				std::string error;
				const auto text = AssetIO::ReadText(decl->path, &error);
				if (!text) return luaL_error(L, "road network '%s': %s", name.c_str(), error.c_str());
				const nlohmann::json json = nlohmann::json::parse(*text, nullptr, false);
				if (json.is_discarded()) return luaL_error(L, "road network '%s' is not valid JSON", name.c_str());
				auto network = std::make_unique<RoadNetwork>();
				if (!network->Load(json)) return luaL_error(L, "road network '%s' has no usable nodes and edges", name.c_str());
				index = s->roads.size();
				s->roads.push_back(std::move(network));
				s->roadIndex[name] = index;
			}
			PushHandle(L, kRoadMeta, index);
			return 1;
		}

		// nox.collision(name): collision meshes from a glTF declared in project.json "collision".
		int NoxCollision(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const std::string name = luaL_checkstring(L, 1);
			const DeclaredFile* decl = FindDeclared(s->declaredCollision, name);
			if (!decl) return luaL_error(L, "collision '%s' is not declared in project.json (collision)", name.c_str());
			size_t index = 0;
			const auto known = s->collisionIndex.find(name);
			if (known != s->collisionIndex.end()) {
				index = known->second;
			}
			else {
				if (s->collision.size() >= services_limits::kMaxCollision) {
					return luaL_error(L, "too many collision worlds (max %d)", static_cast<int>(services_limits::kMaxCollision));
				}
				auto world = std::make_unique<CollisionWorld>();
				if (world->LoadGLTF(decl->path) <= 0) return luaL_error(L, "collision '%s' has no triangle meshes", name.c_str());
				index = s->collision.size();
				s->collision.push_back(std::move(world));
				s->collisionIndex[name] = index;
			}
			PushHandle(L, kCollisionMeta, index);
			return 1;
		}

		// Handles are checked against the world's tables: a handle kept across a reload (or after release) refers to nothing.
		RoadNetwork& RoadOf(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const size_t index = HandleIndex(L, 1, kRoadMeta);
			if (index >= s->roads.size() || !s->roads[index]) luaL_error(L, "this road network handle is no longer valid");
			return *s->roads[index];
		}

		// road:route(x1, z1, x2, z2) -> array of {x=, z=} along the streets, or nil when there is no route.
		int RoadRoute(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			const glm::vec2 from(static_cast<float>(CheckFinite(L, 2)), static_cast<float>(CheckFinite(L, 3)));
			const glm::vec2 to(static_cast<float>(CheckFinite(L, 4)), static_cast<float>(CheckFinite(L, 5)));
			const std::vector<glm::vec2> route = net.FindRoute(from, to);
			if (route.empty()) {
				lua_pushnil(L);
				return 1;
			}
			const size_t count = std::min(route.size(), services_limits::kMaxRoutePoints);
			lua_createtable(L, static_cast<int>(count), 0);
			for (size_t i = 0; i < count; ++i) {
				lua_createtable(L, 0, 2);
				lua_pushnumber(L, route[i].x);
				lua_setfield(L, -2, "x");
				lua_pushnumber(L, route[i].y);
				lua_setfield(L, -2, "z");
				lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
			}
			return 1;
		}

		// road:nearest(x, z) -> x, z, distance, edge (1-based) of the closest point on the centre lines; nil when empty.
		int RoadNearest(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			if (net.Edges().empty()) {
				lua_pushnil(L);
				return 1;
			}
			const glm::vec2 p(static_cast<float>(CheckFinite(L, 2)), static_cast<float>(CheckFinite(L, 3)));
			glm::vec2 closest(0.0f);
			int edge = -1;
			const float distance = net.ClosestOnRoads(p, &closest, &edge, nullptr);
			lua_pushnumber(L, closest.x);
			lua_pushnumber(L, closest.y);
			lua_pushnumber(L, distance);
			lua_pushinteger(L, edge + 1);
			return 4;
		}

		// road:edge_polylines([segments]) -> array of flat {x1, z1, x2, z2, ...} arrays, one per edge, ready for ui.polyline.
		int RoadEdgePolylines(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			const int segments = lua_isnoneornil(L, 2) ? 4 : static_cast<int>(luaL_checkinteger(L, 2));
			if (segments < 1 || segments > 8) return luaL_error(L, "edge_polylines: segments must be 1..8");
			const size_t edges = net.Edges().size();
			if (edges * static_cast<size_t>(segments + 1) > services_limits::kMaxEdgeSegments) {
				return luaL_error(L, "edge_polylines: the network has too many edge segments for one call");
			}
			lua_createtable(L, static_cast<int>(edges), 0);
			for (size_t e = 0; e < edges; ++e) {
				lua_createtable(L, 2 * (segments + 1), 0);
				for (int k = 0; k <= segments; ++k) {
					const glm::vec3 p = net.EdgePoint(static_cast<int>(e), static_cast<float>(k) / static_cast<float>(segments));
					lua_pushnumber(L, p.x);
					lua_rawseti(L, -2, 2 * k + 1);
					lua_pushnumber(L, p.z);
					lua_rawseti(L, -2, 2 * k + 2);
				}
				lua_rawseti(L, -2, static_cast<lua_Integer>(e + 1));
			}
			return 1;
		}

		// road:bounds() -> minx, minz, maxx, maxz
		int RoadBounds(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			glm::vec2 mn(0.0f), mx(0.0f);
			net.Bounds(mn, mx);
			lua_pushnumber(L, mn.x);
			lua_pushnumber(L, mn.y);
			lua_pushnumber(L, mx.x);
			lua_pushnumber(L, mx.y);
			return 4;
		}

		int RoadEdgeCount(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			lua_pushinteger(L, static_cast<lua_Integer>(net.Edges().size()));
			return 1;
		}

		// nox.roads("name") (through the callable table) and nox.roads.from_table(tbl) share one handle space.
		int NoxRoadsCall(lua_State* L) {
			lua_remove(L, 1);   // the table itself: the name is argument 1 afterwards
			return NoxRoads(L);
		}

		// nox.roads.from_table(tbl): an inline network, the same schema as the scene's game.road_network (no "file").
		// Identical tables share one network (keyed by their canonical JSON), so rebuilding the same roads on every
		// Play costs no slot. Different tables take a slot; nox.roads.release(road) frees a live one (bounded by kMaxRoads).
		int NoxRoadsFromTable(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			luaL_checktype(L, 1, LUA_TTABLE);
			nlohmann::json json;
			std::string error;
			if (!LuaValueToJson(L, 1, json, error)) return luaL_error(L, "nox.roads.from_table: %s", error.c_str());
			if (!json.is_object()) return luaL_error(L, "nox.roads.from_table: the network must be a table with nodes and edges");
			if (json.contains("file")) {
				return luaL_error(L, "nox.roads.from_table takes the inline form; declare network files in project.json (roads)");
			}
			const std::string key = json.dump();
			const auto known = s->inlineRoadIndex.find(key);
			if (known != s->inlineRoadIndex.end() && known->second < s->roads.size() && s->roads[known->second]) {
				PushHandle(L, kRoadMeta, known->second);
				return 1;
			}
			if (LiveRoads(*s) >= services_limits::kMaxRoads) {
				return luaL_error(L, "too many road networks (max %d)", static_cast<int>(services_limits::kMaxRoads));
			}
			if (s->roads.size() >= services_limits::kMaxRoadSlots) {
				return luaL_error(L, "too many road network slots (max %d per world)", static_cast<int>(services_limits::kMaxRoadSlots));
			}
			auto network = std::make_unique<RoadNetwork>();
			if (!network->Load(json)) return luaL_error(L, "nox.roads.from_table: the network has no usable nodes and edges");
			const size_t index = s->roads.size();
			s->roads.push_back(std::move(network));
			s->inlineRoadIndex[key] = index;
			PushHandle(L, kRoadMeta, index);
			return 1;
		}

		// nox.roads.release(road): frees an inline network so the project can rebuild its roads. Declared networks
		// stay loaded for the world. A released handle is invalid afterwards (road calls on it raise an error).
		int NoxRoadsRelease(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const size_t index = HandleIndex(L, 1, kRoadMeta);
			if (index >= s->roads.size() || !s->roads[index]) return 0;   // already released
			for (const auto& [name, declared] : s->roadIndex) {
				if (declared == index) return luaL_error(L, "declared road network '%s' cannot be released", name.c_str());
			}
			for (auto it = s->inlineRoadIndex.begin(); it != s->inlineRoadIndex.end();) {
				if (it->second == index) it = s->inlineRoadIndex.erase(it);
				else ++it;
			}
			s->roads[index].reset();
			return 0;
		}

		// Lua edge indices are 1-based (as nearest() and edge_polylines() return them).
		size_t EdgeIndexArg(lua_State* L, const RoadNetwork& net, int idx) {
			const lua_Integer i = luaL_checkinteger(L, idx);
			if (i < 1 || static_cast<size_t>(i) > net.Edges().size()) {
				luaL_error(L, "edge index %d is out of range (1..%d)", static_cast<int>(i), static_cast<int>(net.Edges().size()));
			}
			return static_cast<size_t>(i - 1);
		}

		// road:edges_in_rect(minx, minz, maxx, maxz) -> {edge indices}: edges whose band (road + sidewalk) meets the rectangle.
		int RoadEdgesInRect(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			const float x0 = static_cast<float>(CheckFinite(L, 2)), z0 = static_cast<float>(CheckFinite(L, 3));
			const float x1 = static_cast<float>(CheckFinite(L, 4)), z1 = static_cast<float>(CheckFinite(L, 5));
			if (x0 > x1 || z0 > z1) return luaL_error(L, "edges_in_rect: need minx <= maxx and minz <= maxz");
			const auto& nodes = net.Nodes();
			const auto& edges = net.Edges();
			const float sidewalk = net.SidewalkWidth();
			lua_createtable(L, 0, 0);
			lua_Integer count = 0;
			for (size_t e = 0; e < edges.size(); ++e) {
				const RoadNetwork::Edge& edge = edges[e];
				if (edge.a < 0 || edge.b < 0 || static_cast<size_t>(edge.a) >= nodes.size() || static_cast<size_t>(edge.b) >= nodes.size()) continue;
				const glm::vec2 pa = nodes[edge.a].pos, pb = nodes[edge.b].pos;
				const float reach = edge.halfWidth + sidewalk;
				const float ex0 = std::min(pa.x, pb.x) - reach, ex1 = std::max(pa.x, pb.x) + reach;
				const float ez0 = std::min(pa.y, pb.y) - reach, ez1 = std::max(pa.y, pb.y) + reach;
				if (ex1 < x0 || ex0 > x1 || ez1 < z0 || ez0 > z1) continue;
				lua_pushinteger(L, static_cast<lua_Integer>(e + 1));
				lua_rawseti(L, -2, ++count);
			}
			return 1;
		}

		// road:edge_polyline(i [, segments]) -> flat {x1, z1, x2, z2, ...} for one edge (segments 1..8, default 4).
		int RoadEdgePolyline(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			const size_t e = EdgeIndexArg(L, net, 2);
			const int segments = lua_isnoneornil(L, 3) ? 4 : static_cast<int>(luaL_checkinteger(L, 3));
			if (segments < 1 || segments > 8) return luaL_error(L, "edge_polyline: segments must be 1..8");
			lua_createtable(L, 2 * (segments + 1), 0);
			for (int k = 0; k <= segments; ++k) {
				const glm::vec3 p = net.EdgePoint(static_cast<int>(e), static_cast<float>(k) / static_cast<float>(segments));
				lua_pushnumber(L, p.x);
				lua_rawseti(L, -2, 2 * k + 1);
				lua_pushnumber(L, p.z);
				lua_rawseti(L, -2, 2 * k + 2);
			}
			return 1;
		}

		// road:edge_info(i) -> halfWidth, y (mean node height), attributes {name, oneway, lanes, lane_width, carriageway, from, to}
		int RoadEdgeInfo(lua_State* L) {
			RoadNetwork& net = RoadOf(L);
			const size_t e = EdgeIndexArg(L, net, 2);
			const RoadNetwork::Edge& edge = net.Edges()[e];
			const auto& nodes = net.Nodes();
			if (edge.a < 0 || edge.b < 0 || static_cast<size_t>(edge.a) >= nodes.size() || static_cast<size_t>(edge.b) >= nodes.size()) {
				return luaL_error(L, "edge_info: edge %d has no valid nodes", static_cast<int>(e + 1));
			}
			lua_pushnumber(L, edge.halfWidth);
			lua_pushnumber(L, 0.5f * (nodes[edge.a].y + nodes[edge.b].y));
			lua_createtable(L, 0, 7);
			lua_pushlstring(L, edge.name.data(), edge.name.size());
			lua_setfield(L, -2, "name");
			lua_pushboolean(L, edge.oneway ? 1 : 0);
			lua_setfield(L, -2, "oneway");
			lua_pushinteger(L, edge.lanes);
			lua_setfield(L, -2, "lanes");
			lua_pushnumber(L, edge.laneWidth);
			lua_setfield(L, -2, "lane_width");
			lua_pushlstring(L, edge.carriageway.data(), edge.carriageway.size());
			lua_setfield(L, -2, "carriageway");
			lua_pushlstring(L, nodes[edge.a].id.data(), nodes[edge.a].id.size());
			lua_setfield(L, -2, "from");
			lua_pushlstring(L, nodes[edge.b].id.data(), nodes[edge.b].id.size());
			lua_setfield(L, -2, "to");
			return 3;
		}

		CollisionWorld& CollisionOf(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const size_t index = HandleIndex(L, 1, kCollisionMeta);
			if (index >= s->collision.size()) luaL_error(L, "this collision handle is no longer valid");
			return *s->collision[index];
		}

		// nox.collision_file(logical) -> collision handle for a glTF the project declares (collision) or the scene names in
		// game.collision.files. Same handle space and limits as nox.collision(name).
		int NoxCollisionFile(lua_State* L) {
			LuaServices* s = ServicesOf(L);
			const std::string requested = luaL_checkstring(L, 1);
			std::string error;
			const auto logical = AssetIO::Normalize(requested, &error);
			if (!logical) return luaL_error(L, "nox.collision_file: %s", error.c_str());
			bool allowed = std::find(s->sceneCollisionFiles.begin(), s->sceneCollisionFiles.end(), *logical) != s->sceneCollisionFiles.end();
			for (const DeclaredFile& decl : s->declaredCollision) {
				if (decl.path == *logical) allowed = true;
			}
			if (!allowed) {
				return luaL_error(L, "nox.collision_file: '%s' is neither declared in project.json (collision) nor named by the scene's game.collision.files", logical->c_str());
			}
			const std::string key = "file:" + *logical;
			size_t index = 0;
			const auto known = s->collisionIndex.find(key);
			if (known != s->collisionIndex.end()) {
				index = known->second;
			}
			else {
				if (s->collision.size() >= services_limits::kMaxCollision) {
					return luaL_error(L, "too many collision worlds (max %d)", static_cast<int>(services_limits::kMaxCollision));
				}
				auto world = std::make_unique<CollisionWorld>();
				if (world->LoadGLTF(*logical) <= 0) return luaL_error(L, "nox.collision_file: '%s' has no triangle meshes", logical->c_str());
				index = s->collision.size();
				s->collision.push_back(std::move(world));
				s->collisionIndex[key] = index;
			}
			PushHandle(L, kCollisionMeta, index);
			return 1;
		}

		// collision:ground_height(x, z [, fromY, maxDrop]) -> y of the walkable surface below (fromY, default 1000), nil if none.
		int CollisionGroundHeight(lua_State* L) {
			CollisionWorld& world = CollisionOf(L);
			const float x = static_cast<float>(CheckFinite(L, 2)), z = static_cast<float>(CheckFinite(L, 3));
			const float fromY = OptFinite(L, 4, 1000.0f);
			const float maxDrop = OptFinite(L, 5, 2000.0f);
			if (!(maxDrop > 0.0f)) return luaL_error(L, "ground_height: maxDrop must be positive");
			CollisionWorld::Hit hit;
			if (!world.GroundBelow(glm::vec3(x, fromY, z), maxDrop, hit)) {
				lua_pushnil(L);
				return 1;
			}
			lua_pushnumber(L, hit.position.y);
			return 1;
		}

		uint8_t ClassMask(lua_State* L, int idx) {
			if (lua_isnoneornil(L, idx)) return CollisionWorld::Any;
			const lua_Integer mask = luaL_checkinteger(L, idx);
			if (mask < 1 || mask > CollisionWorld::Any) luaL_error(L, "class mask must be 1..7 (1 ground, 2 wall, 4 ceiling)");
			return static_cast<uint8_t>(mask);
		}

		// collision:raycast(ox, oy, oz, dx, dy, dz, maxT [, mask]) -> {t, x, y, z, nx, ny, nz} or nil.
		int CollisionRaycast(lua_State* L) {
			CollisionWorld& world = CollisionOf(L);
			const glm::vec3 origin = CheckVec3(L, 2);
			const glm::vec3 dir = CheckVec3(L, 5);
			const double maxT = CheckFinite(L, 8);
			if (!(glm::length(dir) > 1e-6f)) return luaL_error(L, "raycast: direction must not be zero");
			if (maxT <= 0.0 || maxT > 100000.0) return luaL_error(L, "raycast: maxT must be in (0, 100000]");
			const uint8_t mask = ClassMask(L, 9);
			CollisionWorld::Hit hit;
			if (!world.Raycast(origin, glm::normalize(dir), static_cast<float>(maxT), mask, hit)) {
				lua_pushnil(L);
				return 1;
			}
			lua_createtable(L, 0, 7);
			lua_pushnumber(L, hit.t);
			lua_setfield(L, -2, "t");
			lua_pushnumber(L, hit.position.x);
			lua_setfield(L, -2, "x");
			lua_pushnumber(L, hit.position.y);
			lua_setfield(L, -2, "y");
			lua_pushnumber(L, hit.position.z);
			lua_setfield(L, -2, "z");
			lua_pushnumber(L, hit.normal.x);
			lua_setfield(L, -2, "nx");
			lua_pushnumber(L, hit.normal.y);
			lua_setfield(L, -2, "ny");
			lua_pushnumber(L, hit.normal.z);
			lua_setfield(L, -2, "nz");
			return 1;
		}

		// collision:sphere_push(cx, cy, cz, radius [, mask]) -> x, y, z (centre pushed out of the surfaces) and the
		// contact normal nx, ny, nz (zero when nothing touches). This is an overlap query, not a sweep.
		int CollisionSpherePush(lua_State* L) {
			CollisionWorld& world = CollisionOf(L);
			glm::vec3 centre = CheckVec3(L, 2);
			const double radius = CheckFinite(L, 5);
			if (!(radius > 0.0 && radius <= 10000.0)) return luaL_error(L, "sphere_push: radius must be in (0, 10000]");
			const uint8_t mask = ClassMask(L, 6);
			glm::vec3 normal(0.0f);
			world.ResolveSphere(centre, static_cast<float>(radius), mask, &normal);
			PushVec3(L, centre);
			PushVec3(L, normal);
			return 6;
		}

		void SetMetatable(lua_State* L, const char* meta, const luaL_Reg* methods) {
			luaL_newmetatable(L, meta);
			lua_newtable(L);
			luaL_setfuncs(L, methods, 0);
			lua_setfield(L, -2, "__index");
			lua_pushstring(L, meta);
			lua_setfield(L, -2, "__name");
			lua_pop(L, 1);
		}

		void SetField(lua_State* L, int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushcfunction(L, fn);
			lua_setfield(L, tableIdx, name);
		}
	}

	void ResetServices(LuaServices& services, const std::vector<DeclaredFile>& roads, const std::vector<DeclaredFile>& collision) {
		services.declaredRoads = roads;
		services.declaredCollision = collision;
		services.cameraCut = false;
		services.fxEmitsThisFrame = 0;
		if (services.fxUsed) VehicleFX::Clear();
		services.fxUsed = false;
		services.audio = VehicleAudioState{};
		services.audioUsed = false;
		services.audioCues.clear();
		services.roads.clear();
		services.roadIndex.clear();
		services.inlineRoadIndex.clear();
		services.collision.clear();
		services.collisionIndex.clear();
	}

	void InstallServicesApi(lua_State* L, int ctxIdx, int noxIdx, LuaServices* services) {
		ctxIdx = lua_absindex(L, ctxIdx);
		noxIdx = lua_absindex(L, noxIdx);
		lua_pushlightuserdata(L, services);
		lua_setfield(L, LUA_REGISTRYINDEX, kServicesKey);

		SetField(L, ctxIdx, "camera_look_at", CtxCameraLookAt);
		SetField(L, ctxIdx, "camera_position", CtxCameraPosition);
		SetField(L, ctxIdx, "camera_set_fov", CtxCameraSetFov);
		SetField(L, ctxIdx, "camera_set_clip", CtxCameraSetClip);
		SetField(L, ctxIdx, "camera_cut", CtxCameraCut);
		SetField(L, ctxIdx, "camera_project", CtxCameraProject);
		SetField(L, ctxIdx, "overlay_rect", CtxOverlayRect);
		SetField(L, ctxIdx, "fx_smoke", CtxFxSmoke);
		SetField(L, ctxIdx, "fx_spray", CtxFxSpray);
		SetField(L, ctxIdx, "fx_set_enabled", CtxFxSetEnabled);
		SetField(L, ctxIdx, "fx_clear", CtxFxClear);
		SetField(L, ctxIdx, "load_user_json", CtxLoadUserJson);
		SetField(L, ctxIdx, "save_user_json", CtxSaveUserJson);
		SetField(L, ctxIdx, "audio_set", CtxAudioSet);
		SetField(L, ctxIdx, "audio_cue", CtxAudioCue);
		SetField(L, ctxIdx, "fx_count", CtxFxCount);

		// nox.roads is callable (nox.roads("declared name")) and carries from_table (nox.roads.from_table(tbl)).
		lua_newtable(L);                              // nox.roads
		lua_pushcfunction(L, NoxRoadsFromTable);
		lua_setfield(L, -2, "from_table");
		lua_pushcfunction(L, NoxRoadsRelease);
		lua_setfield(L, -2, "release");
		lua_newtable(L);                              // its metatable
		lua_pushcfunction(L, NoxRoadsCall);
		lua_setfield(L, -2, "__call");
		lua_setmetatable(L, -2);
		lua_setfield(L, noxIdx, "roads");
		SetField(L, noxIdx, "collision", NoxCollision);
		SetField(L, noxIdx, "collision_file", NoxCollisionFile);

		static const luaL_Reg roadMethods[] = {
			{ "route", RoadRoute }, { "nearest", RoadNearest }, { "edge_polylines", RoadEdgePolylines },
			{ "bounds", RoadBounds }, { "edge_count", RoadEdgeCount },
			{ "edges_in_rect", RoadEdgesInRect }, { "edge_polyline", RoadEdgePolyline }, { "edge_info", RoadEdgeInfo },
			{ nullptr, nullptr },
		};
		static const luaL_Reg collisionMethods[] = {
			{ "ground_height", CollisionGroundHeight }, { "raycast", CollisionRaycast },
			{ "sphere_push", CollisionSpherePush }, { nullptr, nullptr },
		};
		SetMetatable(L, kRoadMeta, roadMethods);
		SetMetatable(L, kCollisionMeta, collisionMethods);
	}

	// ---- Validation and conversion --------------------------------------------------------------------

	bool ValidateUserFileName(const std::string& name, std::string& error) {
		if (name.empty() || name.size() > services_limits::kMaxUserNameLength) {
			error = "save name must be 1..96 characters";
			return false;
		}
		if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) {
			error = "save name must end with .json";
			return false;
		}
		int segments = 1;
		size_t start = 0;
		for (size_t i = 0; i <= name.size(); ++i) {
			if (i == name.size() || name[i] == '/') {
				if (i == start) {
					error = "save name has an empty path segment";
					return false;
				}
				if (name[start] == '.') {
					error = "save name segments must not start with '.'";
					return false;
				}
				start = i + 1;
				if (i < name.size()) ++segments;
				continue;
			}
			const char c = name[i];
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
			if (!ok) {
				error = "save name may contain only letters, digits, _, -, . and /";
				return false;
			}
		}
		if (segments > 4) {
			error = "save name may have at most 4 path segments";
			return false;
		}
		return true;
	}

	namespace {
		struct JsonBudget {
			int nodes = 0;
			size_t bytes = 0;
		};

		// Arrays from JSON (and nox.array) carry a metatable marker, so an empty array saves as [] and not {}.
		constexpr const char* kJsonArrayMetaKey = "nox.array.meta";

		bool IsJsonArrayMarked(lua_State* L, int idx) {
			if (!lua_getmetatable(L, idx)) return false;
			lua_getfield(L, LUA_REGISTRYINDEX, kJsonArrayMetaKey);
			const bool same = lua_rawequal(L, -1, -2) != 0;
			lua_pop(L, 2);
			return same;
		}

		bool ToJson(lua_State* L, int idx, int depth, JsonBudget& budget, nlohmann::json& out, std::string& error) {
			idx = lua_absindex(L, idx);
			if (++budget.nodes > services_limits::kMaxJsonNodes) {
				error = "save data has more than " + std::to_string(services_limits::kMaxJsonNodes) + " values";
				return false;
			}
			if (depth > services_limits::kMaxJsonDepth) {
				error = "save data is nested deeper than " + std::to_string(services_limits::kMaxJsonDepth) + " levels";
				return false;
			}
			switch (lua_type(L, idx)) {
			case LUA_TNIL:
				out = nullptr;
				return true;
			case LUA_TBOOLEAN:
				out = lua_toboolean(L, idx) != 0;
				return true;
			case LUA_TNUMBER:
				if (lua_isinteger(L, idx)) {
					out = static_cast<int64_t>(lua_tointeger(L, idx));
					return true;
				}
				{
					const double d = lua_tonumber(L, idx);
					if (!std::isfinite(d)) {
						error = "save data cannot contain NaN or infinity";
						return false;
					}
					out = d;
					return true;
				}
			case LUA_TSTRING: {
				size_t length = 0;
				const char* text = lua_tolstring(L, idx, &length);
				if (length > services_limits::kMaxStringBytes) {
					error = "a string in save data is longer than 64 KiB";
					return false;
				}
				budget.bytes += length;
				if (budget.bytes > services_limits::kMaxUserFileBytes) {
					error = "save data is larger than the 1 MiB file limit";
					return false;
				}
				out = std::string(text, length);
				return true;
			}
			case LUA_TTABLE: {
				const bool marked = IsJsonArrayMarked(L, idx);
				size_t count = 0;
				lua_Integer maxIndex = 0;
				bool intKeys = false, stringKeys = false, bad = false;
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					++count;
					if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
						intKeys = true;
						const lua_Integer k = lua_tointeger(L, -2);
						if (k < 1) bad = true;
						else if (k > maxIndex) maxIndex = k;
					}
					else if (lua_type(L, -2) == LUA_TSTRING) {
						stringKeys = true;
					}
					else {
						bad = true;
					}
					lua_pop(L, 1);
				}
				if (bad || (intKeys && stringKeys)) {
					error = "tables in save data must be arrays (keys 1..n) or objects (string keys)";
					return false;
				}
				if (marked && stringKeys) {
					error = "nox.array tables cannot have string keys";
					return false;
				}
				if (intKeys || marked) {
					if (static_cast<size_t>(maxIndex) != count) {
						error = "array tables in save data must have keys 1..n without holes";
						return false;
					}
					out = nlohmann::json::array();
					for (lua_Integer i = 1; i <= maxIndex; ++i) {
						lua_rawgeti(L, idx, i);
						nlohmann::json child;
						const bool ok = ToJson(L, -1, depth + 1, budget, child, error);
						lua_pop(L, 1);
						if (!ok) return false;
						out.push_back(std::move(child));
					}
					return true;
				}
				out = nlohmann::json::object();
				lua_pushnil(L);
				while (lua_next(L, idx) != 0) {
					size_t keyLength = 0;
					const char* key = lua_tolstring(L, -2, &keyLength);
					nlohmann::json child;
					if (!ToJson(L, -1, depth + 1, budget, child, error)) {
						lua_pop(L, 2);
						return false;
					}
					out[std::string(key, keyLength)] = std::move(child);
					lua_pop(L, 1);
				}
				return true;
			}
			default:
				error = std::string("save data cannot contain values of type ") + lua_typename(L, lua_type(L, idx));
				return false;
			}
		}
	}

	void MarkJsonArray(lua_State* L, int idx) {
		idx = lua_absindex(L, idx);
		lua_getfield(L, LUA_REGISTRYINDEX, kJsonArrayMetaKey);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_newtable(L);
			lua_pushstring(L, "nox.array");
			lua_setfield(L, -2, "__name");
			lua_pushvalue(L, -1);
			lua_setfield(L, LUA_REGISTRYINDEX, kJsonArrayMetaKey);
		}
		lua_setmetatable(L, idx);
	}

	bool LuaValueToJson(lua_State* L, int idx, nlohmann::json& out, std::string& error) {
		JsonBudget budget;
		return ToJson(L, idx, 0, budget, out, error);
	}

	void PushJsonValue(lua_State* L, const nlohmann::json& value) {
		switch (value.type()) {
		case nlohmann::json::value_t::null:
			lua_pushnil(L);
			return;
		case nlohmann::json::value_t::boolean:
			lua_pushboolean(L, value.get<bool>() ? 1 : 0);
			return;
		case nlohmann::json::value_t::number_integer:
			lua_pushinteger(L, static_cast<lua_Integer>(value.get<int64_t>()));
			return;
		case nlohmann::json::value_t::number_unsigned:
			lua_pushinteger(L, static_cast<lua_Integer>(value.get<uint64_t>()));
			return;
		case nlohmann::json::value_t::number_float:
			lua_pushnumber(L, value.get<double>());
			return;
		case nlohmann::json::value_t::string: {
			const std::string& text = value.get_ref<const std::string&>();
			lua_pushlstring(L, text.data(), text.size());
			return;
		}
		case nlohmann::json::value_t::array: {
			lua_createtable(L, static_cast<int>(value.size()), 0);
			MarkJsonArray(L, -1);
			lua_Integer i = 1;
			for (const nlohmann::json& child : value) {
				PushJsonValue(L, child);
				lua_rawseti(L, -2, i++);
			}
			return;
		}
		case nlohmann::json::value_t::object: {
			lua_createtable(L, 0, static_cast<int>(value.size()));
			for (auto it = value.begin(); it != value.end(); ++it) {
				PushJsonValue(L, it.value());
				lua_setfield(L, -2, it.key().c_str());
			}
			return;
		}
		default:
			lua_pushnil(L);
			return;
		}
	}
}
