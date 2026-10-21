// Shader reload bookkeeping without a GL context: a recipe rebuilds the same program description from disk, including
// the defines injected after #version, and sees edits made to the files in between.
#include "TestFramework.h"

#include "ShaderLoader.h"
#include "assets/AssetIO.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Loose AssetIO rooted at the scratch directory: shader paths are logical paths under it, as in the engine.
struct LooseScratchAssets {
    LooseScratchAssets() {
        AssetIO::Config config;
        config.mode = AssetIO::Mode::Loose;
        config.looseRoot = NoxTest::ScratchDir();
        config.cacheDir = NoxTest::ScratchDir() / "cache";
        config.userDir = NoxTest::ScratchDir() / "user";
        config.projectName = "shader-hot-reload-test";
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

} // namespace

NOX_TEST(ShaderHotReload, RecipeInjectsDefinesAfterVersionAndSeesEdits) {
    LooseScratchAssets assets;
    NOX_REQUIRE(assets.ok);
    const std::filesystem::path dir = NoxTest::ScratchDir() / "shader_recipe";
    const std::string vertPath = "shader_recipe/t_vert.glsl";   // logical paths under the loose root
    const std::string fragPath = "shader_recipe/t_frag.glsl";
    WriteText(dir / "t_vert.glsl", "#version 460 core\nvoid main() { gl_Position = vec4(0.0); }\n");
    WriteText(dir / "t_frag.glsl", "#version 460 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }\n");

    ShaderRecipe recipe;
    recipe.name = vertPath + " + " + fragPath;
    recipe.stages.push_back({ rhi::ShaderStage::Vertex, vertPath, false, {} });
    recipe.stages.push_back({ rhi::ShaderStage::Fragment, fragPath, true, { "#define FOO 1" } });

    rhi::ProgramDesc desc;
    NOX_REQUIRE(BuildShaderRecipeDesc(recipe, desc));
    NOX_CHECK_EQ(desc.stages.size(), static_cast<size_t>(2));
    NOX_CHECK(desc.debugName == recipe.name);
    const std::string& frag = desc.stages[1].source;
    const size_t version = frag.find("#version");
    const size_t define = frag.find("#define FOO 1");
    const size_t body = frag.find("out vec4 color");
    NOX_REQUIRE(version != std::string::npos && define != std::string::npos && body != std::string::npos);
    NOX_CHECK(version < define && define < body);

    // An edit on disk shows up in the next build (what a reload reads).
    WriteText(dir / "t_frag.glsl", "#version 460 core\nout vec4 color;\nvoid main() { color = vec4(0.5); }\n");
    rhi::ProgramDesc edited;
    NOX_REQUIRE(BuildShaderRecipeDesc(recipe, edited));
    NOX_CHECK(edited.stages[1].source.find("vec4(0.5)") != std::string::npos);
    NOX_CHECK(edited.stages[1].source.find("vec4(1.0)") == std::string::npos);

    // A missing stage makes the build fail (the editor then keeps the previous program).
    std::filesystem::remove(dir / "t_frag.glsl");
    rhi::ProgramDesc missing;
    NOX_CHECK(!BuildShaderRecipeDesc(recipe, missing));
}

NOX_TEST(ShaderHotReload, PlainRecipeKeepsSourcesUnchanged) {
    LooseScratchAssets assets;
    NOX_REQUIRE(assets.ok);
    const std::filesystem::path dir = NoxTest::ScratchDir() / "shader_plain";
    const std::string compPath = "shader_plain/t_comp.glsl";
    WriteText(dir / "t_comp.glsl", "#version 460 core\nlayout(local_size_x = 8) in;\nvoid main() {}\n");

    ShaderRecipe recipe;
    recipe.name = compPath;
    recipe.stages.push_back({ rhi::ShaderStage::Compute, compPath, false, {} });
    rhi::ProgramDesc desc;
    NOX_REQUIRE(BuildShaderRecipeDesc(recipe, desc));
    NOX_REQUIRE(desc.stages.size() == 1);
    NOX_CHECK(desc.stages[0].stage == rhi::ShaderStage::Compute);
    NOX_CHECK(desc.stages[0].source.find("local_size_x = 8") != std::string::npos);
}
