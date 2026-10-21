// Model hot reload bookkeeping without a GL context: the files a model depends on, the debounce before a changed file is
// decoded, the generation counter, and which nodes use a model. The GPU half (retargeting, freeing) is checked live.
#include "TestFramework.h"

#include "ModelHotReload.h"
#include "Scene.h"
#include "SceneNode.h"
#include "assets/AssetIO.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

// Loose AssetIO rooted at the scratch directory, as in the engine's loose projects.
struct LooseScratchAssets {
    LooseScratchAssets() {
        AssetIO::Config config;
        config.mode = AssetIO::Mode::Loose;
        config.looseRoot = NoxTest::ScratchDir();
        config.cacheDir = NoxTest::ScratchDir() / "cache";
        config.userDir = NoxTest::ScratchDir() / "user";
        config.projectName = "model-hot-reload-test";
        ok = AssetIO::Initialize(config);
    }
    ~LooseScratchAssets() { AssetIO::Shutdown(); }
    bool ok = false;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

ModelFileStamp Stamp(const std::string& path, bool exists, uint64_t size, int64_t mtime) {
    ModelFileEntry entry;
    entry.path = path;
    entry.exists = exists;
    entry.size = size;
    entry.mtime = mtime;
    return { entry };
}

} // namespace

NOX_TEST(ModelHotReload, GltfDependenciesAreItsExternalBuffersAndImagesNextToIt) {
    LooseScratchAssets assets;
    NOX_REQUIRE(assets.ok);
    const std::filesystem::path dir = NoxTest::ScratchDir() / "model_deps";
    WriteText(dir / "car.gltf",
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":4,\"uri\":\"car_data.bin\"},{\"byteLength\":4,\"uri\":\"data:application/octet-stream;base64,AAAA\"}],"
        "\"images\":[{\"uri\":\"tex/paint.png\"}]}");
    const std::vector<std::string> paths = ModelDependencyPaths("model_deps/car.gltf");
    NOX_REQUIRE(paths.size() == 3);
    NOX_CHECK(paths[0] == "model_deps/car.gltf");
    NOX_CHECK(paths[1] == "model_deps/car_data.bin");
    NOX_CHECK(paths[2] == "model_deps/tex/paint.png");

    // A binary model lists only itself.
    const std::vector<std::string> glb = ModelDependencyPaths("model_deps/car.glb");
    NOX_CHECK(glb.size() == 1 && glb[0] == "model_deps/car.glb");
}

NOX_TEST(ModelHotReload, StampReportsMissingAndPresentFiles) {
    LooseScratchAssets assets;
    NOX_REQUIRE(assets.ok);
    WriteText(NoxTest::ScratchDir() / "model_stamp" / "present.glb", "glTF12345");
    const ModelFileStamp stamp = StatModelFiles({ "model_stamp/present.glb", "model_stamp/absent.glb" });
    NOX_REQUIRE(stamp.size() == 2);
    NOX_CHECK(stamp[0].exists && stamp[0].size == 9);
    NOX_CHECK(!stamp[1].exists);
    NOX_CHECK(stamp[0] != stamp[1]);
}

NOX_TEST(ModelHotReload, GenerationAdvancesOnlyOnSuccessfulReloads) {
    ModelReloadWatch watch;
    const ModelFileStamp v1 = Stamp("m/car.glb", true, 100, 10);
    const ModelFileStamp v2 = Stamp("m/car.glb", true, 120, 20);
    watch.Track("m/car.glb", v1);
    NOX_CHECK(watch.IsTracked("m/car.glb"));
    NOX_CHECK_EQ(watch.Generation("m/car.glb"), static_cast<uint64_t>(0));
    NOX_CHECK_EQ(watch.Generation("m/untracked.glb"), static_cast<uint64_t>(0));

    NOX_CHECK(!watch.Poll("m/car.glb", v1, 0.0));       // unchanged: nothing to do
    NOX_CHECK(!watch.Changed("m/car.glb", v1));

    watch.RecordFailure("m/car.glb", v2);                // a failed decode keeps generation 0
    NOX_CHECK_EQ(watch.Generation("m/car.glb"), static_cast<uint64_t>(0));
    NOX_CHECK(!watch.Changed("m/car.glb", v2));          // the same bytes are not retried

    const ModelFileStamp v3 = Stamp("m/car.glb", true, 130, 30);
    NOX_CHECK(watch.Changed("m/car.glb", v3));
    watch.RecordSuccess("m/car.glb", v3);
    NOX_CHECK_EQ(watch.Generation("m/car.glb"), static_cast<uint64_t>(1));
    NOX_CHECK(!watch.Changed("m/car.glb", v3));

    watch.Untrack("m/car.glb");
    NOX_CHECK(!watch.IsTracked("m/car.glb"));
    NOX_CHECK_EQ(watch.Generation("m/car.glb"), static_cast<uint64_t>(0));
}

NOX_TEST(ModelHotReload, DebounceWaitsUntilTheStampIsStable) {
    ModelReloadWatch watch;
    const ModelFileStamp v1 = Stamp("m/car.glb", true, 100, 10);
    const ModelFileStamp writing = Stamp("m/car.glb", true, 40, 11);    // half written
    const ModelFileStamp done = Stamp("m/car.glb", true, 100, 12);
    watch.Track("m/car.glb", v1);

    NOX_CHECK(!watch.Poll("m/car.glb", writing, 1.0));    // first sight of the change starts the clock
    NOX_CHECK(!watch.Poll("m/car.glb", writing, 1.2));    // 0.2 s stable: not yet
    NOX_CHECK(!watch.Poll("m/car.glb", done, 1.25));      // still changing: the clock restarts
    NOX_CHECK(!watch.Poll("m/car.glb", done, 1.5));       // 0.25 s stable: not yet
    NOX_CHECK(watch.Poll("m/car.glb", done, 1.6));      // 0.35 s stable
    watch.RecordSuccess("m/car.glb", done);
    NOX_CHECK_EQ(watch.Generation("m/car.glb"), static_cast<uint64_t>(1));
}

NOX_TEST(ModelHotReload, MissingModelFileIsNeverDecodedMidWrite) {
    ModelReloadWatch watch;
    const ModelFileStamp v1 = Stamp("m/car.glb", true, 100, 10);
    watch.Track("m/car.glb", v1);
    const ModelFileStamp gone = Stamp("m/car.glb", false, 0, -1);
    NOX_CHECK(!watch.Poll("m/car.glb", gone, 0.0));
    NOX_CHECK(!watch.Poll("m/car.glb", gone, 5.0));
    NOX_CHECK(!watch.Changed("m/car.glb", gone));
    // It reappears: the debounce starts from the first sighting of the new stamp.
    const ModelFileStamp back = Stamp("m/car.glb", true, 100, 20);
    NOX_CHECK(!watch.Poll("m/car.glb", back, 6.0));
    NOX_CHECK(watch.Poll("m/car.glb", back, 6.4));
}

NOX_TEST(ModelHotReload, CollectsEveryNodeUsingTheModelDepthFirst) {
    auto model = std::make_shared<Scene>();
    auto other = std::make_shared<Scene>();
    auto root = std::make_shared<SceneNode>();
    auto a = std::make_shared<SceneNode>();
    auto b = std::make_shared<SceneNode>();
    auto c = std::make_shared<SceneNode>();
    auto d = std::make_shared<SceneNode>();
    a->SetModel(model);
    c->SetModel(model);
    d->SetModel(other);
    root->AddChild(a);
    root->AddChild(b);
    b->AddChild(c);
    root->AddChild(d);

    const std::vector<std::shared_ptr<SceneNode>> users = CollectNodesUsingModel(root, model.get());
    NOX_REQUIRE(users.size() == 2);
    NOX_CHECK(users[0] == a);
    NOX_CHECK(users[1] == c);
    NOX_CHECK(CollectNodesUsingModel(root, nullptr).empty());
    NOX_CHECK(CollectNodesUsingModel(nullptr, model.get()).empty());
}
