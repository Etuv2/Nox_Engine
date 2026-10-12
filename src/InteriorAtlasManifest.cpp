#include "InteriorAtlasManifest.h"

#include "assets/AssetIO.h"
#include "assets/AssetIOJson.h"

#include <glm/glm.hpp>
#include <iostream>
#include <string_view>

using json = nlohmann::json;

namespace {

	std::string JoinLogical(const std::string& root, const std::string& file)
	{
		return root.empty() ? file : root + "/" + file;
	}

	void ReadVec2(const json& j, const char* key, glm::vec2& v)
	{
		if (j.contains(key) && j[key].is_array() && j[key].size() >= 2) {
			v = glm::vec2(j[key][0].get<float>(), j[key][1].get<float>());
		}
	}

	void ReadVec3(const json& j, glm::vec3& v)
	{
		if (j.is_array() && j.size() >= 3) v = glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
	}

}

namespace InteriorAtlasManifest {

	bool Load(const std::string& root, InteriorAtlas& out)
	{
		const char* tag = "[InteriorAtlas] ";
		const json manifest = root.empty() ? json() : AssetIO::ReadJsonOrNull(JoinLogical(root, "atlas_manifest.json"));
		if (!manifest.is_object() || !manifest.contains("assets") || !manifest["assets"].is_array() || manifest["assets"].empty()) {
			std::cerr << tag << "cannot read atlas_manifest.json under '" << root << "'" << std::endl;
			return false;
		}
		const json nox = manifest.value("nox_interior", json::object());

		InteriorAtlas atlas;
		// Face slots: nox_interior.face_order, else the manifest's face names.
		std::vector<std::string> faceNames;
		if (nox.contains("face_order") && nox["face_order"].is_array()) {
			for (const json& f : nox["face_order"]) if (f.is_string()) faceNames.push_back(f.get<std::string>());
		}
		else if (manifest.contains("faces") && manifest["faces"].is_object()) {
			for (auto it = manifest["faces"].begin(); it != manifest["faces"].end(); ++it) faceNames.push_back(it.key());
		}
		for (const std::string& name : faceNames) {
			InteriorAtlas::FaceRole role;
			if (!InteriorAtlas::FaceRoleFromName(name, role)) {
				std::cerr << tag << "unknown face '" << name << "'" << std::endl;
				return false;
			}
			atlas.faceRoles.push_back(role);
		}
		if (atlas.faceRoles.empty()) {
			std::cerr << tag << "no faces listed" << std::endl;
			return false;
		}

		std::vector<std::string> roomFiles;
		for (const json& asset : manifest["assets"]) {
			const std::string file = asset.value("file", std::string());
			if (file.empty() || !AssetIO::Exists(JoinLogical(root, file)) || !asset.contains("faces")) {
				std::cerr << tag << "asset '" << file << "' is missing" << std::endl;
				return false;
			}
			const json& faces = asset["faces"];
			for (const std::string& faceName : faceNames) {
				if (!faces.contains(faceName) || !faces[faceName].contains("sample_uv_rect") || faces[faceName]["sample_uv_rect"].size() < 4) {
					std::cerr << tag << file << " lacks face " << faceName << std::endl;
					return false;
				}
				const json& face = faces[faceName];
				// sample_uv_rect: pixel-centre bounds with a 2-pixel inset, image top-left origin.
				const json& uv = face["sample_uv_rect"];
				atlas.faceLayers.push_back({ JoinLogical(root, file),
					glm::vec4(uv[0].get<float>(), uv[1].get<float>(), uv[2].get<float>(), uv[3].get<float>()) });
				glm::vec3 mean(0.1f);
				if (face.contains("mean_linear_rgb")) ReadVec3(face["mean_linear_rgb"], mean);
				atlas.faceMean.push_back(mean);
			}
			roomFiles.push_back(file);
		}
		const int rooms = static_cast<int>(roomFiles.size());
		auto roomIndex = [&](const std::string& file) {
			for (int r = 0; r < rooms; ++r) if (roomFiles[r] == file) return r;
			return -1;
		};

		atlas.textureOptions.size = nox.value("face_size", 512);
		atlas.textureOptions.premultiply = false;   // opaque RGB rooms
		atlas.textureOptions.debugName = "window rooms";

		// Room light: (occupied brightness, chance on when unoccupied, that brightness).
		glm::vec3 lightDefault(1.0f, 0.0f, 0.0f);
		if (nox.contains("room_light_default")) ReadVec3(nox["room_light_default"], lightDefault);
		atlas.roomLight.assign(rooms, lightDefault);
		if (nox.contains("room_light") && nox["room_light"].is_object()) {
			for (auto it = nox["room_light"].begin(); it != nox["room_light"].end(); ++it) {
				const int r = roomIndex(it.key());
				if (r < 0) { std::cerr << tag << "room_light names unknown room " << it.key() << std::endl; continue; }
				ReadVec3(it.value(), atlas.roomLight[r]);
			}
		}

		// Variant weights per material style; the row order is the style index the scene uses.
		if (nox.contains("styles") && nox["styles"].is_array()) {
			for (const json& style : nox["styles"]) {
				std::vector<float> row(rooms, 0.0f);
				if (style.contains("weights") && style["weights"].is_object()) {
					for (auto it = style["weights"].begin(); it != style["weights"].end(); ++it) {
						const int r = roomIndex(it.key());
						if (r < 0) { std::cerr << tag << "style weights name unknown room " << it.key() << std::endl; continue; }
						row[r] = it.value().get<float>();
					}
				}
				atlas.styleWeights.push_back(std::move(row));
			}
		}

		if (nox.contains("room_shape") && nox["room_shape"].is_object()) {
			const json& shape = nox["room_shape"];
			ReadVec2(shape, "side_margin", atlas.roomShape.sideMargin);
			ReadVec2(shape, "height", atlas.roomShape.height);
			ReadVec2(shape, "sill_scale", atlas.roomShape.sillScale);
			atlas.roomShape.depthJitter = shape.value("depth_jitter", atlas.roomShape.depthJitter);
		}
		if (nox.contains("pane_uv") && nox["pane_uv"].is_array() && nox["pane_uv"].size() >= 3) {
			atlas.paneUV.grid = nox["pane_uv"][0].get<float>();
			atlas.paneUV.inset = nox["pane_uv"][1].get<float>();
			atlas.paneUV.extent = nox["pane_uv"][2].get<float>();
		}
		atlas.farDistance = nox.value("far_distance", atlas.farDistance);
		out = std::move(atlas);
		return true;
	}
}
