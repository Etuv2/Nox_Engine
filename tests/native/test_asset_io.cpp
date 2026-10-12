// Headless tests for the asset I/O service (src/assets/AssetIO.*).
// Fixtures: tests/fixtures/assetio (regenerate with make_fixtures.ps1). "tree" is the loose reference
// content; "assetio_fixture.pak" holds the same files (stored entries under stored/, Deflate under
// deflate/ and a few others).
#include "TestFramework.h"

#include "../../src/assets/AssetIO.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

	fs::path FixtureTree() { return NoxTest::RepoRoot() / "tests" / "fixtures" / "assetio" / "tree"; }
	fs::path FixturePak() { return NoxTest::RepoRoot() / "tests" / "fixtures" / "assetio" / "assetio_fixture.pak"; }

	std::vector<uint8_t> ReadRaw(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	void WriteRaw(const fs::path& path, const std::vector<uint8_t>& bytes)
	{
		fs::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	// Every regular file of the reference tree as a logical path.
	std::vector<std::string> TreeFiles()
	{
		std::vector<std::string> out;
		const fs::path root = FixtureTree();
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) continue;
			out.push_back(fs::relative(entry.path(), root).generic_string());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	AssetIO::Config LooseConfig(const fs::path& cache)
	{
		AssetIO::Config config;
		config.mode = AssetIO::Mode::Loose;
		config.looseRoot = FixtureTree();
		config.cacheDir = cache;
		config.userDir = cache.parent_path() / "user";
		config.projectName = "assetio-test";
		return config;
	}

	AssetIO::Config PackedConfig(const fs::path& cache, const fs::path& pak)
	{
		AssetIO::Config config;
		config.mode = AssetIO::Mode::Packed;
		config.packs = { pak };
		config.cacheDir = cache;
		config.userDir = cache.parent_path() / "user";
		config.projectName = "assetio-test";
		return config;
	}

	fs::path Scratch(const char* name)
	{
		const fs::path dir = NoxTest::ScratchDir() / name;
		fs::create_directories(dir);
		return dir;
	}

	std::string Bytes(const AssetIO::Buffer& buffer)
	{
		return buffer ? std::string(buffer->begin(), buffer->end()) : std::string("<null>");
	}

	void ShutdownAfter()
	{
		AssetIO::Shutdown();
	}

} // namespace

// ----------------------------------------------------------------------------------------------------
// Pure path rules (do not need Initialize)
// ----------------------------------------------------------------------------------------------------
NOX_TEST(AssetIO, NormalizeRules)
{
	auto n = [](std::string_view p) { return AssetIO::Normalize(p); };
	NOX_CHECK(n("a\\b/./c") == std::optional<std::string>("a/b/c"));
	NOX_CHECK(n("a/b/../c") == std::optional<std::string>("a/c"));
	NOX_CHECK(n("a//b///c/") == std::optional<std::string>("a/b/c"));
	NOX_CHECK(n("") == std::optional<std::string>(""));
	NOX_CHECK(n("./") == std::optional<std::string>(""));
	NOX_CHECK(n("a/b/../../c") == std::optional<std::string>("c"));
	// Spelling is preserved: no case folding, no percent-decoding.
	NOX_CHECK(n("Models/Car_R33.GLB") == std::optional<std::string>("Models/Car_R33.GLB"));
	NOX_CHECK(n("a/%2e%2e/b") == std::optional<std::string>("a/%2e%2e/b"));
	NOX_CHECK(n("a/..b/.c") == std::optional<std::string>("a/..b/.c"));

	NOX_CHECK(!n("/etc/passwd"));
	NOX_CHECK(!n("\\\\server\\share"));
	NOX_CHECK(!n("C:/x"));
	NOX_CHECK(!n("C:x"));
	NOX_CHECK(!n("file:///x"));
	NOX_CHECK(!n("http://example.com/a"));
	NOX_CHECK(!n("../x"));
	NOX_CHECK(!n("a/../../x"));
	NOX_CHECK(!n("..\\x"));
	NOX_CHECK(!n(std::string_view("a\0b", 3)));
	NOX_CHECK(!n("a\tb"));

	std::string error;
	NOX_CHECK(!AssetIO::Normalize("../secret", &error));
	NOX_CHECK(error.find("escapes") != std::string::npos);
}

NOX_TEST(AssetIO, ResolveRelativeStaysInRoot)
{
	auto r = [](std::string_view base, std::string_view ref) { return AssetIO::ResolveRelative(base, ref); };
	NOX_CHECK(r("models/a/b.gltf", "tex/c.png") == std::optional<std::string>("models/a/tex/c.png"));
	NOX_CHECK(r("models/a/b.gltf", "../tex/c.png") == std::optional<std::string>("models/tex/c.png"));
	NOX_CHECK(r("models/a/b.gltf", "./buffer.bin") == std::optional<std::string>("models/a/buffer.bin"));
	NOX_CHECK(r("x.gltf", "y.bin") == std::optional<std::string>("y.bin"));
	NOX_CHECK(r("models/a.gltf", "sub\\tex.png") == std::optional<std::string>("models/sub/tex.png"));
	// Escapes above the root and absolute or scheme references are rejected.
	NOX_CHECK(!r("x.gltf", "../y.bin"));
	NOX_CHECK(!r("models/a.gltf", "../../../y.bin"));
	NOX_CHECK(!r("models/a.gltf", "/abs.bin"));
	NOX_CHECK(!r("models/a.gltf", "\\abs.bin"));
	NOX_CHECK(!r("models/a.gltf", "C:/abs.bin"));
	NOX_CHECK(!r("models/a.gltf", "data:application/octet-stream;base64,AAAA"));
	NOX_CHECK(!r("models/a.gltf", ""));

	NOX_CHECK(AssetIO::Parent("models/a/b.gltf") == "models/a");
	NOX_CHECK(AssetIO::Parent("x.json") == "");
	NOX_CHECK(AssetIO::Parent("models\\a\\b.gltf") == "models/a");
}

// ----------------------------------------------------------------------------------------------------
// Loose and packed modes
// ----------------------------------------------------------------------------------------------------
NOX_TEST(AssetIO, LooseAndPackedParity)
{
	NOX_REQUIRE(fs::exists(FixturePak()));
	const fs::path cacheLoose = Scratch("parity_cache_loose");
	const fs::path cachePacked = Scratch("parity_cache_packed");

	std::string error;
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cacheLoose), &error));
	const std::vector<std::string> files = TreeFiles();
	NOX_REQUIRE(!files.empty());
	std::vector<std::string> looseBytes;
	std::vector<uint64_t> looseSizes;
	for (const std::string& file : files) {
		AssetIO::Buffer buffer = AssetIO::ReadAll(file, &error);
		NOX_REQUIRE(buffer);
		looseBytes.push_back(Bytes(buffer));
		looseSizes.push_back(buffer->size());
		NOX_CHECK(AssetIO::Exists(file));
		auto st = AssetIO::Stat(file);
		NOX_REQUIRE(st.has_value());
		NOX_CHECK(!st->isDirectory);
		NOX_CHECK(!st->fromPack);
		NOX_CHECK_EQ(st->size, static_cast<uint64_t>(buffer->size()));
		auto text = AssetIO::ReadText(file);
		NOX_CHECK(text.has_value() && text->size() == buffer->size());
	}
	const std::vector<std::string> looseList = AssetIO::List("", { .recursive = true });
	const std::vector<std::string> looseStored = AssetIO::List("stored");
	const std::vector<std::string> looseJson = AssetIO::List("", { .recursive = true, .extension = ".GLTF" });
	AssetIO::Shutdown();

	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cachePacked, FixturePak()), &error));
	for (size_t i = 0; i < files.size(); ++i) {
		AssetIO::Buffer buffer = AssetIO::ReadAll(files[i], &error);
		NOX_REQUIRE(buffer);
		NOX_CHECK(Bytes(buffer) == looseBytes[i]);
		auto st = AssetIO::Stat(files[i]);
		NOX_REQUIRE(st.has_value());
		NOX_CHECK(st->fromPack);
		NOX_CHECK_EQ(st->size, looseSizes[i]);
		NOX_CHECK(st->modifiedTime == -1);
		std::unique_ptr<AssetIO::Stream> stream = AssetIO::OpenRead(files[i], &error);
		NOX_REQUIRE(stream);
		NOX_CHECK_EQ(stream->Size(), looseSizes[i]);
		std::string streamed(looseSizes[i], '\0');
		NOX_CHECK_EQ(stream->Read(streamed.data(), streamed.size()), static_cast<int64_t>(streamed.size()));
		NOX_CHECK(streamed == looseBytes[i]);
	}
	NOX_CHECK(AssetIO::List("", { .recursive = true }) == looseList);
	NOX_CHECK(AssetIO::List("stored") == looseStored);
	NOX_CHECK(AssetIO::List("", { .recursive = true, .extension = ".gltf" }) == looseJson);
	NOX_CHECK(std::is_sorted(looseList.begin(), looseList.end()));
	NOX_CHECK(!looseList.empty());
	NOX_CHECK(std::find(looseList.begin(), looseList.end(), "models/scene.gltf") != looseList.end());
	ShutdownAfter();
}

NOX_TEST(AssetIO, ListOptionsAndDirectories)
{
	const fs::path cache = Scratch("list_cache");
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	auto top = AssetIO::List("");
	NOX_CHECK(std::find(top.begin(), top.end(), "stored") == top.end()); // directories excluded by default
	AssetIO::ListOptions withDirs;
	withDirs.includeDirectories = true;
	auto topWithDirs = AssetIO::List("", withDirs);
	NOX_CHECK(std::find(topWithDirs.begin(), topWithDirs.end(), "stored") != topWithDirs.end());
	auto nested = AssetIO::List("nested", { .recursive = true, .includeDirectories = true });
	NOX_CHECK(std::find(nested.begin(), nested.end(), "nested/a/b/c.txt") != nested.end());
	NOX_CHECK(std::find(nested.begin(), nested.end(), "nested/a") != nested.end());
	NOX_CHECK(AssetIO::List("missing_dir").empty());
	auto st = AssetIO::Stat("nested/a");
	NOX_REQUIRE(st.has_value());
	NOX_CHECK(st->isDirectory);
	NOX_CHECK(AssetIO::Exists("nested/a"));
	ShutdownAfter();
}

// ----------------------------------------------------------------------------------------------------
// Streams
// ----------------------------------------------------------------------------------------------------
NOX_TEST(AssetIO, StreamSeekTellSizeShortReads)
{
	const fs::path cache = Scratch("stream_cache");
	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	const std::vector<uint8_t> expected = ReadRaw(FixtureTree() / "stored" / "blob.bin");
	NOX_REQUIRE(expected.size() == 20000);

	for (const char* name : { "stored/blob.bin", "deflate/blob.bin" }) {
		std::string error;
		auto stream = AssetIO::OpenRead(name, &error);
		NOX_REQUIRE(stream);
		NOX_CHECK_EQ(stream->Size(), 20000u);
		NOX_CHECK_EQ(stream->Tell(), 0u);
		NOX_CHECK(stream->Path() == name);

		std::vector<uint8_t> buf(100);
		NOX_CHECK_EQ(stream->Read(buf.data(), 100), 100);
		NOX_CHECK(std::memcmp(buf.data(), expected.data(), 100) == 0);
		NOX_CHECK_EQ(stream->Tell(), 100u);

		// Forward seek, then a middle read.
		NOX_CHECK(stream->Seek(15000));
		buf.assign(1000, 0);
		NOX_CHECK_EQ(stream->Read(buf.data(), 1000), 1000);
		NOX_CHECK(std::memcmp(buf.data(), expected.data() + 15000, 1000) == 0);

		// Backward seek (re-decodes Deflate entries; must still be exact).
		NOX_CHECK(stream->Seek(3));
		buf.assign(10, 0);
		NOX_CHECK_EQ(stream->Read(buf.data(), 10), 10);
		NOX_CHECK(std::memcmp(buf.data(), expected.data() + 3, 10) == 0);

		// Short read at end of file, then EOF.
		NOX_CHECK(stream->Seek(19990));
		buf.assign(100, 0);
		NOX_CHECK_EQ(stream->Read(buf.data(), 100), 10);
		NOX_CHECK(std::memcmp(buf.data(), expected.data() + 19990, 10) == 0);
		NOX_CHECK_EQ(stream->Tell(), 20000u);
		NOX_CHECK_EQ(stream->Read(buf.data(), 100), 0);

		// Seeking exactly to the end is legal; past the end is not.
		NOX_CHECK(stream->Seek(20000));
		NOX_CHECK_EQ(stream->Read(buf.data(), 1), 0);
		NOX_CHECK(!stream->Seek(20001));
		NOX_CHECK_EQ(stream->Tell(), 20000u);
		NOX_CHECK_EQ(stream->Read(buf.data(), 0), 0);
	}
	ShutdownAfter();
}

NOX_TEST(AssetIO, ConcurrentIndependentStreams)
{
	const fs::path cache = Scratch("concurrent_cache");
	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	const std::vector<uint8_t> expected = ReadRaw(FixtureTree() / "stored" / "blob.bin");

	std::atomic<int> failures{ 0 };
	auto worker = [&](const char* name, unsigned seed) {
		std::string error;
		auto stream = AssetIO::OpenRead(name, &error);
		if (!stream) { ++failures; return; }
		std::vector<uint8_t> buf(777);
		unsigned state = seed;
		for (int iteration = 0; iteration < 400; ++iteration) {
			state = state * 1664525u + 1013904223u;
			const uint64_t offset = state % 19000;
			if (!stream->Seek(offset)) { ++failures; return; }
			const int64_t n = stream->Read(buf.data(), buf.size());
			if (n != 777 || std::memcmp(buf.data(), expected.data() + offset, 777) != 0) { ++failures; return; }
		}
	};
	std::thread a(worker, "stored/blob.bin", 1u);
	std::thread b(worker, "deflate/blob.bin", 2u);
	std::thread c(worker, "stored/blob.bin", 3u);
	a.join();
	b.join();
	c.join();
	NOX_CHECK_EQ(failures.load(), 0);
	ShutdownAfter();
}

NOX_TEST(AssetIO, ShutdownKillsOpenStreamsAndBuffersSurvive)
{
	const fs::path cache = Scratch("lifetime_cache");
	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	const std::vector<uint8_t> expected = ReadRaw(FixtureTree() / "deflate" / "hello.txt");

	AssetIO::Buffer buffer = AssetIO::ReadAll("deflate/hello.txt");
	NOX_REQUIRE(buffer);
	auto stream = AssetIO::OpenRead("stored/blob.bin");
	NOX_REQUIRE(stream);
	std::vector<uint8_t> head(16);
	NOX_CHECK_EQ(stream->Read(head.data(), head.size()), 16);

	AssetIO::Shutdown();
	NOX_CHECK(!AssetIO::IsInitialized());
	// The buffer owns its bytes; it remains valid after Shutdown.
	NOX_CHECK(buffer->size() == expected.size());
	NOX_CHECK(std::memcmp(buffer->data(), expected.data(), expected.size()) == 0);
	// Streams opened before Shutdown fail cleanly instead of touching freed archives.
	NOX_CHECK_EQ(stream->Read(head.data(), head.size()), -1);
	NOX_CHECK(!stream->Seek(0));

	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	AssetIO::Buffer again = AssetIO::ReadAll("deflate/hello.txt");
	NOX_REQUIRE(again);
	NOX_CHECK(Bytes(again) == Bytes(buffer));
	NOX_CHECK(again != buffer);
	ShutdownAfter();
	NOX_CHECK(buffer->size() == expected.size());
}

// ----------------------------------------------------------------------------------------------------
// Failures
// ----------------------------------------------------------------------------------------------------
NOX_TEST(AssetIO, MissingFilesFailCleanly)
{
	const fs::path cache = Scratch("missing_cache");
	std::string error;
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache), &error));
	NOX_CHECK(!AssetIO::Exists("no/such/file.json"));
	NOX_CHECK(!AssetIO::Stat("no/such/file.json"));
	error.clear();
	NOX_CHECK(!AssetIO::ReadAll("no/such/file.json", &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(!AssetIO::ReadText("no/such/file.json"));
	NOX_CHECK(!AssetIO::OpenRead("no/such/file.json"));
	// Directories are not files.
	NOX_CHECK(!AssetIO::ReadAll("nested"));
	NOX_CHECK(!AssetIO::OpenRead("nested"));
	ShutdownAfter();

	// Empty files are valid.
	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	AssetIO::Buffer empty = AssetIO::ReadAll("empty.txt");
	NOX_REQUIRE(empty);
	NOX_CHECK(empty->empty());
	ShutdownAfter();
}

NOX_TEST(AssetIO, EscapesAndAbsolutesRejectedThroughApi)
{
	const fs::path cache = Scratch("escape_cache");
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	std::string error;
	NOX_CHECK(!AssetIO::ReadAll("../CMakeLists.txt", &error));
	NOX_CHECK(error.find("escapes") != std::string::npos);
	NOX_CHECK(!AssetIO::ReadAll("/etc/hosts"));
	NOX_CHECK(!AssetIO::ReadAll("C:/Windows/win.ini"));
	NOX_CHECK(!AssetIO::Exists("../x"));
	NOX_CHECK(!AssetIO::Stat("file:///x"));
	NOX_CHECK(!AssetIO::OpenRead("..\\x"));
	NOX_CHECK(AssetIO::List("../").empty());
	ShutdownAfter();
}

NOX_TEST(AssetIO, CaseSpellingAcrossModes)
{
	const fs::path cache = Scratch("case_cache");
	// Loose mode on Windows is case-insensitive: the lookup succeeds and emits a one-time warning.
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	NOX_CHECK(AssetIO::ReadAll("case/mixed.txt") != nullptr);
	NOX_CHECK(AssetIO::ReadAll("Case/Mixed.txt") != nullptr);
	ShutdownAfter();
	// Packs are case-sensitive: only the stored spelling resolves.
	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	NOX_CHECK(AssetIO::ReadAll("Case/Mixed.txt") != nullptr);
	NOX_CHECK(!AssetIO::ReadAll("case/mixed.txt"));
	NOX_CHECK(!AssetIO::Exists("CASE/MIXED.TXT"));
	ShutdownAfter();
}

NOX_TEST(AssetIO, MissingPackFailsInitialize)
{
	const fs::path cache = Scratch("nopak_cache");
	std::string error;
	NOX_CHECK(!AssetIO::Initialize(PackedConfig(cache, NoxTest::ScratchDir() / "does_not_exist.pak"), &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(!AssetIO::IsInitialized());
}

NOX_TEST(AssetIO, CorruptAndTruncatedPaksNeverCrash)
{
	const fs::path cache = Scratch("corrupt_cache");
	const std::vector<uint8_t> good = ReadRaw(FixturePak());
	NOX_REQUIRE(good.size() > 1000);

	// Not an archive at all.
	const fs::path garbage = NoxTest::ScratchDir() / "garbage.pak";
	WriteRaw(garbage, std::vector<uint8_t>(4096, 0xAA));
	NOX_CHECK(!AssetIO::Initialize(PackedConfig(cache, garbage)));
	NOX_CHECK(!AssetIO::IsInitialized());

	// Truncated: the central directory lives at the end, so mounting should fail; any surviving
	// mount must still answer every lookup without crashing.
	for (size_t keep : { good.size() / 2, good.size() - 30, static_cast<size_t>(200) }) {
		const fs::path truncated = NoxTest::ScratchDir() / ("truncated_" + std::to_string(keep) + ".pak");
		WriteRaw(truncated, std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(keep)));
		const bool ok = AssetIO::Initialize(PackedConfig(cache, truncated));
		if (ok) {
			for (const std::string& file : TreeFiles()) {
				AssetIO::Buffer buffer = AssetIO::ReadAll(file);
				if (buffer) {
					auto st = AssetIO::Stat(file);
					NOX_CHECK(st.has_value() && st->size == buffer->size());
				}
				auto stream = AssetIO::OpenRead(file);
				if (stream) (void)stream->Read(nullptr, 0);
			}
		}
		AssetIO::Shutdown();
	}

	// Corrupted compressed payload: decoding may fail, but must not crash and must not over-report.
	std::vector<uint8_t> flipped = good;
	for (size_t i = 200; i < 600 && i < flipped.size(); ++i) flipped[i] ^= 0x5Au;
	const fs::path flippedPath = NoxTest::ScratchDir() / "flipped.pak";
	WriteRaw(flippedPath, flipped);
	if (AssetIO::Initialize(PackedConfig(cache, flippedPath))) {
		for (const std::string& file : TreeFiles()) {
			AssetIO::Buffer buffer = AssetIO::ReadAll(file);
			if (buffer) NOX_CHECK(buffer->size() <= 20000);
		}
		AssetIO::Shutdown();
	}
}

// ----------------------------------------------------------------------------------------------------
// Generated cache, user and source locations
// ----------------------------------------------------------------------------------------------------
NOX_TEST(AssetIO, GeneratedMountOverridesOnlyGeneratedPrefix)
{
	const fs::path cache = Scratch("gen_cache");
	WriteRaw(cache / "out.txt", { 'g', 'e', 'n', '\n' });
	WriteRaw(cache / "stored" / "hello.txt", { 'x' });

	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	AssetIO::Buffer generated = AssetIO::ReadAll("generated/out.txt");
	NOX_REQUIRE(generated);
	NOX_CHECK(Bytes(generated) == "gen\n");
	// The cache mount does not shadow shipped paths.
	NOX_CHECK(Bytes(AssetIO::ReadAll("stored/hello.txt")) == "hello stored\n");
	ShutdownAfter();

	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	NOX_CHECK(Bytes(AssetIO::ReadAll("generated/out.txt")) == "gen\n");
	NOX_CHECK(Bytes(AssetIO::ReadAll("stored/hello.txt")) == "hello stored\n");
	ShutdownAfter();
}

NOX_TEST(AssetIO, CacheInsideRootIsNotListedTwice)
{
	// Development layout: the cache is <root>/generated itself.
	const fs::path root = Scratch("root_with_generated");
	fs::remove_all(root);
	fs::create_directories(root / "generated");
	WriteRaw(root / "config.json", { '{', '}' });
	WriteRaw(root / "generated" / "g.txt", { 'g' });

	AssetIO::Config config;
	config.mode = AssetIO::Mode::Loose;
	config.looseRoot = root;
	config.cacheDir = root / "generated";
	NOX_REQUIRE(AssetIO::Initialize(config));
	auto listed = AssetIO::List("generated");
	NOX_CHECK_EQ(listed.size(), 1u);
	NOX_CHECK(Bytes(AssetIO::ReadAll("generated/g.txt")) == "g");
	ShutdownAfter();
}

NOX_TEST(AssetIO, GeneratedOutputPathRoundTrip)
{
	const fs::path cache = Scratch("roundtrip_cache");
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	auto physical = AssetIO::GeneratedOutputPath("generated/shuto/district_01/layout.json");
	NOX_REQUIRE(physical.has_value());
	NOX_CHECK(physical->string().find("shuto") != std::string::npos);
	WriteRaw(*physical, { '{', '}' });
	NOX_CHECK(Bytes(AssetIO::ReadAll("generated/shuto/district_01/layout.json")) == "{}");
	// Only generated/ outputs go to the cache.
	NOX_CHECK(!AssetIO::GeneratedOutputPath("stored/hello.txt"));
	NOX_CHECK(!AssetIO::GeneratedOutputPath("generated/../stored/x"));
	ShutdownAfter();
}

NOX_TEST(AssetIO, SourcePathOnlyInLooseMode)
{
	const fs::path cache = Scratch("source_cache");
	NOX_REQUIRE(AssetIO::Initialize(LooseConfig(cache)));
	auto source = AssetIO::SourcePath("nested/a/b/c.txt");
	NOX_REQUIRE(source.has_value());
	NOX_CHECK(fs::equivalent(*source, FixtureTree() / "nested" / "a" / "b" / "c.txt"));
	NOX_CHECK(!AssetIO::SourcePath("../escape"));
	ShutdownAfter();

	NOX_REQUIRE(AssetIO::Initialize(PackedConfig(cache, FixturePak())));
	NOX_CHECK(!AssetIO::SourcePath("nested/a/b/c.txt"));
	NOX_CHECK(AssetIO::Stat("nested/a/b/c.txt")->fromPack);
	ShutdownAfter();
}

NOX_TEST(AssetIO, LazyInitializeAtWorkingDirectory)
{
	ShutdownAfter();
	NOX_CHECK(!AssetIO::IsInitialized());
	// Safety net: a read before Initialize mounts the current directory loosely.
	NOX_CHECK(AssetIO::Exists("CMakeLists.txt"));
	NOX_CHECK(AssetIO::IsInitialized());
	NOX_CHECK(AssetIO::GetMode() == AssetIO::Mode::Loose);
	ShutdownAfter();
}
