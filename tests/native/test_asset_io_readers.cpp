// Reader-level tests for the AssetIO adapters: SDL RWops over owned buffers, shader #include
// resolution, and TinyGLTF external buffers resolved relative to the logical model path.
#include "TestFramework.h"

#include "../../src/assets/AssetIO.h"
#include "../../src/assets/AssetIOSdl.h"
#include "../../src/assets/AssetIOGltf.h"
#include "../../src/ShaderLoader.h"
#include "tiny_gltf.h"

#include <cstring>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
	void WriteText(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}

	fs::path ReaderRoot(const char* name)
	{
		const fs::path root = NoxTest::ScratchDir() / name;
		fs::remove_all(root);
		fs::create_directories(root);
		return root;
	}

	void InitLoose(const fs::path& root)
	{
		AssetIO::Config config;
		config.mode = AssetIO::Mode::Loose;
		config.looseRoot = root;
		config.cacheDir = root / "generated";
		config.userDir = root / "user";
		NOX_REQUIRE(AssetIO::Initialize(config));
	}
}

NOX_TEST(AssetIOReaders, SdlRWopsOverOwnedBuffer)
{
	const fs::path root = ReaderRoot("rwops_root");
	WriteText(root / "fonts" / "digits.txt", "0123456789");
	InitLoose(root);

	SDL_RWops* ops = AssetIO::OpenRWops("fonts/digits.txt");
	NOX_REQUIRE(ops != nullptr);
	NOX_CHECK_EQ(ops->size(ops), 10);
	char buf[4] = {};
	NOX_CHECK_EQ(ops->read(ops, buf, 1, 4), static_cast<size_t>(4));
	NOX_CHECK(std::memcmp(buf, "0123", 4) == 0);
	NOX_CHECK_EQ(ops->seek(ops, -2, RW_SEEK_END), static_cast<Sint64>(8));
	NOX_CHECK_EQ(ops->read(ops, buf, 1, 4), static_cast<size_t>(2)); // short read at end
	NOX_CHECK(std::memcmp(buf, "89", 2) == 0);
	NOX_CHECK_EQ(ops->read(ops, buf, 1, 4), static_cast<size_t>(0));
	NOX_CHECK_EQ(ops->seek(ops, 11, RW_SEEK_SET), static_cast<Sint64>(-1)); // past the end
	NOX_CHECK_EQ(ops->close(ops), 0);

	// Missing files give nullptr; the buffer stays owned by the RWops until closed.
	NOX_CHECK(AssetIO::OpenRWops("fonts/missing.ttf") == nullptr);
	AssetIO::Shutdown();
}

NOX_TEST(AssetIOReaders, ShaderIncludesResolveFromIncludingFile)
{
	const fs::path root = ReaderRoot("shader_root");
	WriteText(root / "shaders" / "main.frag", "#version 450\n#include \"lib/a.glsl\"\nvoid main() {}\n");
	WriteText(root / "shaders" / "lib" / "a.glsl", "float a();\n#include \"b.glsl\"\n");
	WriteText(root / "shaders" / "lib" / "b.glsl", "float b_value = 1.0;\n#include \"a.glsl\"\n");
	InitLoose(root);

	std::string source;
	NOX_REQUIRE(LoadShaderSourceWithIncludes("shaders/main.frag", source));
	NOX_CHECK(source.find("float a();") != std::string::npos);
	NOX_CHECK(source.find("float b_value = 1.0;") != std::string::npos); // relative to lib/, not shaders/
	NOX_CHECK(source.find("void main() {}") != std::string::npos);

	// Missing includes fail; an include that escapes the asset root fails.
	WriteText(root / "shaders" / "bad.frag", "#include \"missing.glsl\"\n");
	std::string bad;
	NOX_CHECK(!LoadShaderSourceWithIncludes("shaders/bad.frag", bad));
	WriteText(root / "shaders" / "escape.frag", "#include \"../../outside.glsl\"\n");
	std::string escape;
	NOX_CHECK(!LoadShaderSourceWithIncludes("shaders/escape.frag", escape));
	AssetIO::Shutdown();
}

NOX_TEST(AssetIOReaders, GltfExternalBufferResolvesRelativeToModel)
{
	const fs::path root = ReaderRoot("gltf_root");
	WriteText(root / "models" / "car.gltf",
		"{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"car.bin\",\"byteLength\":4}]}");
	WriteText(root / "models" / "sub" / "wheel.gltf",
		"{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"../car.bin\",\"byteLength\":4}]}");
	const char payload[4] = { 1, 2, 3, 4 };
	WriteText(root / "models" / "car.bin", std::string(payload, 4));
	WriteText(root / "models" / "sub" / "dummy.txt", "x");
	InitLoose(root);

	for (const char* model : { "models/car.gltf", "models/sub/wheel.gltf" }) {
		tinygltf::TinyGLTF loader;
		std::string err, warn;
		NOX_REQUIRE(loader.SetFsCallbacks(AssetIO::MakeGltfFsCallbacks(), &err));
		tinygltf::Model gltf;
		const bool ok = loader.LoadASCIIFromFile(&gltf, &err, &warn, model);
		NOX_REQUIRE(ok);
		NOX_REQUIRE(gltf.buffers.size() == 1);
		NOX_CHECK(gltf.buffers[0].data.size() == 4);
		NOX_CHECK(gltf.buffers[0].data.size() == 4 && gltf.buffers[0].data[3] == 4);
	}
	AssetIO::Shutdown();
}
