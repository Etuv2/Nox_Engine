// Headless tests for cooked procedural results (src/cook/CookedResults.*): format round trip including render
// extras, key stability and sensitivity, invalid/missing result handling, and the loose-tree file flow.
#include "TestFramework.h"

#include "../../src/assets/AssetIO.h"
#include "../../src/cook/CookedResults.h"
#include "../../src/cook/Sha256.h"
#include "../../src/SceneExtensions.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

	uint32_t Bits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	bool SameFloats(const float* a, const float* b, int count)
	{
		for (int i = 0; i < count; ++i) if (Bits(a[i]) != Bits(b[i])) return false;
		return true;
	}

	bool SameLayers(const std::vector<TextureArrayBuilder::Layer>& a, const std::vector<TextureArrayBuilder::Layer>& b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i].file != b[i].file || !SameFloats(&a[i].rect.x, &b[i].rect.x, 4)) return false;
		}
		return true;
	}

	bool SameOptions(const TextureArrayBuilder::Options& a, const TextureArrayBuilder::Options& b)
	{
		return a.size == b.size && a.premultiply == b.premultiply && Bits(a.alphaFloor) == Bits(b.alphaFloor) &&
			a.anisotropic == b.anisotropic && a.debugName == b.debugName;
	}

	// Every stored field, bit for bit (the revision is runtime state and is not compared).
	bool SameExtras(const SceneRenderExtras& a, const SceneRenderExtras& b)
	{
		if (a.decalSets.size() != b.decalSets.size()) return false;
		for (size_t s = 0; s < a.decalSets.size(); ++s) {
			const DecalSet& x = a.decalSets[s];
			const DecalSet& y = b.decalSets[s];
			if (x.name != y.name || !SameLayers(x.layers, y.layers) || !SameOptions(x.textureOptions, y.textureOptions)) return false;
			if (Bits(x.material.albedoScale) != Bits(y.material.albedoScale) || Bits(x.material.roughness) != Bits(y.material.roughness) ||
				Bits(x.material.wetnessResponse) != Bits(y.material.wetnessResponse) || Bits(x.material.puddleResponse) != Bits(y.material.puddleResponse)) return false;
			if (x.instances.size() != y.instances.size()) return false;
			for (size_t i = 0; i < x.instances.size(); ++i) {
				const DecalInstance& p = x.instances[i];
				const DecalInstance& q = y.instances[i];
				if (!SameFloats(&p.center.x, &q.center.x, 3) || !SameFloats(&p.axisU.x, &q.axisU.x, 3) ||
					!SameFloats(&p.axisV.x, &q.axisV.x, 3) || !SameFloats(&p.uvRect.x, &q.uvRect.x, 4) ||
					p.layer != q.layer || !SameFloats(&p.tint.x, &q.tint.x, 4)) return false;
			}
		}
		const LensFlareSet& f = a.lensFlares;
		const LensFlareSet& g = b.lensFlares;
		if (!SameLayers(f.layers, g.layers) || !SameOptions(f.textureOptions, g.textureOptions)) return false;
		if (f.styles.size() != g.styles.size() || f.sources.size() != g.sources.size()) return false;
		for (size_t s = 0; s < f.styles.size(); ++s) {
			if (f.styles[s].layers.size() != g.styles[s].layers.size()) return false;
			for (size_t l = 0; l < f.styles[s].layers.size(); ++l) {
				const LensFlareLayer& p = f.styles[s].layers[l];
				const LensFlareLayer& q = g.styles[s].layers[l];
				if (p.textureLayer != q.textureLayer || Bits(p.gain) != Bits(q.gain) || Bits(p.width) != Bits(q.width) ||
					!SameFloats(&p.tint.x, &q.tint.x, 3) || p.useSourceTint != q.useSourceTint ||
					Bits(p.glareMin) != Bits(q.glareMin) || Bits(p.glareWeight) != Bits(q.glareWeight)) return false;
			}
		}
		for (size_t i = 0; i < f.sources.size(); ++i) {
			const LensFlareSource& p = f.sources[i];
			const LensFlareSource& q = g.sources[i];
			if (!SameFloats(&p.position.x, &q.position.x, 3) || p.style != q.style || !SameFloats(&p.facing.x, &q.facing.x, 3) ||
				Bits(p.luminance) != Bits(q.luminance) || !SameFloats(&p.tint.x, &q.tint.x, 3) || Bits(p.size) != Bits(q.size)) return false;
		}
		if (Bits(f.luminanceToGlare) != Bits(g.luminanceToGlare) || Bits(f.gain) != Bits(g.gain) ||
			Bits(f.minPixels) != Bits(g.minPixels) || Bits(f.maxPixels) != Bits(g.maxPixels) ||
			Bits(f.referenceHeight) != Bits(g.referenceHeight) || Bits(f.fadeStart) != Bits(g.fadeStart) ||
			Bits(f.fadeEnd) != Bits(g.fadeEnd) || Bits(f.visibilityRate) != Bits(g.visibilityRate)) return false;

		const InteriorAtlas& p = a.interiorAtlas;
		const InteriorAtlas& q = b.interiorAtlas;
		if (p.faceRoles != q.faceRoles || !SameLayers(p.faceLayers, q.faceLayers) || !SameOptions(p.textureOptions, q.textureOptions)) return false;
		if (p.faceMean.size() != q.faceMean.size() || p.roomLight.size() != q.roomLight.size() || p.styleWeights.size() != q.styleWeights.size()) return false;
		for (size_t i = 0; i < p.faceMean.size(); ++i) if (!SameFloats(&p.faceMean[i].x, &q.faceMean[i].x, 3)) return false;
		for (size_t i = 0; i < p.roomLight.size(); ++i) if (!SameFloats(&p.roomLight[i].x, &q.roomLight[i].x, 3)) return false;
		for (size_t i = 0; i < p.styleWeights.size(); ++i) {
			if (p.styleWeights[i].size() != q.styleWeights[i].size() || !SameFloats(p.styleWeights[i].data(), q.styleWeights[i].data(), static_cast<int>(p.styleWeights[i].size()))) return false;
		}
		return SameFloats(&p.roomShape.sideMargin.x, &q.roomShape.sideMargin.x, 2) && SameFloats(&p.roomShape.height.x, &q.roomShape.height.x, 2) &&
			Bits(p.roomShape.depthJitter) == Bits(q.roomShape.depthJitter) && SameFloats(&p.roomShape.sillScale.x, &q.roomShape.sillScale.x, 2) &&
			Bits(p.paneUV.grid) == Bits(q.paneUV.grid) && Bits(p.paneUV.inset) == Bits(q.paneUV.inset) &&
			Bits(p.paneUV.extent) == Bits(q.paneUV.extent) && Bits(p.farDistance) == Bits(q.farDistance);
	}

	// Values chosen to be awkward in decimal: they must survive text round trips exactly.
	SceneRenderExtras SampleExtras()
	{
		SceneRenderExtras e;
		DecalSet set;
		set.name = "road_paint";
		set.layers = { { "textures/road_decals/arrow.png", glm::vec4(0.1f, 0.2f, 0.9f, 0.8f) }, { "textures/road_decals/line.png" } };
		set.textureOptions.size = 1024;
		set.textureOptions.alphaFloor = 1.0f / 3.0f;
		set.textureOptions.debugName = "road paint";
		set.material = { 0.37f, 0.61f, 0.123456789f, 1e-7f };
		DecalInstance d;
		d.center = glm::vec3(1.0f / 3.0f, -2.5e-5f, 1e3f);
		d.axisU = glm::vec3(0.7071068f, 0.0f, 0.7071068f);
		d.axisV = glm::vec3(0.0f, 0.0f, -1.0f);
		d.uvRect = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
		d.layer = 3;
		d.tint = glm::vec4(1.0f, 0.5f, 0.25f, 0.125f);
		set.instances = { d, DecalInstance{} };
		e.decalSets.push_back(set);

		e.lensFlares.layers = { { "textures/lens_flares/glow.png", glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) } };
		e.lensFlares.textureOptions.size = 512;
		e.lensFlares.textureOptions.anisotropic = false;
		LensFlareStyle style;
		style.layers = { LensFlareLayer{ 0, 1.1f, 0.3f, glm::vec3(1.0f, 0.9f, 0.8f), true, 0.05f, 0.7f },
			LensFlareLayer{ 0, 0.25f, 2.0f, glm::vec3(0.5f), false, 0.0f, 0.0f } };
		e.lensFlares.styles = { style };
		LensFlareSource source;
		source.position = glm::vec3(1.5f, 9.25f, -3.125f);
		source.style = 0;
		source.facing = glm::vec3(0.0f, -1.0f, 0.0f);
		source.luminance = 4321.5f;
		source.tint = glm::vec3(0.9f, 0.95f, 1.0f);
		source.size = 0.6f;
		e.lensFlares.sources = { source };
		e.lensFlares.luminanceToGlare = 1.2e-3f;
		e.lensFlares.gain = 1.0f / 7.0f;
		e.lensFlares.minPixels = 22.0f;
		e.lensFlares.maxPixels = 64.0f;
		e.lensFlares.referenceHeight = 1440.0f;
		e.lensFlares.fadeStart = 90.0f;
		e.lensFlares.fadeEnd = 220.0f;
		e.lensFlares.visibilityRate = 14.0f;

		InteriorAtlas& atlas = e.interiorAtlas;
		atlas.faceRoles = { InteriorAtlas::FaceRole::Back, InteriorAtlas::FaceRole::Left, InteriorAtlas::FaceRole::Right,
			InteriorAtlas::FaceRole::Ceiling, InteriorAtlas::FaceRole::Floor };
		for (int room = 0; room < 2; ++room) {
			for (int face = 0; face < 5; ++face) {
				atlas.faceLayers.push_back({ "textures/window_rooms/room" + std::to_string(room) + ".png",
					glm::vec4(0.1f * face, 0.25f, 0.5f + 0.01f * room, 0.75f) });
				atlas.faceMean.push_back(glm::vec3(0.01f * face, 1.0f / 9.0f, 0.333f));
			}
		}
		atlas.textureOptions.size = 512;
		atlas.textureOptions.premultiply = false;
		atlas.textureOptions.debugName = "window rooms";
		atlas.roomLight = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.8f, 0.25f, 0.6f) };
		atlas.styleWeights = { { 0.7f, 0.3f }, { 0.1f, 0.9f / 3.0f }, { 1.0f, 0.0f } };
		atlas.roomShape.sideMargin = glm::vec2(1.0f, 1.4f);
		atlas.roomShape.height = glm::vec2(2.8f, 3.2f);
		atlas.roomShape.depthJitter = 0.1f;
		atlas.roomShape.sillScale = glm::vec2(1.0f, 1.25f);
		atlas.paneUV = { 3.0f, 0.02f, 0.9f };
		atlas.farDistance = 60.0f;
		e.revision = 42;
		return e;
	}

	ProceduralGeneratorInfo TestInfo()
	{
		return ProceduralGeneratorInfo{ 1, { "road_network" }, { "texture_root" }, { "inputs/pack" }, "generated/nfs_test" };
	}

	json TestParams()
	{
		return json{ { "seed", 5 }, { "texture_root", "inputs/pack" }, { "output", "generated/x" } };
	}

	json TestGame()
	{
		return json{ { "road_network", json{ { "nodes", json::array({ json{ { "x", 1.5 } } }) } } }, { "unrelated", 1 } };
	}

	// Result with a fixed identity for the format tests (no files involved).
	CookedResults::Result SyntheticResult()
	{
		CookedResults::Result r;
		r.generator = "testgen";
		r.generatorVersion = 3;
		r.defaultOutput = "generated/nfs_test";
		r.params = json{ { "seed", 9 }, { "texture_root", "textures/cc0" } };
		r.sceneGameKeys = { "road_network" };
		r.sceneGame = json{ { "road_network", json{ { "a", 1 } } } };
		r.inputs = { { "textures/cc0/a.jpg", std::string(64, 'a') } };
		r.model = "model.gltf";
		r.sideFiles = { { "_layout.json", "model_layout.json" } };
		r.childNodes = json::array({ json{ { "type", "light" }, { "name", "lamp" } } });
		r.reflectionProbes = json::array({ json{ { "position", json::array({ 1.0, 2.0, 3.0 }) } } });
		r.hasRenderExtras = true;
		r.renderExtras = SampleExtras();
		r.key = CookedResults::ComputeKey(r.generator, r.generatorVersion, r.params, r.sceneGame, r.inputs);
		r.dir = "generated/cooked/" + r.generator + "-" + r.key;
		return r;
	}

	void InitLooseTree(const fs::path& root)
	{
		fs::remove_all(root);
		fs::create_directories(root / "inputs" / "pack");
		fs::create_directories(root / "generated");
		std::ofstream(root / "inputs" / "pack" / "a.json", std::ios::binary) << "{\"a\":1}";
		std::ofstream(root / "inputs" / "pack" / "b.json", std::ios::binary) << "{\"b\":2}";
		std::ofstream(root / "inputs" / "pack" / "notes.txt", std::ios::binary) << "not packed";
		AssetIO::Config config;
		config.mode = AssetIO::Mode::Loose;
		config.looseRoot = root;
		config.cacheDir = root / "generated";
		config.userDir = root / "user";
		config.projectName = "test";
		std::string error;
		NOX_REQUIRE(AssetIO::Initialize(config, &error));
	}

	void WriteText(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
	}
}

NOX_TEST(CookedResults, Sha256Vectors)
{
	NOX_CHECK_EQ(Sha256::HexOf(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	NOX_CHECK_EQ(Sha256::HexOf("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	NOX_CHECK_EQ(Sha256::HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	const std::string million(1000000, 'a');
	NOX_CHECK_EQ(Sha256::HexOf(million), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	Sha256::Hasher chunked;
	for (size_t i = 0; i < million.size(); i += 777) chunked.Update(std::string_view(million).substr(i, 777));
	NOX_CHECK_EQ(chunked.FinishHex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

NOX_TEST(CookedResults, RenderExtrasRoundTripIsExact)
{
	const SceneRenderExtras original = SampleExtras();
	const std::string text = CookedResults::RenderExtrasToJson(original).dump();
	const json parsed = json::parse(text, nullptr, false);
	NOX_REQUIRE(!parsed.is_discarded());
	SceneRenderExtras restored;
	std::string error;
	NOX_REQUIRE(CookedResults::RenderExtrasFromJson(parsed, restored, &error));
	NOX_CHECK(SameExtras(original, restored));
	NOX_CHECK_EQ(restored.revision, uint64_t(0));   // revision is runtime state
	// Serializing again gives the same text (stable canonical form).
	NOX_CHECK_EQ(CookedResults::RenderExtrasToJson(restored).dump(), text);
}

NOX_TEST(CookedResults, ResultRoundTripKeepsIdentityAndExtras)
{
	const CookedResults::Result original = SyntheticResult();
	const std::string text = CookedResults::ToJson(original).dump(1);
	const json parsed = json::parse(text, nullptr, false);
	NOX_REQUIRE(!parsed.is_discarded());
	CookedResults::Result restored;
	std::string error;
	NOX_REQUIRE(CookedResults::FromJson(parsed, restored, &error));
	NOX_CHECK_EQ(restored.key, original.key);
	NOX_CHECK_EQ(restored.generator, original.generator);
	NOX_CHECK_EQ(restored.generatorVersion, original.generatorVersion);
	NOX_CHECK_EQ(restored.defaultOutput, original.defaultOutput);
	NOX_CHECK(restored.params == original.params);
	NOX_CHECK(restored.childNodes == original.childNodes);
	NOX_CHECK(restored.reflectionProbes == original.reflectionProbes);
	NOX_CHECK_EQ(restored.model, original.model);
	NOX_CHECK_EQ(restored.sideFiles.size(), size_t(1));
	NOX_CHECK(SameExtras(original.renderExtras, restored.renderExtras));
	NOX_CHECK(restored.hasRenderExtras);
	NOX_CHECK_EQ(CookedResults::ToJson(restored).dump(1), text);
}

NOX_TEST(CookedResults, KeyIsStableAndSensitiveToDeclaredInputsOnly)
{
	const ProceduralGeneratorInfo info = TestInfo();
	const json game = TestGame();
	json reordered = json::object();
	reordered["output"] = "generated/other";     // location: not part of the key
	reordered["texture_root"] = "inputs/pack";
	reordered["seed"] = 5;
	const std::string key = CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(TestParams()),
		CookedResults::SceneGameSubset(game, info.sceneGameKeys), {});
	NOX_CHECK_EQ(key.size(), size_t(64));
	NOX_CHECK_EQ(key, CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(reordered),
		CookedResults::SceneGameSubset(game, info.sceneGameKeys), {}));
	NOX_CHECK(key != CookedResults::ComputeKey("testgen", 2, CookedResults::WithoutOutput(TestParams()),
		CookedResults::SceneGameSubset(game, info.sceneGameKeys), {}));
	json otherSeed = TestParams();
	otherSeed["seed"] = 6;
	NOX_CHECK(key != CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(otherSeed),
		CookedResults::SceneGameSubset(game, info.sceneGameKeys), {}));
	// Scene game keys the generator does not declare do not change the key; declared ones do.
	json unrelated = game;
	unrelated["unrelated"] = 2;
	NOX_CHECK_EQ(key, CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(TestParams()),
		CookedResults::SceneGameSubset(unrelated, info.sceneGameKeys), {}));
	json changedRoad = game;
	changedRoad["road_network"]["nodes"] = json::array();
	NOX_CHECK(key != CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(TestParams()),
		CookedResults::SceneGameSubset(changedRoad, info.sceneGameKeys), {}));
	const std::vector<CookedResults::InputHash> inputs = { { "inputs/pack/a.txt", std::string(64, '1') } };
	NOX_CHECK(key != CookedResults::ComputeKey("testgen", 1, CookedResults::WithoutOutput(TestParams()),
		CookedResults::SceneGameSubset(game, info.sceneGameKeys), inputs));
}

NOX_TEST(CookedResults, InvalidResultsAreRejected)
{
	const CookedResults::Result good = SyntheticResult();
	const json base = CookedResults::ToJson(good);
	std::string error;
	CookedResults::Result out;

	json wrongFormat = base;
	wrongFormat["format"] = 2;
	NOX_CHECK(!CookedResults::FromJson(wrongFormat, out, &error));
	NOX_CHECK(error.find("format") != std::string::npos);

	json edited = base;
	edited["params"]["seed"] = 77;   // identity edited without re-keying
	NOX_CHECK(!CookedResults::FromJson(edited, out, &error));
	NOX_CHECK(error.find("key") != std::string::npos);

	json badKey = base;
	badKey["key"] = "not-a-digest";
	NOX_CHECK(!CookedResults::FromJson(badKey, out, &error));

	json missingExtras = base;
	missingExtras.erase("render_extras");
	NOX_CHECK(!CookedResults::FromJson(missingExtras, out, &error));

	json brokenExtras = base;
	brokenExtras["render_extras"]["lens_flares"]["sources"][0]["style"] = "sodium";
	NOX_CHECK(!CookedResults::FromJson(brokenExtras, out, &error));

	json badRect = base;
	badRect["render_extras"]["decal_sets"][0]["layers"][0]["rect"] = json::array({ 0.0, 1.0 });
	NOX_CHECK(!CookedResults::FromJson(badRect, out, &error));

	json unsafeSide = base;
	unsafeSide["side_files"][0]["file"] = "../escape.json";
	NOX_CHECK(!CookedResults::FromJson(unsafeSide, out, &error));

	NOX_CHECK(!CookedResults::FromJson(json::array(), out, &error));
	NOX_CHECK(CookedResults::FromJson(base, out, &error));
}

NOX_TEST(CookedResults, CookedDirectoryFlowOnLooseTree)
{
	const fs::path root = NoxTest::ScratchDir() / "cooked_tree";
	InitLooseTree(root);
	const ProceduralGeneratorInfo info = TestInfo();
	const json game = TestGame();
	std::string error;

	// Identity: the directory and key come from declared inputs (texture_root contents) and the scene game.
	auto identity = CookedResults::Identify("testgen", info, TestParams(), game, &error);
	NOX_REQUIRE(identity.has_value());
	NOX_CHECK(identity->dir.rfind("generated/cooked/testgen-", 0) == 0);
	NOX_CHECK_EQ(identity->inputs.size(), size_t(2));   // notes.txt is not a packaged input
	CookedResults::Result absent;
	NOX_CHECK(CookedResults::LoadDir(identity->dir, absent, &error) == CookedResults::Lookup::Missing);

	// A live-style generator output written under the cooked directory (what nox_cook does).
	const std::string stem = identity->dir + "/model";
	ProceduralGeneratorResult generated;
	generated.ok = true;
	generated.gltfPath = stem + ".gltf";
	generated.auxiliaryPaths = { stem + "_layout.json" };
	generated.childNodes = json::array({ json{ { "type", "light" }, { "name", "lamp" } } });
	generated.reflectionProbes = json::array({ json{ { "position", json::array({ 0.5, 1.0, 2.0 }) } } });
	generated.hasRenderExtras = true;
	generated.renderExtras = SampleExtras();
	WriteText(root / (stem + ".gltf"), "{\"asset\":{\"version\":\"2.0\"}}");
	WriteText(root / (stem + "_layout.json"), "{\"obstacles\":[]}");

	CookedResults::Result result;
	NOX_REQUIRE(CookedResults::FromGenerated(*identity, generated, result, &error));
	NOX_REQUIRE(CookedResults::Write(result, &error));

	CookedResults::Result loaded;
	NOX_CHECK(CookedResults::LoadDir(identity->dir, loaded, &error) == CookedResults::Lookup::Found);
	NOX_CHECK_EQ(loaded.key, identity->key);
	NOX_CHECK(SameExtras(loaded.renderExtras, generated.renderExtras));
	NOX_CHECK(loaded.childNodes == generated.childNodes);
	NOX_CHECK_EQ(CookedResults::ToGenerated(loaded).gltfPath, stem + ".gltf");
	NOX_CHECK_EQ(CookedResults::ToGenerated(loaded).auxiliaryPaths.size(), size_t(1));

	// Unregistered lookup: same generator, params (output ignored) and scene subset; other seed fails.
	CookedResults::Result found;
	NOX_CHECK(CookedResults::FindUnregistered("testgen", TestParams(), game, found, &error));
	NOX_CHECK_EQ(found.key, identity->key);
	json otherSeed = TestParams();
	otherSeed["seed"] = 6;
	NOX_CHECK(!CookedResults::FindUnregistered("testgen", otherSeed, game, found, &error));
	NOX_CHECK(error.find("nox_cook") != std::string::npos);

	// Side file restored at the live location (authored output + suffix), then unchanged on a second call.
	NOX_CHECK(CookedResults::MaterializeSideFiles(loaded, "generated/x", &error));
	NOX_CHECK(fs::exists(root / "generated" / "x_layout.json"));
	NOX_CHECK(CookedResults::MaterializeSideFiles(loaded, "generated/x", &error));

	// Changing a declared input changes the key: the old directory is no longer the cooked result.
	WriteText(root / "inputs" / "pack" / "a.json", "{\"a\":2}");
	auto changed = CookedResults::Identify("testgen", info, TestParams(), game, &error);
	NOX_REQUIRE(changed.has_value());
	NOX_CHECK(changed->key != identity->key);
	NOX_CHECK(CookedResults::LoadDir(changed->dir, loaded, &error) == CookedResults::Lookup::Missing);

	// Corrupt and incomplete results are reported as invalid, not silently used.
	WriteText(root / identity->dir / "result.json", "{\"format\": 1, ");
	NOX_CHECK(CookedResults::LoadDir(identity->dir, loaded, &error) == CookedResults::Lookup::Invalid);
	NOX_CHECK(!error.empty());
	NOX_CHECK(CookedResults::Write(result, &error));
	fs::remove(root / (stem + "_layout.json"));
	NOX_CHECK(CookedResults::LoadDir(identity->dir, loaded, &error) == CookedResults::Lookup::Invalid);
	NOX_CHECK(error.find("side file is missing") != std::string::npos);

	AssetIO::Shutdown();
}
