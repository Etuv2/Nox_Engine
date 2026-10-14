#pragma once
// Content Browser (docs/editor.md, "Assets"): folder tree, tile and list views, recursive search and file
// actions over the logical asset tree (AssetIO, so loose and packed projects read the same).
//
// Threads: a scan worker lists the tree every 2 s and publishes an immutable index when it changed (this is
// the file watcher). Thumbnail workers decode and downscale images. Everything that touches the GPU or ImGui
// runs on the UI thread: finished decodes are uploaded at most kUploadsPerFrame per frame, and only tiles that
// are on screen request a thumbnail.
#include <IMGUI/imgui.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "rhi/RHI.h"

namespace nox::content {

// Asset categories used by the browser filter, the tile colours and the double-click action.
enum class AssetKind { Folder, Scene, Prefab, Model, Texture, Material, Script, Audio, Shader, Font, Other };

// Kind of a logical path from its name and location (scenes are *.json under scenes/, prefabs are *.prefab.json).
AssetKind ClassifyAsset(const std::string& logicalPath, bool isDirectory);
const char* AssetKindLabel(AssetKind kind);

class ContentBrowser {
public:
	struct Host {
		std::function<bool()> playing;                                    // scene edits are refused while the game runs
		std::function<void(const std::string& scenePath)> openScene;      // Scene: switch to the scene (unsaved check)
		std::function<void(const std::string& assetPath)> placeAsset;     // Prefab or Model: place at the spawn point
	};

	explicit ContentBrowser(Host host);
	~ContentBrowser();
	ContentBrowser(const ContentBrowser&) = delete;
	ContentBrowser& operator=(const ContentBrowser&) = delete;

	// Draws the window `title` (the layout keeps the name "Assets"). open follows the window's close button.
	void Draw(const char* title, bool* open);
	void FocusNextFrame() { m_focusNext = true; }

	// Sorted logical paths of the project's files (capped), for the inspector's asset pickers.
	const std::vector<std::string>& AssetPaths();

	// NOX_EDITOR_SMOKE: waits for the first published index (false on timeout), enters a folder (false when the
	// index has no such folder) and reports what is shown.
	bool WaitForScan(std::chrono::milliseconds timeout);
	bool SmokeNavigate(const std::string& folder);
	size_t VisibleCount() const { return m_visibleCount; }
	const std::string& Folder() const { return m_folder; }

private:
	struct Entry {
		std::string path;     // logical, '/'-separated
		std::string name;     // last segment
		std::string parent;   // logical directory ("" = project root)
		std::string ext;      // lower-case extension ("" for folders)
		AssetKind kind = AssetKind::Other;
		bool isDir = false;
		uint64_t size = 0;
		int64_t mtime = -1;
	};
	struct Index {
		std::vector<Entry> entries;
		std::map<std::string, std::vector<size_t>> children;   // directory -> entries directly inside (folders first)
		std::unordered_map<std::string, size_t> byPath;
		uint64_t signature = 0;     // hash of every path, size and timestamp: changes when the tree changes
		uint64_t generation = 0;    // increments on every published index
	};
	struct CachedThumb {
		rhi::TextureHandle tex;
		int w = 0, h = 0;          // uploaded size
		int origW = 0, origH = 0;  // size of the source image
		int64_t mtime = -2;        // source timestamp the thumbnail was decoded from
		bool failed = false;
		uint64_t lastUsed = 0;
	};
	struct ThumbJob { std::string key, path; int maxDim = 0; int64_t mtime = -1; };
	struct ThumbResult {
		std::string key;
		int64_t mtime = -1;
		bool ok = false;
		int origW = 0, origH = 0, w = 0, h = 0;
		std::vector<uint8_t> pixels;
	};
	enum class Modal { None, Rename, Delete, NewFolder, NewScene, NewScript };

	std::shared_ptr<const Index> CurrentIndex() const;
	void ScanLoop();
	void WalkLoose(const std::filesystem::path& root, std::vector<Entry>& out);
	void WalkLogical(std::vector<Entry>& out);
	void ThumbLoop();
	void RequestRescan();
	void DrainThumbs();
	const CachedThumb* Thumb(const Entry& e, int maxDim);
	void Retire(rhi::TextureHandle tex);

	void Navigate(const std::string& folder, bool record);
	void GoBack();
	void GoForward();
	void GoUp();

	void DrawToolbar();
	void DrawFilterRow();
	void DrawFolderTree(const Index& idx);
	void DrawFolderChildren(const Index& idx, const std::string& dir);
	void DrawContents(const Index& idx, float height);
	void DrawGrid(const std::vector<const Entry*>& items);
	void DrawList(const std::vector<const Entry*>& items);
	void DrawBackgroundMenu();
	void DrawCreateMenu();
	void DrawDetails(const Index& idx, float height);
	void DrawThumb(ImDrawList* dl, const Entry& e, ImVec2 min, ImVec2 max, int maxDim);
	void HandleItem(const Entry& e, bool hovered);
	void DrawEntryMenu(const Entry& e);
	void DrawModal();
	std::vector<const Entry*> Collect(const Index& idx) const;

	void Open(const Entry& e);
	void OpenScript(const Entry& e);
	void OpenWithShell(const Entry& e);
	void ShowInExplorer(const std::string& logicalPath);
	void OpenModal(Modal kind, const std::string& target, const std::string& initialName);
	void ApplyRename(const std::string& oldPath, const std::string& newName);
	void DeleteEntry(const std::string& path);
	void Duplicate(const Entry& e);
	void CreateNew(Modal kind, const std::string& name);
	bool Writable() const;

	Host m_host;
	std::atomic<bool> m_stop{ false };

	// Index (scan thread publishes, UI thread reads).
	mutable std::mutex m_indexMutex;
	std::shared_ptr<const Index> m_index;
	std::thread m_scanThread;
	std::mutex m_scanMutex;
	std::condition_variable m_scanCv;
	bool m_rescan = false;
	uint64_t m_pathsGeneration = 0;
	std::vector<std::string> m_paths;

	// Thumbnails (workers decode, UI thread uploads).
	std::vector<std::thread> m_thumbWorkers;
	std::mutex m_thumbMutex;
	std::condition_variable m_thumbCv;
	std::deque<ThumbJob> m_thumbJobs;
	std::deque<ThumbResult> m_thumbDone;
	std::set<std::string> m_thumbPending;
	std::unordered_map<std::string, CachedThumb> m_thumbs;   // least recently used evicted above the capacity
	std::vector<std::pair<uint64_t, rhi::TextureHandle>> m_retired;   // destroyed two frames later

	// Navigation and view state.
	std::string m_folder;                  // current folder ("" = project root)
	std::vector<std::string> m_back, m_forward;
	std::string m_selected;                // logical path of the selected entry
	std::string m_pendingOpen;             // double-clicked entry, opened after the items are drawn
	char m_query[128] = "";
	int m_typeFilter = 0;                  // index into the filter table; 0 = all
	bool m_gridView = true;
	float m_tileSize = 112.0f;
	bool m_focusNext = false;
	size_t m_visibleCount = 0;
	uint64_t m_frame = 0;

	// File actions.
	Modal m_modal = Modal::None;
	bool m_openModal = false;
	std::string m_modalTarget;
	char m_nameBuf[256] = "";
};

} // namespace nox::content
