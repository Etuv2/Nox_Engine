#pragma once
// One logical asset service for every runtime reader (docs/asset_io.md).
//
// Logical paths are relative, '/'-separated and keep their authored spelling, e.g.
// "shaders/includes/pbr_common.glsl" or "models/car_r33_rig.glb". Development mounts the loose
// project tree at the root; shipping mounts explicit read-only packs (engine.pak, game.pak).
// Runtime-generated files live in a writable cache mounted only under "generated/".
//
// Writes never go through this service. Editor authoring writes use SourcePath(); settings,
// saves, logs and snapshots use UserDir(); generator output uses CacheDir().
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AssetIO {

	enum class Mode { Loose, Packed };

	struct Config {
		Mode mode = Mode::Loose;
		// Loose: physical project root mounted at "" (the repository or an unpacked tree).
		std::filesystem::path looseRoot;
		// Packed: archives mounted at "" in this order. Ownership is disjoint (checked at pack time).
		std::vector<std::filesystem::path> packs;
		// Physical writable directory mounted at "generated/" (overrides only that prefix).
		std::filesystem::path cacheDir;
		// Physical writable per-project directory for settings/saves/logs/snapshots.
		std::filesystem::path userDir;
		std::string projectName;
	};

	// Owned whole-file bytes. Consumers that keep pointers into the data (SDL fonts, ImGui atlases,
	// RWops) keep the shared_ptr alive for as long as they use it.
	using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

	struct FileStat {
		bool isDirectory = false;
		uint64_t size = 0;
		int64_t modifiedTime = -1;   // seconds since epoch; -1 when unknown (archive entries)
		bool fromPack = false;
	};

	// Independent cursor per stream; separate streams may be used from different threads.
	// Cost note: stored entries seek directly. In Deflate entries a backward Seek restarts decoding from
	// the beginning of the entry, so random access needs stored entries or ReadAll. Read returns fewer
	// bytes than requested only at end of file or on error (-1; Stream is dead after Shutdown).
	class Stream {
	public:
		virtual ~Stream() = default;
		// Returns bytes read (0 at end), or -1 on error. Short reads are legal.
		virtual int64_t Read(void* dst, uint64_t bytes) = 0;
		virtual bool Seek(uint64_t offset) = 0;
		virtual uint64_t Tell() const = 0;
		virtual uint64_t Size() const = 0;
		virtual const std::string& Path() const = 0;
	};

	struct ListOptions {
		bool recursive = false;
		bool includeDirectories = false;
		std::string extension;   // ".json" filters files by case-insensitive extension; empty = all
	};

	// Lifetime -------------------------------------------------------------------------------------
	bool Initialize(const Config& config, std::string* error = nullptr);
	void Shutdown();
	bool IsInitialized();
	const Config& GetConfig();
	Mode GetMode();

	// Paths ------------------------------------------------------------------------------------------
	// Lexical normalization: '\' -> '/', removes "." and empty segments, resolves ".." inside the root.
	// Returns nullopt (with a reason) for absolute paths, drive letters, URIs or root escapes.
	std::optional<std::string> Normalize(std::string_view path, std::string* error = nullptr);
	// Resolves a reference relative to the directory of baseFile ("models/a/b.gltf" + "tex/c.png").
	std::optional<std::string> ResolveRelative(std::string_view baseFile, std::string_view reference,
		std::string* error = nullptr);
	// Directory part of a logical path ("models/a/b.gltf" -> "models/a"; "x.json" -> "").
	std::string Parent(std::string_view logicalPath);

	// Reads ------------------------------------------------------------------------------------------
	bool Exists(std::string_view path);
	std::optional<FileStat> Stat(std::string_view path);
	Buffer ReadAll(std::string_view path, std::string* error = nullptr);
	std::optional<std::string> ReadText(std::string_view path, std::string* error = nullptr);
	std::unique_ptr<Stream> OpenRead(std::string_view path, std::string* error = nullptr);
	// Sorted logical paths under a directory ("" = root). Entries keep their stored spelling.
	std::vector<std::string> List(std::string_view directory, const ListOptions& options = {});

	// Writable locations (physical) ------------------------------------------------------------------
	// Physical file behind a logical path in the loose source project; nullopt in Packed mode or for
	// invalid paths. Only editor authoring operations may write here.
	std::optional<std::filesystem::path> SourcePath(std::string_view logicalPath);
	const std::filesystem::path& UserDir();
	const std::filesystem::path& CacheDir();
	// Physical path for a logical "generated/..." output inside CacheDir (creates parent folders).
	std::optional<std::filesystem::path> GeneratedOutputPath(std::string_view logicalPath);
	// Additive helper: maps an absolute physical path inside the loose root to its logical path (for
	// authored paths that were absolute before the migration). nullopt in Packed mode or outside the root.
	std::optional<std::string> LogicalFromPhysical(const std::filesystem::path& absolutePhysical);
}
