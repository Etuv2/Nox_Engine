#pragma once
// Lua `ui` drawing API (docs/lua_api.md "UI"). Draw handlers (nox.on_draw) record commands into a bounded,
// CPU-side list (UiRecorder). The engine plays the list back onto the game overlay ImDrawList after the
// handlers return, so recording is testable without a GPU or an ImGui frame.
//
// Coordinates are overlay-relative (origin = top-left of the game view). Colors are packed as
// IM_COL32 (ABGR in memory). Fonts are project-declared (project.json "fonts"), loaded through AssetIO
// in InitializeResources and looked up by id.
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct ImDrawList;
struct ImFont;
struct lua_State;
class Texture;

namespace NoxLua {

	struct DataState;
	class UiNavigator;

	namespace ui_limits {
		constexpr size_t kMaxCommands = 4096;        // per frame
		constexpr size_t kMaxPoints = 32768;         // per frame, all polylines/polygons/arcs
		constexpr size_t kMaxPointsPerCall = 512;    // one polyline/polygon call
		constexpr size_t kMaxClipDepth = 16;
		constexpr size_t kMaxTextBytes = 512;
		constexpr size_t kMaxArcSegments = 256;      // arc() samples
		constexpr size_t kMaxTextures = 16;          // ui.texture images per module
		constexpr size_t kMaxTextureBytes = 64u * 1024u * 1024u;   // decoded RGBA bytes of all of them
		constexpr int kMaxTextureSide = 4096;        // pixels per side
		constexpr size_t kMaxTextureFileBytes = 16u * 1024u * 1024u;
	}

	enum class UiOp : uint8_t { Rect, RectFilled, Line, Polyline, PolygonFilled, Circle, CircleFilled, Text, Image, Picture, ClipPush, ClipPop };
	enum class UiAlign : uint8_t { Left, Center, Right };
	enum class UiValign : uint8_t { Top, Middle, Bottom };

	struct UiCommand {
		UiOp op = UiOp::Rect;
		uint32_t color = 0xFFFFFFFFu;     // IM_COL32 packed
		glm::vec2 a{ 0.0f }, b{ 0.0f };   // rect min/max, line ends, circle centre (a) + radius (b.x), anchor (a)
		glm::vec4 uv{ 0.0f, 0.0f, 1.0f, 1.0f };   // Picture: u0, v0, u1, v1
		float thickness = 1.0f;
		float rounding = 0.0f;
		float size = 0.0f;                // text size in pixels
		bool closed = false;              // polyline closed
		UiAlign halign = UiAlign::Left;
		UiValign valign = UiValign::Top;
		uint32_t pointStart = 0;          // into UiRecorder::Points()
		uint32_t pointCount = 0;
		std::string font;                 // text: project font id ("" = default)
		std::string text;                 // text string, or icon id for Image
	};

	// Bounded per-frame command list. Begin() clears it but keeps capacity.
	class UiRecorder {
	public:
		void Begin();
		// Each returns false (and counts a drop) when a bound would be exceeded.
		bool Add(UiCommand command);
		bool AddPoints(UiCommand command, const std::vector<glm::vec2>& points);
		// Clip stack is balanced at record time; PopClip fails with no open clip.
		bool PushClip(const glm::vec2& min, const glm::vec2& max);
		bool PopClip();
		size_t ClipDepth() const { return m_clipDepth; }

		const std::vector<UiCommand>& Commands() const { return m_commands; }
		const std::vector<glm::vec2>& Points() const { return m_points; }
		size_t Dropped() const { return m_dropped; }

	private:
		std::vector<UiCommand> m_commands;
		std::vector<glm::vec2> m_points;
		size_t m_clipDepth = 0;
		size_t m_dropped = 0;
	};

	// Project fonts by id. Entries are ImGui atlas fonts (owned by the ImGui context).
	struct UiFontEntry {
		std::string id;
		ImFont* font = nullptr;
		float size = 0.0f;   // baked size of the file's font (informational)
	};
	class UiFontTable {
	public:
		void Add(UiFontEntry entry);
		const UiFontEntry* Find(const std::string& id) const;
		const std::vector<UiFontEntry>& Entries() const { return m_entries; }
	private:
		std::vector<UiFontEntry> m_entries;
	};

	// Declaration from project.json "fonts": [{"id","path","size"}].
	struct UiFontDecl {
		std::string id;
		std::string path;   // logical, resolved against the descriptor
		float size = 16.0f;
	};

	// Loads the declared fonts into the ImGui atlas (AssetIO bytes copied into atlas-owned memory). A missing
	// or unreadable file adds the ImGui default font under the same id and returns the reason in `messages`.
	// Must run on the GUI thread with a live ImGui context, before the font texture is created or rebuilt.
	void LoadUiFonts(const std::vector<UiFontDecl>& decls, UiFontTable& out, std::vector<std::string>& messages);

	// Images decoded from project files through AssetIO and uploaded once (ui.texture). Owned by the module, so the
	// GPU textures live as long as the module and survive world reloads. Bounded by ui_limits (count and bytes).
	class UiTextureCache {
	public:
		UiTextureCache() = default;
		~UiTextureCache();
		UiTextureCache(const UiTextureCache&) = delete;
		UiTextureCache& operator=(const UiTextureCache&) = delete;

		// Loads the logical image once. False with a reason when it is missing, too large or over budget.
		bool Acquire(const std::string& logical, std::string& error);
		// GPU handle for ImGui (ImTextureID value), null when the image is not in the cache.
		const void* Handle(const std::string& logical) const;
		void Clear();
		size_t Count() const { return m_entries.size(); }
		size_t Bytes() const { return m_bytes; }

	private:
		struct Entry {
			std::unique_ptr<Texture> texture;
			size_t bytes = 0;
		};
		std::map<std::string, Entry> m_entries;
		size_t m_bytes = 0;
	};

	// Lua binding state, one per VM (owned by LuaWorld). Recorder is set only while draw handlers run.
	struct UiState {
		UiRecorder* recorder = nullptr;
		const UiFontTable* fonts = nullptr;
		UiTextureCache* textures = nullptr;   // ui.texture cache (module-owned); null outside the module
		const DataState* data = nullptr;      // read policy of ui.texture (project declarations)
		UiNavigator* nav = nullptr;           // focus and navigation of the widgets (LuaUiFocus.h), owned by the world
		glm::vec2 viewport{ 0.0f };
		glm::vec2 pointerOverlay{ 0.0f };   // pointer position relative to the overlay, valid while drawing
		glm::vec2 origin{ 0.0f };           // overlay origin in window coordinates (widget hit regions)
	};

	// Creates the `ui` table (returned as a registry reference) and binds its functions to `state`.
	int InstallUiApi(lua_State* L, UiState* state);

	// Plays a recorded list onto an ImDrawList, offset by origin. Unbalanced clips are closed at the end.
	// `textures` resolves ui.texture images; without it those commands draw nothing.
	void PlaybackUi(const UiRecorder& recorder, const UiFontTable* fonts, ImDrawList* drawList, const glm::vec2& origin,
		const UiTextureCache* textures = nullptr);

	// Pure text measurement with a font (ImGui atlas); used by ui.text_size and tests.
	glm::vec2 MeasureUiText(const UiFontTable* fonts, const std::string& fontId, float size, const std::string& text);
}
