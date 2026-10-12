// Asset I/O service backed by PhysicsFS (static, ZIP archives only). See AssetIO.h for the contract.
//
// Lock order: g_stateMutex (shared for operations, exclusive for Initialize/Shutdown) -> g_streamsMutex
// -> per-stream mutex. Stream reads take only their own mutex, so independent streams never contend.
#include "AssetIO.h"

#include "physfs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

namespace AssetIO {
namespace {

	// ---------------------------------------------------------------------------------------------
	// Shared state
	// ---------------------------------------------------------------------------------------------
	struct State {
		bool initialized = false;
		Config config;
		std::vector<std::string> mountedPacks;   // UTF-8 names exactly as mounted
		std::string looseRootUtf8;
		std::filesystem::path emptyPath;
	};

	std::shared_mutex g_stateMutex;
	State g_state;

	std::mutex g_spellingMutex;                       // loose-mode case diagnostics
	std::map<std::string, std::set<std::string>> g_dirEntries;   // dir -> exact entry names
	std::set<std::string> g_spellingWarned;

	std::atomic<bool> g_lazyLogged{ false };

	std::string ToUtf8(const std::filesystem::path& p)
	{
		const std::u8string u = p.u8string();
		return std::string(reinterpret_cast<const char*>(u.data()), u.size());
	}

	std::string LastPhysFsError()
	{
		const char* text = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
		return text ? text : "unknown PhysicsFS error";
	}

	void SetError(std::string* error, std::string message)
	{
		if (error) *error = std::move(message);
	}

	// ---------------------------------------------------------------------------------------------
	// Streams: registered so Shutdown can close them before PHYSFS_deinit.
	// ---------------------------------------------------------------------------------------------
	class PhysFsStream;
	std::mutex g_streamsMutex;
	std::unordered_set<PhysFsStream*> g_streams;

	class PhysFsStream final : public Stream {
	public:
		PhysFsStream(PHYSFS_File* file, std::string path, uint64_t size) :
			m_file(file), m_path(std::move(path)), m_size(size)
		{
			std::lock_guard<std::mutex> lock(g_streamsMutex);
			g_streams.insert(this);
		}

		~PhysFsStream() override
		{
			{
				std::lock_guard<std::mutex> lock(g_streamsMutex);
				g_streams.erase(this);
			}
			std::lock_guard<std::mutex> lock(m_mutex);
			CloseLocked();
		}

		int64_t Read(void* dst, uint64_t bytes) override
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_file) return -1;
			if (bytes == 0 || m_pos >= m_size) return 0;
			uint64_t wanted = std::min<uint64_t>(bytes, m_size - m_pos);
			uint8_t* out = static_cast<uint8_t*>(dst);
			uint64_t done = 0;
			// PhysicsFS may return fewer bytes than requested; loop until full or end of file.
			while (done < wanted) {
				const PHYSFS_sint64 n = PHYSFS_readBytes(m_file, out + done, wanted - done);
				if (n < 0) {
					m_pos += done;
					return done > 0 ? static_cast<int64_t>(done) : -1;
				}
				if (n == 0) break;
				done += static_cast<uint64_t>(n);
			}
			m_pos += done;
			return static_cast<int64_t>(done);
		}

		bool Seek(uint64_t offset) override
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_file || offset > m_size) return false;
			// Backward seeks inside Deflate entries make PhysicsFS restart decoding (see AssetIO.h).
			if (!PHYSFS_seek(m_file, offset)) return false;
			m_pos = offset;
			return true;
		}

		uint64_t Tell() const override
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_pos;
		}

		uint64_t Size() const override { return m_size; }
		const std::string& Path() const override { return m_path; }

		// Called by Shutdown with g_streamsMutex held. The stream stays valid but reads fail.
		void Kill()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			CloseLocked();
		}

	private:
		void CloseLocked()
		{
			if (m_file) {
				PHYSFS_close(m_file);
				m_file = nullptr;
			}
		}

		mutable std::mutex m_mutex;
		PHYSFS_File* m_file = nullptr;
		std::string m_path;
		uint64_t m_size = 0;
		uint64_t m_pos = 0;
	};

	// ---------------------------------------------------------------------------------------------
	// Lexical normalization
	// ---------------------------------------------------------------------------------------------
	bool SplitAndResolve(std::string_view path, std::vector<std::string>& parts, std::string* error)
	{
		if (path.find('\0') != std::string_view::npos) {
			SetError(error, "path contains a NUL character");
			return false;
		}
		for (unsigned char ch : path) {
			if (ch < 0x20) {
				SetError(error, "path contains a control character");
				return false;
			}
		}
		if (path.find(':') != std::string_view::npos) {
			SetError(error, "path contains ':' (drive letters and URIs are not allowed): " + std::string(path));
			return false;
		}
		if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
			SetError(error, "absolute paths are not allowed: " + std::string(path));
			return false;
		}
		std::string current;
		auto flush = [&](bool& ok) {
			if (current.empty() || current == ".") {
				current.clear();
				return;
			}
			if (current == "..") {
				if (parts.empty()) {
					SetError(error, "path escapes the asset root: " + std::string(path));
					ok = false;
				} else {
					parts.pop_back();
				}
			} else {
				parts.push_back(current);
			}
			current.clear();
		};
		bool ok = true;
		for (char ch : path) {
			if (ch == '/' || ch == '\\') {
				flush(ok);
				if (!ok) return false;
			} else {
				current.push_back(ch);
			}
		}
		flush(ok);
		return ok;
	}

	std::optional<std::string> NormalizeLocked(std::string_view path, std::string* error)
	{
		std::vector<std::string> parts;
		if (!SplitAndResolve(path, parts, error)) return std::nullopt;
		std::string out;
		for (const std::string& part : parts) {
			if (!out.empty()) out.push_back('/');
			out += part;
		}
		return out;
	}

	// ---------------------------------------------------------------------------------------------
	// State access
	// ---------------------------------------------------------------------------------------------
	void DeinitLocked()
	{
		{
			std::lock_guard<std::mutex> lock(g_streamsMutex);
			for (PhysFsStream* stream : g_streams) stream->Kill();
		}
		{
			std::lock_guard<std::mutex> lock(g_spellingMutex);
			g_dirEntries.clear();
		}
		if (g_state.initialized) PHYSFS_deinit();
		g_state.initialized = false;
		g_state.mountedPacks.clear();
		g_state.looseRootUtf8.clear();
	}

	bool InitializeLocked(const Config& config, std::string* error)
	{
		if (config.mode == Mode::Loose) {
			std::error_code ec;
			if (config.looseRoot.empty() || !std::filesystem::is_directory(config.looseRoot, ec)) {
				SetError(error, "loose asset root is not a directory: " + ToUtf8(config.looseRoot));
				return false;
			}
		} else {
			for (const auto& pack : config.packs) {
				std::error_code ec;
				if (!std::filesystem::is_regular_file(pack, ec)) {
					SetError(error, "pack not found: " + ToUtf8(pack));
					return false;
				}
			}
		}

		if (!PHYSFS_init(nullptr)) {
			SetError(error, "PHYSFS_init failed: " + LastPhysFsError());
			return false;
		}

		std::vector<std::string> mounted;
		auto fail = [&](std::string message) {
			SetError(error, std::move(message));
			if (PHYSFS_isInit()) PHYSFS_deinit();
			return false;
		};

		if (config.mode == Mode::Loose) {
			const std::string root = ToUtf8(config.looseRoot);
			if (!PHYSFS_mount(root.c_str(), "", 1)) return fail("cannot mount " + root + ": " + LastPhysFsError());
			mounted.push_back(root);
			g_state.looseRootUtf8 = root;
		} else {
			for (const auto& pack : config.packs) {
				const std::string name = ToUtf8(pack);
				if (!PHYSFS_mount(name.c_str(), "", 1)) return fail("cannot mount pack " + name + ": " + LastPhysFsError());
				mounted.push_back(name);
			}
		}

		if (!config.cacheDir.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(config.cacheDir, ec);
			if (ec) return fail("cannot create cache directory " + ToUtf8(config.cacheDir) + ": " + ec.message());
			bool alreadyVisible = false;
			if (config.mode == Mode::Loose) {
				// Development: cacheDir may be <root>/generated, which is already visible under "generated/".
				std::error_code ecA, ecB;
				const auto a = std::filesystem::weakly_canonical(config.cacheDir, ecA);
				const auto b = std::filesystem::weakly_canonical(config.looseRoot / "generated", ecB);
				alreadyVisible = !ecA && !ecB && a == b;
			}
			if (!alreadyVisible) {
				const std::string cache = ToUtf8(config.cacheDir);
				// Prepended, so "generated/..." lookups see cache output first; only that prefix is affected.
				if (!PHYSFS_mount(cache.c_str(), "generated", 0)) return fail("cannot mount cache " + cache + ": " + LastPhysFsError());
				mounted.push_back(cache);
			}
		}

		g_state.config = config;
		g_state.mountedPacks = std::move(mounted);
		g_state.initialized = true;
		{
			std::lock_guard<std::mutex> lock(g_spellingMutex);
			g_dirEntries.clear();
			g_spellingWarned.clear();
		}
		return true;
	}

	// Ensures a state exists for operations called before Initialize (development safety net).
	void LazyInitIfNeeded()
	{
		{
			std::shared_lock<std::shared_mutex> lock(g_stateMutex);
			if (g_state.initialized) return;
		}
		std::unique_lock<std::shared_mutex> lock(g_stateMutex);
		if (g_state.initialized) return;
		Config config;
		config.mode = Mode::Loose;
		config.looseRoot = std::filesystem::current_path();
		config.cacheDir = config.looseRoot / "generated";
		std::string error;
		if (!InitializeLocked(config, &error)) {
			std::fprintf(stderr, "[AssetIO] lazy initialization failed: %s\n", error.c_str());
			return;
		}
		if (!g_lazyLogged.exchange(true)) {
			std::fprintf(stderr, "[AssetIO] used before Initialize; mounted %s in loose mode\n",
				ToUtf8(config.looseRoot).c_str());
		}
	}

	// Loose-mode spelling diagnostics: the physical file system is case-insensitive on Windows, so a
	// path can work loose and fail in a case-sensitive pack. Warn once per requested path.
	void CheckSpellingLoose(const std::string& normalized)
	{
		if (g_state.config.mode != Mode::Loose) return;
		{
			std::lock_guard<std::mutex> lock(g_spellingMutex);
			if (g_spellingWarned.count(normalized)) return;
		}
		if (normalized.empty()) return;
		std::string dir;
		size_t start = 0;
		bool exact = true;
		while (start <= normalized.size() && exact) {
			const size_t slash = normalized.find('/', start);
			const std::string part = normalized.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
			{
				std::lock_guard<std::mutex> lock(g_spellingMutex);
				auto it = g_dirEntries.find(dir);
				if (it == g_dirEntries.end()) {
					char** list = PHYSFS_enumerateFiles(dir.c_str());
					std::set<std::string> fresh;
					if (list) {
						for (char** p = list; *p; ++p) fresh.insert(*p);
						PHYSFS_freeList(list);
					}
					it = g_dirEntries.emplace(dir, std::move(fresh)).first;
				}
				if (!it->second.count(part)) exact = false;
			}
			if (slash == std::string::npos) break;
			dir = dir.empty() ? part : dir + "/" + part;
			start = slash + 1;
		}
		if (!exact) {
			std::lock_guard<std::mutex> lock(g_spellingMutex);
			if (g_spellingWarned.insert(normalized).second) {
				std::fprintf(stderr, "[AssetIO] warning: '%s' differs in case from the on-disk spelling "
					"(packs are case-sensitive; fix the reference)\n", normalized.c_str());
			}
		}
	}

	// True when realDir (from PHYSFS_getRealDir) is one of the packs mounted in packed mode.
	bool IsPackFile(const std::string& realDir)
	{
		if (g_state.config.mode != Mode::Packed) return false;
		return std::find(g_state.mountedPacks.begin(), g_state.mountedPacks.end(), realDir) != g_state.mountedPacks.end();
	}

	std::string Join(const std::string& dir, const std::string& name)
	{
		return dir.empty() ? name : dir + "/" + name;
	}

	std::string Lower(std::string_view s)
	{
		std::string out(s);
		for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return out;
	}

	// Opens a normalized path. Caller holds the state lock.
	PHYSFS_File* OpenNormalized(const std::string& normalized, std::string* error, uint64_t* size)
	{
		PHYSFS_Stat st{};
		if (!PHYSFS_stat(normalized.c_str(), &st)) {
			SetError(error, "not found: " + normalized + " (" + LastPhysFsError() + ")");
			return nullptr;
		}
		if (st.filetype == PHYSFS_FILETYPE_DIRECTORY) {
			SetError(error, "is a directory: " + normalized);
			return nullptr;
		}
		PHYSFS_File* file = PHYSFS_openRead(normalized.c_str());
		if (!file) {
			SetError(error, "cannot open " + normalized + ": " + LastPhysFsError());
			return nullptr;
		}
		PHYSFS_sint64 length = PHYSFS_fileLength(file);
		if (length < 0) length = st.filesize;
		if (length < 0) {
			PHYSFS_close(file);
			SetError(error, "unknown file size: " + normalized);
			return nullptr;
		}
		*size = static_cast<uint64_t>(length);
		CheckSpellingLoose(normalized);
		return file;
	}

	// Holds the shared state lock for the duration of one operation.
	class Op {
	public:
		Op() : m_lock(g_stateMutex) {}
		explicit operator bool() const { return g_state.initialized; }
	private:
		std::shared_lock<std::shared_mutex> m_lock;
	};

	// Acquire the shared lock, lazily initializing first when needed.
	Op Begin()
	{
		LazyInitIfNeeded();
		return Op();
	}

} // namespace

	// -------------------------------------------------------------------------------------------------
	// Lifetime
	// -------------------------------------------------------------------------------------------------
	bool Initialize(const Config& config, std::string* error)
	{
		std::unique_lock<std::shared_mutex> lock(g_stateMutex);
		DeinitLocked();
		return InitializeLocked(config, error);
	}

	void Shutdown()
	{
		std::unique_lock<std::shared_mutex> lock(g_stateMutex);
		DeinitLocked();
	}

	bool IsInitialized()
	{
		std::shared_lock<std::shared_mutex> lock(g_stateMutex);
		return g_state.initialized;
	}

	const Config& GetConfig()
	{
		return g_state.config;
	}

	Mode GetMode()
	{
		return g_state.config.mode;
	}

	// -------------------------------------------------------------------------------------------------
	// Paths
	// -------------------------------------------------------------------------------------------------
	std::optional<std::string> Normalize(std::string_view path, std::string* error)
	{
		return NormalizeLocked(path, error);
	}

	std::optional<std::string> ResolveRelative(std::string_view baseFile, std::string_view reference, std::string* error)
	{
		std::string base;
		if (auto normalizedBase = NormalizeLocked(baseFile, error)) base = *normalizedBase;
		else return std::nullopt;
		if (reference.empty()) {
			SetError(error, "empty reference");
			return std::nullopt;
		}
		if (reference.front() == '/' || reference.front() == '\\') {
			SetError(error, "absolute references are not allowed: " + std::string(reference));
			return std::nullopt;
		}
		// References resolve against the directory of the normalized base file.
		const size_t slash = base.rfind('/');
		const std::string dir = slash == std::string::npos ? std::string() : base.substr(0, slash);
		const std::string joined = dir.empty() ? std::string(reference) : dir + "/" + std::string(reference);
		return NormalizeLocked(joined, error);
	}

	std::string Parent(std::string_view logicalPath)
	{
		auto normalized = NormalizeLocked(logicalPath, nullptr);
		if (!normalized) return std::string();
		const size_t slash = normalized->rfind('/');
		return slash == std::string::npos ? std::string() : normalized->substr(0, slash);
	}

	// -------------------------------------------------------------------------------------------------
	// Reads
	// -------------------------------------------------------------------------------------------------
	bool Exists(std::string_view path)
	{
		auto guard = Begin();
		if (!guard) return false;
		auto normalized = NormalizeLocked(path, nullptr);
		if (!normalized) return false;
		if (normalized->empty()) return true;
		return PHYSFS_exists(normalized->c_str()) != 0;
	}

	std::optional<FileStat> Stat(std::string_view path)
	{
		auto guard = Begin();
		if (!guard) return std::nullopt;
		auto normalized = NormalizeLocked(path, nullptr);
		if (!normalized) return std::nullopt;
		FileStat out;
		if (normalized->empty()) {
			out.isDirectory = true;
			return out;
		}
		PHYSFS_Stat st{};
		if (!PHYSFS_stat(normalized->c_str(), &st)) return std::nullopt;
		out.isDirectory = st.filetype == PHYSFS_FILETYPE_DIRECTORY;
		out.size = out.isDirectory || st.filesize < 0 ? 0 : static_cast<uint64_t>(st.filesize);
		const char* real = PHYSFS_getRealDir(normalized->c_str());
		out.fromPack = real && IsPackFile(real);
		out.modifiedTime = out.fromPack ? -1 : static_cast<int64_t>(st.modtime);
		return out;
	}

	Buffer ReadAll(std::string_view path, std::string* error)
	{
		auto guard = Begin();
		if (!guard) {
			SetError(error, "AssetIO is not initialized");
			return nullptr;
		}
		auto normalized = NormalizeLocked(path, error);
		if (!normalized) return nullptr;
		uint64_t size = 0;
		PHYSFS_File* file = OpenNormalized(*normalized, error, &size);
		if (!file) return nullptr;
		auto bytes = std::make_shared<std::vector<uint8_t>>();
		bytes->reserve(static_cast<size_t>(size));
		std::vector<uint8_t> chunk(64 * 1024);
		while (bytes->size() < size) {
			const uint64_t want = std::min<uint64_t>(chunk.size(), size - bytes->size());
			const PHYSFS_sint64 n = PHYSFS_readBytes(file, chunk.data(), want);
			if (n < 0) {
				SetError(error, "read failed: " + *normalized + " (" + LastPhysFsError() + ")");
				PHYSFS_close(file);
				return nullptr;
			}
			if (n == 0) break;
			bytes->insert(bytes->end(), chunk.begin(), chunk.begin() + n);
		}
		PHYSFS_close(file);
		if (bytes->size() != size) {
			SetError(error, "truncated entry: " + *normalized);
			return nullptr;
		}
		return bytes;
	}

	std::optional<std::string> ReadText(std::string_view path, std::string* error)
	{
		Buffer buffer = ReadAll(path, error);
		if (!buffer) return std::nullopt;
		return std::string(buffer->begin(), buffer->end());
	}

	std::unique_ptr<Stream> OpenRead(std::string_view path, std::string* error)
	{
		auto guard = Begin();
		if (!guard) {
			SetError(error, "AssetIO is not initialized");
			return nullptr;
		}
		auto normalized = NormalizeLocked(path, error);
		if (!normalized) return nullptr;
		uint64_t size = 0;
		PHYSFS_File* file = OpenNormalized(*normalized, error, &size);
		if (!file) return nullptr;
		return std::make_unique<PhysFsStream>(file, *normalized, size);
	}

	std::vector<std::string> List(std::string_view directory, const ListOptions& options)
	{
		std::vector<std::string> out;
		auto guard = Begin();
		if (!guard) return out;
		auto normalized = NormalizeLocked(directory, nullptr);
		if (!normalized) return out;

		const std::string ext = Lower(options.extension);
		std::set<std::string> results;
		std::vector<std::string> pending{ *normalized };
		std::set<std::string> visited;
		while (!pending.empty()) {
			const std::string dir = pending.back();
			pending.pop_back();
			if (!visited.insert(dir).second) continue;
			char** list = PHYSFS_enumerateFiles(dir.c_str());
			if (!list) continue;
			for (char** p = list; *p; ++p) {
				const std::string name = *p;
				if (name.empty() || name == "." || name == "..") continue;
				const std::string full = Join(dir, name);
				PHYSFS_Stat st{};
				if (!PHYSFS_stat(full.c_str(), &st)) continue;
				if (st.filetype == PHYSFS_FILETYPE_DIRECTORY) {
					if (options.includeDirectories) results.insert(full);
					if (options.recursive) pending.push_back(full);
					continue;
				}
				if (st.filetype != PHYSFS_FILETYPE_REGULAR) continue;
				if (!ext.empty()) {
					const std::string lowerName = Lower(name);
					if (lowerName.size() < ext.size() || lowerName.compare(lowerName.size() - ext.size(), ext.size(), ext) != 0) continue;
				}
				results.insert(full);
			}
			PHYSFS_freeList(list);
		}
		out.assign(results.begin(), results.end());
		return out;
	}

	// -------------------------------------------------------------------------------------------------
	// Writable locations
	// -------------------------------------------------------------------------------------------------
	std::optional<std::filesystem::path> SourcePath(std::string_view logicalPath)
	{
		auto guard = Begin();
		if (!guard || g_state.config.mode != Mode::Loose) return std::nullopt;
		auto normalized = NormalizeLocked(logicalPath, nullptr);
		if (!normalized || normalized->empty()) return std::nullopt;
		std::filesystem::path out = g_state.config.looseRoot;
		size_t start = 0;
		while (start <= normalized->size()) {
			const size_t slash = normalized->find('/', start);
			out /= normalized->substr(start, slash == std::string::npos ? std::string::npos : slash - start);
			if (slash == std::string::npos) break;
			start = slash + 1;
		}
		return out;
	}

	const std::filesystem::path& UserDir()
	{
		return g_state.initialized ? g_state.config.userDir : g_state.emptyPath;
	}

	const std::filesystem::path& CacheDir()
	{
		return g_state.initialized ? g_state.config.cacheDir : g_state.emptyPath;
	}

	std::optional<std::filesystem::path> GeneratedOutputPath(std::string_view logicalPath)
	{
		auto guard = Begin();
		if (!guard || g_state.config.cacheDir.empty()) return std::nullopt;
		auto normalized = NormalizeLocked(logicalPath, nullptr);
		if (!normalized || normalized->compare(0, 10, "generated/") != 0 || normalized->size() <= 10) return std::nullopt;
		std::filesystem::path out = g_state.config.cacheDir;
		size_t start = 10;
		while (start <= normalized->size()) {
			const size_t slash = normalized->find('/', start);
			out /= normalized->substr(start, slash == std::string::npos ? std::string::npos : slash - start);
			if (slash == std::string::npos) break;
			start = slash + 1;
		}
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
		if (ec) return std::nullopt;
		return out;
	}

	std::optional<std::string> LogicalFromPhysical(const std::filesystem::path& absolutePhysical)
	{
		auto guard = Begin();
		if (!guard || g_state.config.mode != Mode::Loose || !absolutePhysical.is_absolute()) return std::nullopt;
		std::error_code ecRoot, ecPath;
		const auto root = std::filesystem::weakly_canonical(g_state.config.looseRoot, ecRoot);
		const auto path = std::filesystem::weakly_canonical(absolutePhysical, ecPath);
		if (ecRoot || ecPath) return std::nullopt;
		const auto rel = path.lexically_relative(root);
		if (rel.empty()) return std::nullopt;
		for (const auto& part : rel) {
			if (part == "..") return std::nullopt;
		}
		return NormalizeLocked(ToUtf8(rel), nullptr);
	}

} // namespace AssetIO
