#define NOMINMAX
#include "ContentBrowser.h"

#include "InspectorWidgets.h"
#include "../EditorExtensions.h"
#include "../assets/AssetIO.h"
#include "../assets/AssetIOImage.h"
#include "../editor/ExternalEditor.h"
#include "../editor/ProcessRunner.h"
#include "../editor/solace/SolaceUI.h"
#include "../json.hpp"
#include "../scripting/LuaEditorSupport.h"
#include "../stb_image.h"

#include <Windows.h>
#include <shellapi.h>
#include <IMGUI/imgui.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <iostream>
#include <set>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace nox::content {

namespace {
	constexpr float kTileMin = 64.0f;
	constexpr float kTileMax = 256.0f;
	constexpr int kThumbDim = 192;                  // grid thumbnails (longest side, pixels)
	constexpr int kPreviewDim = 512;                // details preview
	constexpr size_t kCacheCapacity = 256;          // thumbnails kept on the GPU
	constexpr size_t kUploadsPerFrame = 4;          // texture uploads on the UI thread per frame
	constexpr int kThumbWorkers = 2;
	constexpr uint64_t kMaxThumbFileBytes = 64ull << 20;
	constexpr size_t kMaxIndexEntries = 60000;
	constexpr size_t kMaxSearchResults = 2000;
	constexpr size_t kMaxPickerPaths = 4000;
	constexpr auto kScanInterval = std::chrono::milliseconds(2000);
	constexpr size_t kMaxBackHistory = 200;

	struct TypeFilter { const char* label; bool any; AssetKind kind; };
	const TypeFilter kTypeFilters[] = {
		{ "All types", true, AssetKind::Other }, { "Scene", false, AssetKind::Scene }, { "Prefab", false, AssetKind::Prefab },
		{ "Model", false, AssetKind::Model }, { "Texture", false, AssetKind::Texture }, { "Material", false, AssetKind::Material },
		{ "Script", false, AssetKind::Script }, { "Audio", false, AssetKind::Audio }, { "Shader", false, AssetKind::Shader },
		{ "Font", false, AssetKind::Font },
	};

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	bool EndsWith(const std::string& text, const std::string& suffix)
	{
		return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool ContainsNoCase(const std::string& text, const char* filter)
	{
		if (!filter || !filter[0]) return true;
		return Lower(text).find(Lower(filter)) != std::string::npos;
	}

	std::string Basename(const std::string& path)
	{
		const size_t slash = path.rfind('/');
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	// Logical paths are UTF-8 with '/' separators; the physical walk converts from the native path.
	std::string Utf8(const std::filesystem::path& p)
	{
		const std::u8string u = p.generic_u8string();
		return std::string(reinterpret_cast<const char*>(u.data()), u.size());
	}

	std::string Join(const std::string& dir, const std::string& name)
	{
		return dir.empty() ? name : dir + "/" + name;
	}

	// Lower-case extension; the compound extensions of the authored JSON formats are kept whole.
	std::string Extension(const std::string& name)
	{
		const std::string lower = Lower(name);
		if (EndsWith(lower, ".prefab.json")) return ".prefab.json";
		if (EndsWith(lower, ".material.json")) return ".material.json";
		const size_t dot = lower.rfind('.');
		if (dot == std::string::npos || dot == 0) return std::string();
		return lower.substr(dot);
	}

	// "car.glb" -> "car" + ".glb"; "a.prefab.json" -> "a" + ".prefab.json".
	void SplitName(const std::string& name, std::string& stem, std::string& ext)
	{
		ext = Extension(name);
		stem = ext.empty() ? name : name.substr(0, name.size() - ext.size());
	}

	std::string FirstSegment(const std::string& path)
	{
		const size_t slash = path.find('/');
		return slash == std::string::npos ? path : path.substr(0, slash);
	}

	// Folders that hold build products, generated output or third-party code, not project assets. Pruned at any
	// depth by name; at the project root only, the names below are pruned too (a nested "include" can be an asset).
	bool PrunedDirectory(const std::string& parent, const std::string& name)
	{
		if (name.empty() || name[0] == '.') return true;
		static const std::set<std::string> kSkip = { "build", "external", "output", "captures", "snapshots", "node_modules" };
		static const std::set<std::string> kRootSkip = {
			"validation_output", "logs", "package", "x64", "include", "lib", "release", "debug", "cmake", "generated",
			"evaluationcaptures", "performance_data", "nox_engine",
		};
		const std::string lower = Lower(name);
		if (kSkip.count(lower)) return true;
		return parent.empty() && kRootSkip.count(lower) > 0;
	}

	// Physical file behind a logical path: loose projects only. Packed assets are read-only and have no file.
	std::optional<fs::path> PhysicalPath(const std::string& logical)
	{
		return AssetIO::SourcePath(logical);
	}

	bool ValidName(const std::string& name)
	{
		if (name.empty() || name == "." || name == "..") return false;
		return name.find_first_of("/\\:*?\"<>|\n\r") == std::string::npos;
	}

	std::string FormatSize(uint64_t bytes)
	{
		char buf[32];
		if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
		else if (bytes < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
		else std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		return buf;
	}

	std::string FormatTime(int64_t seconds)
	{
		if (seconds < 0) return "-";
		const std::time_t t = static_cast<std::time_t>(seconds);
		std::tm tmv{};
		if (localtime_s(&tmv, &t) != 0) return "-";
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
		return buf;
	}

	std::string TruncateText(const std::string& text, float maxWidth)
	{
		if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
		std::string out = text;
		while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth) out.pop_back();
		return out + "...";
	}

	ImU32 KindColor(AssetKind kind)
	{
		switch (kind) {
		case AssetKind::Folder: return IM_COL32(230, 190, 90, 255);
		case AssetKind::Scene: return IM_COL32(110, 170, 250, 255);
		case AssetKind::Prefab: return IM_COL32(120, 220, 150, 255);
		case AssetKind::Model: return IM_COL32(200, 150, 255, 255);
		case AssetKind::Texture: return IM_COL32(255, 140, 120, 255);
		case AssetKind::Material: return IM_COL32(255, 200, 110, 255);
		case AssetKind::Script: return IM_COL32(120, 235, 225, 255);
		case AssetKind::Audio: return IM_COL32(255, 225, 120, 255);
		case AssetKind::Shader: return IM_COL32(170, 170, 255, 255);
		case AssetKind::Font: return IM_COL32(200, 200, 200, 255);
		default: return IM_COL32(150, 150, 150, 255);
		}
	}

	solace::Icon KindIcon(AssetKind kind)
	{
		switch (kind) {
		case AssetKind::Folder: return solace::Icon::Folder;
		case AssetKind::Scene: return solace::Icon::Map;
		case AssetKind::Prefab: return solace::Icon::Package;
		case AssetKind::Model: return solace::Icon::Layers;
		case AssetKind::Texture: return solace::Icon::Sun;
		case AssetKind::Material: return solace::Icon::Sparkles;
		case AssetKind::Script: return solace::Icon::Command;
		case AssetKind::Audio: return solace::Icon::Bell;
		case AssetKind::Shader: return solace::Icon::Settings;
		case AssetKind::Font: return solace::Icon::Info;
		default: return solace::Icon::Copy;
		}
	}

	// Short badge text for a file type (the extension, shortened for the compound formats).
	std::string BadgeText(const std::string& ext)
	{
		if (ext == ".prefab.json") return "PREFAB";
		if (ext == ".material.json") return "MATERIAL";
		std::string s = ext.empty() ? std::string() : Lower(ext.substr(1));
		for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if (s.size() > 6) s.resize(6);
		return s;
	}

	// Wide path list for the shell: double-null terminated, as SHFileOperationW requires.
	std::wstring ShellPathList(const fs::path& p)
	{
		std::wstring list = p.wstring();
		list.push_back(L'\0');
		list.push_back(L'\0');
		return list;
	}

	bool OpenInShell(const fs::path& p)
	{
		const HINSTANCE r = ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return reinterpret_cast<intptr_t>(r) > 32;
	}

	bool MoveToRecycleBin(const fs::path& p)
	{
		const std::wstring from = ShellPathList(p);
		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = from.c_str();
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
		return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
	}

	// FNV-1a over bytes: the index change signature (ScanLoop).
	uint64_t Fnv(uint64_t h, const void* data, size_t n)
	{
		const unsigned char* p = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
		return h;
	}

	// Decodes an image into RGBA8 (HDR is tone-mapped), box-filtered down to fit maxDim. Runs on a thumbnail worker.
	bool DecodeRgba(const std::string& path, int maxDim, int& origW, int& origH, int& w, int& h, std::vector<uint8_t>& out)
	{
		int x = 0, y = 0, ch = 0;
		std::vector<uint8_t> rgba;
		if (EndsWith(Lower(path), ".hdr")) {
			float* f = AssetIO::LoadImageFileF(path, &x, &y, &ch, 4);
			if (!f) return false;
			rgba.resize(static_cast<size_t>(x) * static_cast<size_t>(y) * 4);
			for (size_t i = 0; i < rgba.size(); ++i) {
				float v = f[i];
				if ((i & 3) != 3) v = std::pow(std::max(0.0f, v / (1.0f + std::max(0.0f, v))), 1.0f / 2.2f);
				rgba[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			stbi_image_free(f);
		}
		else {
			unsigned char* p = AssetIO::LoadImageFile(path, &x, &y, &ch, 4);
			if (!p) return false;
			rgba.assign(p, p + static_cast<size_t>(x) * static_cast<size_t>(y) * 4);
			stbi_image_free(p);
		}
		origW = x;
		origH = y;
		const int longest = std::max(x, y);
		if (longest <= maxDim) {
			w = x;
			h = y;
			out = std::move(rgba);
			return true;
		}
		const float scale = static_cast<float>(maxDim) / static_cast<float>(longest);
		w = std::max(1, static_cast<int>(x * scale + 0.5f));
		h = std::max(1, static_cast<int>(y * scale + 0.5f));
		out.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
		for (int dy = 0; dy < h; ++dy) {
			const int sy0 = dy * y / h;
			const int sy1 = std::max(sy0 + 1, (dy + 1) * y / h);
			for (int dx = 0; dx < w; ++dx) {
				const int sx0 = dx * x / w;
				const int sx1 = std::max(sx0 + 1, (dx + 1) * x / w);
				uint32_t sum[4] = { 0, 0, 0, 0 };
				uint32_t count = 0;
				for (int sy = sy0; sy < sy1; ++sy) {
					for (int sx = sx0; sx < sx1; ++sx) {
						const uint8_t* s = &rgba[(static_cast<size_t>(sy) * x + sx) * 4];
						for (int c = 0; c < 4; ++c) sum[c] += s[c];
						++count;
					}
				}
				uint8_t* d = &out[(static_cast<size_t>(dy) * w + dx) * 4];
				for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / std::max(1u, count));
			}
		}
		return true;
	}
} // namespace

AssetKind ClassifyAsset(const std::string& path, bool isDirectory)
{
	if (isDirectory) return AssetKind::Folder;
	const std::string ext = Extension(Basename(path));
	if (ext == ".prefab.json") return AssetKind::Prefab;
	if (ext == ".material.json") return AssetKind::Material;
	if (ext == ".json") return FirstSegment(path) == "scenes" ? AssetKind::Scene : AssetKind::Other;
	if (ext == ".glb" || ext == ".gltf" || ext == ".fbx") return AssetKind::Model;
	if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".hdr" || ext == ".bmp") return AssetKind::Texture;
	if (ext == ".lua") return AssetKind::Script;
	if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return AssetKind::Audio;
	if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".shader") return AssetKind::Shader;
	if (ext == ".ttf" || ext == ".otf" || ext == ".fon") return AssetKind::Font;
	return AssetKind::Other;
}

const char* AssetKindLabel(AssetKind kind)
{
	switch (kind) {
	case AssetKind::Folder: return "Folder";
	case AssetKind::Scene: return "Scene";
	case AssetKind::Prefab: return "Prefab";
	case AssetKind::Model: return "Model";
	case AssetKind::Texture: return "Texture";
	case AssetKind::Material: return "Material";
	case AssetKind::Script: return "Script";
	case AssetKind::Audio: return "Audio";
	case AssetKind::Shader: return "Shader";
	case AssetKind::Font: return "Font";
	default: return "File";
	}
}

ContentBrowser::ContentBrowser(Host host)
	: m_host(std::move(host))
	, m_index(std::make_shared<const Index>())
{
	m_scanThread = std::thread([this] { ScanLoop(); });
	for (int i = 0; i < kThumbWorkers; ++i) m_thumbWorkers.emplace_back([this] { ThumbLoop(); });
}

ContentBrowser::~ContentBrowser()
{
	m_stop = true;
	{ std::lock_guard<std::mutex> lock(m_scanMutex); }
	m_scanCv.notify_all();
	{ std::lock_guard<std::mutex> lock(m_thumbMutex); }
	m_thumbCv.notify_all();
	if (m_scanThread.joinable()) m_scanThread.join();
	for (std::thread& t : m_thumbWorkers) if (t.joinable()) t.join();
	// The GPU outlives the browser (EditorWorkspace destroys it after its own resources).
	for (auto& [key, thumb] : m_thumbs) {
		if (thumb.tex) rhi::Gpu().DestroyTexture(thumb.tex);
	}
	for (auto& [frame, tex] : m_retired) {
		if (tex) rhi::Gpu().DestroyTexture(tex);
	}
}

// ---------------------------------------------------------------------------------------------
// Index: scan thread
// ---------------------------------------------------------------------------------------------
std::shared_ptr<const ContentBrowser::Index> ContentBrowser::CurrentIndex() const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	return m_index;
}

void ContentBrowser::ScanLoop()
{
	uint64_t lastSignature = 0;
	bool first = true;
	while (!m_stop.load()) {
		const auto started = std::chrono::steady_clock::now();
		auto idx = std::make_shared<Index>();
		if (AssetIO::IsInitialized()) {
			const AssetIO::Config& config = AssetIO::GetConfig();
			if (AssetIO::GetMode() == AssetIO::Mode::Loose && !config.looseRoot.empty()) WalkLoose(config.looseRoot, idx->entries);
			else WalkLogical(idx->entries);
		}
		for (size_t i = 0; i < idx->entries.size(); ++i) {
			const Entry& e = idx->entries[i];
			idx->byPath[e.path] = i;
			idx->children[e.parent].push_back(i);
		}
		for (auto& [dir, list] : idx->children) {
			std::sort(list.begin(), list.end(), [&](size_t a, size_t b) {
				const Entry& ea = idx->entries[a];
				const Entry& eb = idx->entries[b];
				if (ea.isDir != eb.isDir) return ea.isDir;
				return Lower(ea.name) < Lower(eb.name);
			});
		}
		uint64_t sig = 1469598103934665603ull;
		for (const Entry& e : idx->entries) {
			sig = Fnv(sig, e.path.data(), e.path.size());
			sig = Fnv(sig, &e.size, sizeof(e.size));
			sig = Fnv(sig, &e.mtime, sizeof(e.mtime));
		}
		idx->signature = sig;

		if (!m_stop.load() && (first || sig != lastSignature)) {
			// One line per published index (the first scan and every change), not per poll.
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			std::cout << "[ContentBrowser] scan: " << idx->entries.size() << " entries in " << ms << " ms" << std::endl;
			first = false;
			lastSignature = sig;
			std::lock_guard<std::mutex> lock(m_indexMutex);
			idx->generation = m_index->generation + 1;
			m_index = std::move(idx);
		}

		std::unique_lock<std::mutex> lock(m_scanMutex);
		m_scanCv.wait_for(lock, kScanInterval, [this] { return m_stop.load() || m_rescan; });
		m_rescan = false;
	}
}

// Loose project: walks the physical tree with the directory iterator. Each directory entry carries its type, size
// and write time, so no file is opened and no image is decoded. Pruned folders are not descended into.
void ContentBrowser::WalkLoose(const fs::path& root, std::vector<Entry>& out)
{
	std::error_code ec;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	if (ec) return;
	const fs::recursive_directory_iterator end;
	while (it != end && !m_stop.load() && out.size() < kMaxIndexEntries) {
		const fs::directory_entry& de = *it;
		const fs::path rel = de.path().lexically_relative(root);
		Entry e;
		e.path = Utf8(rel);
		e.name = Utf8(de.path().filename());
		e.parent = Utf8(rel.parent_path());
		e.isDir = de.is_directory(ec);
		ec.clear();
		if (e.isDir && PrunedDirectory(e.parent, e.name)) {
			it.disable_recursion_pending();
		}
		else {
			e.ext = e.isDir ? std::string() : Extension(e.name);
			e.kind = ClassifyAsset(e.path, e.isDir);
			if (!e.isDir) {
				const std::uintmax_t size = de.file_size(ec);
				if (!ec) e.size = size;
				ec.clear();
			}
			const fs::file_time_type written = de.last_write_time(ec);
			if (!ec) {
				const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(written);
				e.mtime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
			}
			ec.clear();
			out.push_back(std::move(e));
		}
		it.increment(ec);
		if (ec) break;
	}
}

// Packed project: the logical tree through AssetIO.
void ContentBrowser::WalkLogical(std::vector<Entry>& out)
{
	AssetIO::ListOptions options;
	options.includeDirectories = true;
	std::vector<std::string> pending{ std::string() };
	while (!pending.empty() && !m_stop.load() && out.size() < kMaxIndexEntries) {
		const std::string dir = pending.back();
		pending.pop_back();
		for (const std::string& child : AssetIO::List(dir, options)) {
			const auto st = AssetIO::Stat(child);
			if (!st) continue;
			Entry e;
			e.path = child;
			e.name = Basename(child);
			e.parent = dir;
			e.isDir = st->isDirectory;
			if (e.isDir && PrunedDirectory(e.parent, e.name)) continue;
			e.ext = e.isDir ? std::string() : Extension(e.name);
			e.kind = ClassifyAsset(child, e.isDir);
			e.size = st->size;
			e.mtime = st->modifiedTime;
			if (e.isDir) pending.push_back(child);
			out.push_back(std::move(e));
			if (out.size() >= kMaxIndexEntries) break;
		}
	}
}

void ContentBrowser::RequestRescan()
{
	{
		std::lock_guard<std::mutex> lock(m_scanMutex);
		m_rescan = true;
	}
	m_scanCv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Thumbnails: workers decode, the UI thread uploads
// ---------------------------------------------------------------------------------------------
void ContentBrowser::ThumbLoop()
{
	for (;;) {
		ThumbJob job;
		{
			std::unique_lock<std::mutex> lock(m_thumbMutex);
			m_thumbCv.wait(lock, [this] { return m_stop.load() || !m_thumbJobs.empty(); });
			if (m_stop.load()) return;
			job = std::move(m_thumbJobs.front());
			m_thumbJobs.pop_front();
		}
		ThumbResult result;
		result.key = job.key;
		result.mtime = job.mtime;
		const auto st = AssetIO::Stat(job.path);
		if (st && !st->isDirectory && st->size <= kMaxThumbFileBytes) {
			result.ok = DecodeRgba(job.path, job.maxDim, result.origW, result.origH, result.w, result.h, result.pixels);
		}
		std::lock_guard<std::mutex> lock(m_thumbMutex);
		m_thumbDone.push_back(std::move(result));
	}
}

void ContentBrowser::Retire(rhi::TextureHandle tex)
{
	if (tex) m_retired.emplace_back(m_frame, tex);
}

// Returns the cached thumbnail, or nullptr after queuing a decode. Only called for tiles that are on screen.
const ContentBrowser::CachedThumb* ContentBrowser::Thumb(const Entry& e, int maxDim)
{
	if (e.isDir || e.kind != AssetKind::Texture) return nullptr;
	const std::string key = e.path + "|" + std::to_string(maxDim);
	auto it = m_thumbs.find(key);
	if (it != m_thumbs.end()) {
		if (it->second.mtime == e.mtime) {
			it->second.lastUsed = m_frame;
			return &it->second;
		}
		// The file changed on disk: drop the old decode and decode again.
		Retire(it->second.tex);
		m_thumbs.erase(it);
	}
	if (m_thumbPending.count(key)) return nullptr;
	if (e.size > kMaxThumbFileBytes) {
		CachedThumb failed;
		failed.failed = true;
		failed.mtime = e.mtime;
		failed.lastUsed = m_frame;
		return &(m_thumbs[key] = failed);
	}
	m_thumbPending.insert(key);
	{
		std::lock_guard<std::mutex> lock(m_thumbMutex);
		m_thumbJobs.push_back({ key, e.path, maxDim, e.mtime });
	}
	m_thumbCv.notify_one();
	return nullptr;
}

void ContentBrowser::DrainThumbs()
{
	// A texture is destroyed two frames after it was retired, so no draw list still refers to it.
	for (auto it = m_retired.begin(); it != m_retired.end();) {
		if (m_frame >= it->first + 2) {
			rhi::TextureHandle tex = it->second;
			rhi::Gpu().DestroyTexture(tex);
			it = m_retired.erase(it);
		}
		else {
			++it;
		}
	}

	std::vector<ThumbResult> batch;
	{
		std::lock_guard<std::mutex> lock(m_thumbMutex);
		while (!m_thumbDone.empty() && batch.size() < kUploadsPerFrame) {
			batch.push_back(std::move(m_thumbDone.front()));
			m_thumbDone.pop_front();
		}
	}
	for (ThumbResult& r : batch) {
		m_thumbPending.erase(r.key);
		CachedThumb c;
		c.mtime = r.mtime;
		c.origW = r.origW;
		c.origH = r.origH;
		c.lastUsed = m_frame;
		if (r.ok && !r.pixels.empty()) {
			rhi::TextureDesc desc;
			desc.width = r.w;
			desc.height = r.h;
			desc.debugName = "content-browser-thumbnail";
			c.tex = rhi::Gpu().CreateTexture(desc);
			// One mip level: the default GL minification filter would leave the texture incomplete (sampled as black).
			rhi::Gpu().SetTextureFilter(c.tex, rhi::Filter::Linear, rhi::Filter::Linear);
			rhi::Gpu().SetTextureWrapAxis(c.tex, 0, rhi::Wrap::ClampToEdge);
			rhi::Gpu().SetTextureWrapAxis(c.tex, 1, rhi::Wrap::ClampToEdge);
			rhi::Gpu().UpdateTexture(c.tex, rhi::TextureRegion{}, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, r.pixels.data());
			c.w = r.w;
			c.h = r.h;
		}
		else {
			c.failed = true;
		}
		if (auto old = m_thumbs.find(r.key); old != m_thumbs.end()) Retire(old->second.tex);
		m_thumbs[r.key] = c;
	}

	// Least recently used first; never evict a thumbnail drawn in the previous frame.
	while (m_thumbs.size() > kCacheCapacity) {
		auto victim = m_thumbs.end();
		for (auto it = m_thumbs.begin(); it != m_thumbs.end(); ++it) {
			if (it->second.lastUsed >= m_frame) continue;
			if (victim == m_thumbs.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
		}
		if (victim == m_thumbs.end()) break;
		Retire(victim->second.tex);
		m_thumbs.erase(victim);
	}
}

// ---------------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------------
void ContentBrowser::Navigate(const std::string& folder, bool record)
{
	if (folder == m_folder) return;
	if (record) {
		m_back.push_back(m_folder);
		if (m_back.size() > kMaxBackHistory) m_back.erase(m_back.begin());
		m_forward.clear();
	}
	m_folder = folder;
	m_selected.clear();
}

void ContentBrowser::GoBack()
{
	if (m_back.empty()) return;
	m_forward.push_back(m_folder);
	m_folder = m_back.back();
	m_back.pop_back();
	m_selected.clear();
}

void ContentBrowser::GoForward()
{
	if (m_forward.empty()) return;
	m_back.push_back(m_folder);
	m_folder = m_forward.back();
	m_forward.pop_back();
	m_selected.clear();
}

void ContentBrowser::GoUp()
{
	if (m_folder.empty()) return;
	Navigate(AssetIO::Parent(m_folder), true);
}

bool ContentBrowser::WaitForScan(std::chrono::milliseconds timeout)
{
	const auto until = std::chrono::steady_clock::now() + timeout;
	while (CurrentIndex()->generation == 0) {
		if (std::chrono::steady_clock::now() >= until) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return true;
}

bool ContentBrowser::SmokeNavigate(const std::string& folder)
{
	const auto idx = CurrentIndex();
	const auto it = idx->byPath.find(folder);
	if (it == idx->byPath.end() || !idx->entries[it->second].isDir) return false;
	Navigate(folder, false);
	return true;
}

const std::vector<std::string>& ContentBrowser::AssetPaths()
{
	const auto idx = CurrentIndex();
	if (m_pathsGeneration != idx->generation) {
		m_pathsGeneration = idx->generation;
		m_paths.clear();
		for (const Entry& e : idx->entries) {
			if (!e.isDir) m_paths.push_back(e.path);
		}
		std::sort(m_paths.begin(), m_paths.end());
		if (m_paths.size() > kMaxPickerPaths) m_paths.resize(kMaxPickerPaths);
	}
	return m_paths;
}

bool ContentBrowser::Writable() const
{
	return AssetIO::IsInitialized() && AssetIO::GetMode() == AssetIO::Mode::Loose;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
void ContentBrowser::Draw(const char* title, bool* open)
{
	++m_frame;
	DrainThumbs();
	const auto index = CurrentIndex();
	if (m_focusNext) {
		ImGui::SetNextWindowFocus();
		m_focusNext = false;
	}
	if (!ImGui::Begin(title, open)) {
		ImGui::End();
		return;
	}

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) GoUp();
		if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) m_pendingOpen = m_selected;
		if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) GoBack();
		if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) GoForward();
	}

	DrawToolbar();
	DrawFilterRow();
	ImGui::Separator();

	// The details strip takes a share of short drawers (7 lines when there is room) and the item table the rest.
	const float frameH = ImGui::GetFrameHeight();
	const float detailsH = std::clamp(ImGui::GetContentRegionAvail().y * 0.35f, frameH * 3.0f, frameH * 7.0f);
	const float bodyH = std::max(ImGui::GetContentRegionAvail().y - detailsH - ImGui::GetStyle().ItemSpacing.y, 60.0f);
	if (ImGui::BeginTable("##cb_body", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, bodyH))) {
		ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 200.0f);
		ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::BeginChild("##cb_tree", ImVec2(0, bodyH));
		DrawFolderTree(*index);
		ImGui::EndChild();
		ImGui::TableSetColumnIndex(1);
			DrawContents(*index, bodyH);
		ImGui::EndTable();
	}
	DrawDetails(*index, detailsH);

	// Double-click (or Enter) is handled after the items so that navigation does not change them mid-frame.
	if (!m_pendingOpen.empty()) {
		const std::string path = std::move(m_pendingOpen);
		m_pendingOpen.clear();
		if (auto it = index->byPath.find(path); it != index->byPath.end()) Open(index->entries[it->second]);
	}
	DrawModal();
	ImGui::End();
}

void ContentBrowser::DrawToolbar()
{
	const float size = 24.0f;
	if (solace::IconButton("##cb_back", solace::Icon::ChevronLeft, "Back (Alt+Left)", false, !m_back.empty(), size)) GoBack();
	ImGui::SameLine();
	if (solace::IconButton("##cb_fwd", solace::Icon::ChevronRight, "Forward (Alt+Right)", false, !m_forward.empty(), size)) GoForward();
	ImGui::SameLine();
	if (solace::IconButton("##cb_up", solace::Icon::ChevronUp, "Up (Backspace)", false, !m_folder.empty(), size)) GoUp();
	ImGui::SameLine();
	if (solace::IconButton("##cb_refresh", solace::Icon::Refresh, "Refresh now", false, true, size)) RequestRescan();
	ImGui::SameLine();

	// Breadcrumb: every segment of the current folder is a button.
	if (ImGui::SmallButton("Project")) Navigate("", true);
	std::string acc;
	size_t start = 0;
	while (start < m_folder.size()) {
		const size_t slash = m_folder.find('/', start);
		const std::string seg = m_folder.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		acc = acc.empty() ? seg : acc + "/" + seg;
		ImGui::SameLine();
		ImGui::TextDisabled(">");
		ImGui::SameLine();
		ImGui::PushID(acc.c_str());
		if (ImGui::SmallButton(seg.c_str())) Navigate(acc, true);
		ImGui::PopID();
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
}

void ContentBrowser::DrawFilterRow()
{
	const float controlsW = 120.0f + 110.0f + 90.0f + 3.0f * ImGui::GetStyle().ItemSpacing.x + 20.0f;
	const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - (m_gridView ? controlsW : controlsW - 90.0f));
	solace::SearchField("##cb_search", m_query, sizeof(m_query), m_folder.empty() ? "Search the project" : "Search this folder and below", searchW);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::BeginCombo("##cb_type", kTypeFilters[m_typeFilter].label)) {
		for (int i = 0; i < static_cast<int>(std::size(kTypeFilters)); ++i) {
			if (ImGui::Selectable(kTypeFilters[i].label, m_typeFilter == i)) m_typeFilter = i;
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	const char* views[] = { "Tiles", "List" };
	const int view = solace::SegmentedControl("##cb_view", views, 2, m_gridView ? 0 : 1, 110.0f);
	m_gridView = view == 0;
	if (m_gridView) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		ImGui::SliderFloat("##cb_size", &m_tileSize, kTileMin, kTileMax, "%.0f px");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile size");
	}
}

void ContentBrowser::DrawFolderTree(const Index& idx)
{
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (m_folder.empty()) flags |= ImGuiTreeNodeFlags_Selected;
	const bool open = ImGui::TreeNodeEx("Project", flags);
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Navigate("", true);
	if (open) {
		DrawFolderChildren(idx, "");
		ImGui::TreePop();
	}
}

void ContentBrowser::DrawFolderChildren(const Index& idx, const std::string& dir)
{
	const auto it = idx.children.find(dir);
	if (it == idx.children.end()) return;
	for (size_t i : it->second) {
		const Entry& e = idx.entries[i];
		if (!e.isDir) continue;
		bool hasSub = false;
		if (const auto sub = idx.children.find(e.path); sub != idx.children.end()) {
			for (size_t j : sub->second) {
				if (idx.entries[j].isDir) { hasSub = true; break; }
			}
		}
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (m_folder == e.path) flags |= ImGuiTreeNodeFlags_Selected;
		if (!hasSub) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const bool open = ImGui::TreeNodeEx(e.path.c_str(), flags, "%s", e.name.c_str());
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Navigate(e.path, true);
		if (open && hasSub) {
			DrawFolderChildren(idx, e.path);
			ImGui::TreePop();
		}
	}
}

// Entries shown in the item pane: the folder's children, or every match below the folder while searching.
std::vector<const ContentBrowser::Entry*> ContentBrowser::Collect(const Index& idx) const
{
	std::vector<const Entry*> out;
	const TypeFilter& filter = kTypeFilters[m_typeFilter];
	const auto matchesType = [&](const Entry& e) { return filter.any || e.kind == filter.kind; };
	if (m_query[0] == '\0') {
		if (const auto it = idx.children.find(m_folder); it != idx.children.end()) {
			for (size_t i : it->second) {
				const Entry& e = idx.entries[i];
				if (e.isDir || matchesType(e)) out.push_back(&e);
			}
		}
		return out;
	}
	const std::string scope = m_folder.empty() ? std::string() : m_folder + "/";
	for (const Entry& e : idx.entries) {
		if (!scope.empty() && e.path.compare(0, scope.size(), scope) != 0) continue;
		if (e.isDir ? !filter.any : !matchesType(e)) continue;
		if (!ContainsNoCase(e.path, m_query)) continue;
		out.push_back(&e);
		if (out.size() >= kMaxSearchResults) break;
	}
	return out;
}

void ContentBrowser::DrawContents(const Index& idx, float height)
{
	const std::vector<const Entry*> items = Collect(idx);
	m_visibleCount = items.size();
	if (m_query[0] != '\0') {
		ImGui::TextDisabled("%zu result(s) for \"%s\"%s", items.size(), m_query,
			items.size() >= kMaxSearchResults ? " (first 2000 shown)" : "");
	}
	else {
		ImGui::TextDisabled("%zu item(s)", items.size());
	}
	ImGui::BeginChild("##cb_items", ImVec2(0, height));
	if (items.empty()) {
		if (idx.entries.empty()) ImGui::TextDisabled("No assets found yet. The project is scanned in the background.");
		else if (m_query[0] == '\0') ImGui::TextDisabled("This folder is empty. Right-click to create something.");
		else ImGui::TextDisabled("Nothing matches the search and type filter.");
	}
	if (m_gridView) DrawGrid(items);
	else DrawList(items);
	DrawBackgroundMenu();
	ImGui::EndChild();
}

void ContentBrowser::DrawGrid(const std::vector<const Entry*>& items)
{
	const float tile = m_tileSize;
	const float labelH = ImGui::GetTextLineHeight() + 6.0f;
	const float spacing = 10.0f;
	const float cell = tile + spacing;
	const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / cell));
	int col = 0;
	for (const Entry* e : items) {
		ImGui::PushID(e->path.c_str());
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 thumbMax(origin.x + tile, origin.y + tile);
		const bool selected = m_selected == e->path;
		if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tile, tile + labelH))) {
			m_selected = e->path;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		DrawThumb(dl, *e, origin, thumbMax, kThumbDim);
		if (selected) dl->AddRect(origin, ImVec2(thumbMax.x, thumbMax.y + labelH), ImGui::GetColorU32(ImGuiCol_NavHighlight), 6.0f, 0, 2.0f);
		const std::string label = TruncateText(e->name, tile);
		const float textW = ImGui::CalcTextSize(label.c_str()).x;
		dl->AddText(ImVec2(origin.x + (tile - textW) * 0.5f, thumbMax.y + 3.0f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
		HandleItem(*e, hovered);
		ImGui::PopID();
		if (++col < cols) ImGui::SameLine(0.0f, spacing);
		else col = 0;
	}
}

void ContentBrowser::DrawList(const std::vector<const Entry*>& items)
{
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
	if (!ImGui::BeginTable("##cb_list", 4, flags)) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 84.0f);
	ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 140.0f);
	ImGui::TableHeadersRow();
	for (const Entry* e : items) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::PushID(e->path.c_str());
		const bool selected = m_selected == e->path;
		if (ImGui::Selectable(e->name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
			m_selected = e->path;
		}
		HandleItem(*e, ImGui::IsItemHovered());
		ImGui::PopID();
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(AssetKindLabel(e->kind));
		ImGui::TableSetColumnIndex(2);
		if (!e->isDir) ImGui::TextUnformatted(FormatSize(e->size).c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::TextUnformatted(FormatTime(e->mtime).c_str());
	}
	ImGui::EndTable();
}

void ContentBrowser::DrawThumb(ImDrawList* dl, const Entry& e, ImVec2 min, ImVec2 max, int maxDim)
{
	dl->AddRectFilled(min, max, IM_COL32(24, 26, 31, 255), 6.0f);
	if (e.kind == AssetKind::Texture && !e.isDir && ImGui::IsRectVisible(min, max)) {
		if (const CachedThumb* t = Thumb(e, maxDim); t && t->tex) {
			const float s = std::min((max.x - min.x) / static_cast<float>(t->w), (max.y - min.y) / static_cast<float>(t->h));
			const ImVec2 size(t->w * s, t->h * s);
			const ImVec2 p0(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f);
			dl->AddImage((ImTextureID)rhi::Gpu().ImGuiTextureId(t->tex), p0,
				ImVec2(p0.x + size.x, p0.y + size.y));
			return;
		}
	}
	// Typed icon with a badge for the file type (textures and models without a decoded preview fall back to this).
	const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - (e.isDir ? 0.0f : 8.0f));
	solace::DrawIcon(dl, KindIcon(e.kind), center, (max.x - min.x) * 0.36f, KindColor(e.kind), 1.6f);
	if (!e.isDir) {
		const std::string badge = BadgeText(e.ext);
		const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
		dl->AddText(ImVec2((min.x + max.x - ts.x) * 0.5f, max.y - ts.y - 6.0f), KindColor(e.kind), badge.c_str());
	}
}

void ContentBrowser::HandleItem(const Entry& e, bool hovered)
{
	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_pendingOpen = e.path;
	if (!e.isDir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		// The payload is the logical path: inspector asset fields and the viewport accept it (kAssetDragType).
		ImGui::SetDragDropPayload(nox::editor::kAssetDragType, e.path.c_str(), e.path.size() + 1);
		ImGui::TextUnformatted(e.path.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_selected = e.path;
	if (ImGui::BeginPopupContextItem("##cb_entry")) {
		DrawEntryMenu(e);
		ImGui::EndPopup();
	}
}

void ContentBrowser::DrawEntryMenu(const Entry& e)
{
	const bool writable = Writable();
	if (ImGui::MenuItem(e.isDir ? "Open Folder" : "Open")) Open(e);
	if (ImGui::MenuItem("Show in Explorer", nullptr, false, writable)) ShowInExplorer(e.path);
	if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(e.path.c_str());
	if (!e.isDir && EndsWith(Lower(e.path), ".lua") && ImGui::MenuItem("Open in External Editor", nullptr, false, writable)) OpenScript(e);
	const std::string projectRoot = NoxLua::ProjectRootOf(e.path);
	if (!projectRoot.empty() && ImGui::MenuItem("Open Project Folder in External Editor", nullptr, false, writable)) {
		std::string error;
		if (!ExternalEditor::OpenLogicalFolder(projectRoot, error)) ui::Toast(error, ui::ToastKind::Warning);
	}
	ImGui::Separator();
	if (ImGui::MenuItem("Duplicate", nullptr, false, writable && !e.isDir)) Duplicate(e);
	if (ImGui::MenuItem("Rename...", nullptr, false, writable)) OpenModal(Modal::Rename, e.path, e.name);
	if (ImGui::MenuItem("Delete...", nullptr, false, writable)) OpenModal(Modal::Delete, e.path, e.name);
	ImGui::Separator();
	DrawCreateMenu();
	if (!writable) ImGui::TextDisabled("Packed assets are read-only");
}

void ContentBrowser::DrawBackgroundMenu()
{
	if (!ImGui::BeginPopupContextWindow("##cb_background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) return;
	DrawCreateMenu();
	if (ImGui::MenuItem("Show Folder in Explorer", nullptr, false, Writable())) ShowInExplorer(m_folder);
	if (ImGui::MenuItem("Refresh")) RequestRescan();
	if (!Writable()) ImGui::TextDisabled("Packed assets are read-only");
	ImGui::EndPopup();
}

void ContentBrowser::DrawCreateMenu()
{
	if (!ImGui::BeginMenu("New", Writable())) return;
	if (ImGui::MenuItem("Folder")) OpenModal(Modal::NewFolder, m_folder, "New Folder");
	if (ImGui::MenuItem("Scene")) OpenModal(Modal::NewScene, m_folder, "new_scene.json");
	if (ImGui::MenuItem("Lua Script")) OpenModal(Modal::NewScript, m_folder, "new_script.lua");
	// New Material: skipped, the project has no *.material.json format yet.
	ImGui::EndMenu();
}

void ContentBrowser::DrawDetails(const Index& idx, float height)
{
	ImGui::BeginChild("##cb_details", ImVec2(0, height), ImGuiChildFlags_Borders);
	const Entry* e = nullptr;
	if (!m_selected.empty()) {
		if (const auto it = idx.byPath.find(m_selected); it != idx.byPath.end()) e = &idx.entries[it->second];
	}
	if (!e) {
		ImGui::TextDisabled("Select an asset for details. Double-click opens it. Drag it into the Scene view, onto a node or into an asset field.");
	}
	else {
		const float preview = std::max(48.0f, height - 2.0f * ImGui::GetStyle().WindowPadding.y - 4.0f);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(preview, preview));
		DrawThumb(ImGui::GetWindowDrawList(), *e, p0, ImVec2(p0.x + preview, p0.y + preview), kPreviewDim);
		ImGui::SameLine(0.0f, 16.0f);
		ImGui::BeginGroup();
		ImGui::TextUnformatted(e->name.c_str());
		ImGui::TextDisabled("%s", e->path.c_str());
		ImGui::Text("Type: %s", AssetKindLabel(e->kind));
		if (!e->isDir) ImGui::Text("Size: %s", FormatSize(e->size).c_str());
		ImGui::Text("Modified: %s", FormatTime(e->mtime).c_str());
		if (e->kind == AssetKind::Texture) {
			if (const CachedThumb* t = Thumb(*e, kPreviewDim); t && t->origW > 0) ImGui::Text("Dimensions: %d x %d px", t->origW, t->origH);
		}
		if (e->kind == AssetKind::Model) ImGui::TextDisabled("Model: drag into the Scene view to place it, or double-click to add it.");
		if (e->kind == AssetKind::Prefab) ImGui::TextDisabled("Prefab: double-click to instantiate it, or drop it on a node.");
		ImGui::EndGroup();
	}
	ImGui::EndChild();
}

void ContentBrowser::DrawModal()
{
	if (m_modal == Modal::None) return;
	const char* title = "";
	switch (m_modal) {
	case Modal::Rename: title = "Rename Asset"; break;
	case Modal::Delete: title = "Delete Asset"; break;
	case Modal::NewFolder: title = "New Folder"; break;
	case Modal::NewScene: title = "New Scene"; break;
	case Modal::NewScript: title = "New Lua Script"; break;
	default: break;
	}
	if (m_openModal) {
		ImGui::OpenPopup(title);
		m_openModal = false;
	}
	if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		m_modal = Modal::None;
		return;
	}
	bool close = false;
	bool confirm = false;
	if (m_modal == Modal::Delete) {
		ImGui::Text("Move \"%s\" to the Recycle Bin?", Basename(m_modalTarget).c_str());
		ImGui::TextDisabled("References from scenes and prefabs are not changed.");
		if (ImGui::Button("Move to Recycle Bin")) confirm = true;
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) close = true;
	}
	else {
		const char* label = m_modal == Modal::Rename ? "New name" : "Name";
		ImGui::TextUnformatted(label);
		ImGui::SetNextItemWidth(320.0f);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		const bool enter = ImGui::InputText("##cb_name", m_nameBuf, sizeof(m_nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		const char* action = m_modal == Modal::Rename ? "Rename" : "Create";
		if (enter || ImGui::Button(action)) confirm = true;
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) close = true;
		if (m_modal == Modal::Rename) ImGui::TextDisabled("Scenes and prefabs that reference the old name are not updated.");
	}
	if (confirm) {
		if (m_modal == Modal::Rename) ApplyRename(m_modalTarget, m_nameBuf);
		else if (m_modal == Modal::Delete) DeleteEntry(m_modalTarget);
		else CreateNew(m_modal, m_nameBuf);
		close = true;
	}
	if (close) {
		ImGui::CloseCurrentPopup();
		m_modal = Modal::None;
	}
	ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------
void ContentBrowser::Open(const Entry& e)
{
	if (e.isDir) {
		Navigate(e.path, true);
		return;
	}
	switch (e.kind) {
	case AssetKind::Scene:
		if (m_host.openScene) m_host.openScene(e.path);
		break;
	case AssetKind::Prefab:
	case AssetKind::Model:
		if (m_host.playing && m_host.playing()) {
			ui::Toast("Stop the game before adding assets to the scene", ui::ToastKind::Warning);
			break;
		}
		if (m_host.placeAsset) m_host.placeAsset(e.path);
		break;
	case AssetKind::Script:
		OpenScript(e);
		break;
	default:
		OpenWithShell(e);
		break;
	}
}

void ContentBrowser::OpenScript(const Entry& e)
{
	const auto phys = PhysicalPath(e.path);
	if (!phys) {
		ui::Toast("Packed scripts have no file on disk to open", ui::ToastKind::Warning);
		return;
	}
	// The editor chosen in Edit > Preferences (ExternalEditor.h): VS Code by default, else the OS handler.
	std::string error;
	if (!ExternalEditor::OpenLogicalFile(e.path, 0, error)) ui::Toast(error, ui::ToastKind::Warning);
}

void ContentBrowser::OpenWithShell(const Entry& e)
{
	const auto phys = PhysicalPath(e.path);
	if (!phys) {
		ui::Toast("Packed assets have no file on disk to open", ui::ToastKind::Warning);
		return;
	}
	if (!OpenInShell(*phys)) ui::Toast("No program is associated with " + e.name, ui::ToastKind::Warning);
}

void ContentBrowser::ShowInExplorer(const std::string& path)
{
	std::wstring args;
	if (path.empty()) {
		if (!Writable()) return;
		args = L"\"" + AssetIO::GetConfig().looseRoot.wstring() + L"\"";
	}
	else {
		const auto phys = PhysicalPath(path);
		if (!phys) {
			ui::Toast("Packed assets are not files on disk", ui::ToastKind::Warning);
			return;
		}
		args = L"/select,\"" + phys->wstring() + L"\"";
	}
	ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void ContentBrowser::OpenModal(Modal kind, const std::string& target, const std::string& initialName)
{
	m_modal = kind;
	m_openModal = true;
	m_modalTarget = target;
	std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", initialName.c_str());
}

void ContentBrowser::ApplyRename(const std::string& oldPath, const std::string& newName)
{
	if (!ValidName(newName)) {
		ui::Toast("That name is not valid", ui::ToastKind::Warning);
		return;
	}
	const std::string newPath = Join(AssetIO::Parent(oldPath), newName);
	if (newPath == oldPath) return;
	const auto src = PhysicalPath(oldPath);
	const auto dst = PhysicalPath(newPath);
	if (!src || !dst) return;
	std::error_code ec;
	if (fs::exists(*dst, ec)) {
		ui::Toast("A file with that name already exists", ui::ToastKind::Warning);
		return;
	}
	fs::rename(*src, *dst, ec);
	if (ec) {
		ui::Toast("Rename failed: " + ec.message(), ui::ToastKind::Error);
		return;
	}
	if (m_selected == oldPath) m_selected = newPath;
	if (m_folder == oldPath) m_folder = newPath;
	RequestRescan();
	ui::Toast("Renamed", ui::ToastKind::Success);
}

void ContentBrowser::DeleteEntry(const std::string& path)
{
	const auto phys = PhysicalPath(path);
	if (!phys) return;
	if (!MoveToRecycleBin(*phys)) {
		ui::Toast("Could not move " + Basename(path) + " to the Recycle Bin", ui::ToastKind::Error);
		return;
	}
	if (m_selected == path || m_selected.rfind(path + "/", 0) == 0) m_selected.clear();
	if (m_folder == path || m_folder.rfind(path + "/", 0) == 0) Navigate(AssetIO::Parent(path), false);
	RequestRescan();
	ui::Toast("Moved to the Recycle Bin", ui::ToastKind::Info);
}

void ContentBrowser::Duplicate(const Entry& e)
{
	const auto src = PhysicalPath(e.path);
	if (!src) return;
	std::string stem, ext;
	SplitName(e.name, stem, ext);
	for (int n = 1; n < 100; ++n) {
		const std::string name = stem + (n == 1 ? std::string(" copy") : " copy " + std::to_string(n)) + ext;
		const std::string target = Join(e.parent, name);
		const auto dst = PhysicalPath(target);
		std::error_code ec;
		if (!dst || fs::exists(*dst, ec)) continue;
		fs::copy_file(*src, *dst, ec);
		if (ec) {
			ui::Toast("Duplicate failed: " + ec.message(), ui::ToastKind::Error);
			return;
		}
		m_selected = target;
		RequestRescan();
		ui::Toast("Duplicated", ui::ToastKind::Success);
		return;
	}
	ui::Toast("Too many copies of this asset", ui::ToastKind::Warning);
}

void ContentBrowser::CreateNew(Modal kind, const std::string& rawName)
{
	std::string name = rawName;
	if (kind == Modal::NewScene && !EndsWith(Lower(name), ".json")) name += ".json";
	if (kind == Modal::NewScript && !EndsWith(Lower(name), ".lua")) name += ".lua";
	if (!ValidName(name)) {
		ui::Toast("That name is not valid", ui::ToastKind::Warning);
		return;
	}
	const std::string target = Join(m_folder, name);
	const auto phys = PhysicalPath(target);
	if (!phys) return;
	std::error_code ec;
	if (fs::exists(*phys, ec)) {
		ui::Toast(name + " already exists", ui::ToastKind::Warning);
		return;
	}
	if (kind == Modal::NewFolder) {
		fs::create_directories(*phys, ec);
	}
	else if (kind == Modal::NewScene) {
		// The minimal scene document the loader accepts (same shape as the shipped scenes).
		std::string stem, ext;
		SplitName(name, stem, ext);
		const json doc = { { "scene_name", stem }, { "exposure", 1.0 }, { "gamma", 2.2 }, { "skybox", "" }, { "nodes", json::array() } };
		std::ofstream out(*phys, std::ios::binary | std::ios::trunc);
		out << doc.dump(2) << '\n';
		if (!out) ec = std::make_error_code(std::errc::io_error);
	}
	else {
		std::string stem, ext;
		SplitName(name, stem, ext);
		std::ofstream out(*phys, std::ios::binary | std::ios::trunc);
		out << "-- " << name << ": a Lua system for this project. See docs/lua_api.md.\n"
			<< "nox.system({\n"
			<< "    name = \"" << stem << "\",\n"
			<< "    order = 100,\n"
			<< "    update = function(ctx, dt)\n"
			<< "    end,\n"
			<< "})\n";
		if (!out) ec = std::make_error_code(std::errc::io_error);
	}
	if (ec) {
		ui::Toast("Could not create " + name + ": " + ec.message(), ui::ToastKind::Error);
		return;
	}
	m_selected = target;
	RequestRescan();
	ui::Toast("Created " + name, ui::ToastKind::Success);
}

} // namespace nox::content
