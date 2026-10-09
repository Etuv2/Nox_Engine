// Solace-style editor UI kit for NOX (Dear ImGui 1.91.9-docking, SDL2 + OpenGL3 backends).
//
// Ported from Free-Solace-ImGui-Interface (MIT, Copyright (c) 2026 Pondot), pinned revision
// 6b88e2ed65b06ffbfa26b1aa4561579707fbeb4f. The visual language (near-black surfaces, white
// primary actions, hairline borders, Geist typography, Lucide-style stroke icons, animated
// hover/press feedback) is reproduced with ImGui primitives only. Solace's Win32/DX11 host,
// FreeType rasterizer, ImGui 1.92 APIs and dynamic font sizes are not used.
// See THIRD_PARTY.md in this folder for licenses and the list of adapted files.
//
// Usage order:
//   1. ImGui::CreateContext(); create the SDL2 window and ImGui SDL2/OpenGL3 backends.
//   2. solace::LoadFonts(14.f, dpiScale);             // before the font texture is built
//   3. ImGui_ImplOpenGL3_DestroyFontsTexture(); ImGui_ImplOpenGL3_CreateFontsTexture();
//      (only needed if the font atlas was already uploaded; LoadFonts clears it)
//   4. solace::ApplyTheme(true, dpiScale);            // style + colours (dark or light)
//   5. Each frame, call the controls below inside your windows. Call DrawToasts() last.
//
// Conventions:
//   * Sizes passed as "dp" are multiplied by the DPI scale given to ApplyTheme (see Px()).
//   * Every control that takes an `id` pushes it as an ImGui ID, so repeated labels are safe.
//   * Hover/press/focus transitions are animated through a per-ID store that is garbage
//     collected automatically; no per-control state needs to be owned by the caller.
//   * Focus rings are drawn when keyboard navigation is active (ImGui NavCursorVisible).
//   * Only include/IMGUI and the standard library are required.

#pragma once

#include "IMGUI/imgui.h"
#include "IMGUI/imgui_internal.h"

namespace solace
{

// ---------------------------------------------------------------------------------------------
// Palette and theme
// ---------------------------------------------------------------------------------------------

// Colour roles used by every control. Values come from Solace's dark/light palettes.
struct Palette
{
    ImU32 accent;         // primary action fill (Solace "primary": near-white in dark)
    ImU32 accentText;     // text drawn on accent
    ImU32 background;     // window / viewport background (Solace background)
    ImU32 surface;        // cards, popups, panels (Solace card)
    ImU32 surfaceRaised;  // hovered rows and buttons
    ImU32 surfacePressed; // pressed surfaces
    ImU32 inputFill;      // text fields and sunken wells
    ImU32 border;         // hairline borders (Solace border)
    ImU32 borderStrong;   // hovered or focused borders (Solace border_strong)
    ImU32 text;           // primary text
    ImU32 textMuted;      // secondary text (Solace muted_foreground)
    ImU32 textSubtle;     // tertiary / disabled text
    ImU32 success;
    ImU32 warning;
    ImU32 error;          // Solace destructive
    ImU32 info;
    ImU32 focusRing;      // keyboard focus ring
    ImU32 selection;      // selected row / text selection tint
    ImU32 dirty;          // dirty-state dot
};

// Current palette (valid after ApplyTheme; defaults to dark).
const Palette& GetPalette();
bool IsDarkTheme();

// Sets the full ImGuiStyle (spacing, rounding, borders, every ImGuiCol_ including docking,
// tab, title, menubar and table colours) from the Solace palette. dpiScale multiplies all
// sizes. Safe to call again to switch theme at runtime.
void ApplyTheme(bool dark = true, float dpiScale = 1.0f);

// DPI scale last given to ApplyTheme (1.0 before). Px(dp) returns dp * scale.
float DpiScale();
float Px(float dp);

// Spacing scale from Solace (sp_1..sp_6), in dp.
namespace space
{
inline constexpr float xs = 4.f;   // sp_1
inline constexpr float sm = 6.f;   // sp_1_5
inline constexpr float md = 8.f;   // sp_2
inline constexpr float lg = 12.f;  // sp_3
inline constexpr float xl = 16.f;  // sp_4
inline constexpr float xxl = 24.f; // sp_6
} // namespace space

// ---------------------------------------------------------------------------------------------
// Typography
// ---------------------------------------------------------------------------------------------

// Faces loaded by LoadFonts. Regular/Medium/Semibold are at the body size; Caption is Regular
// at 0.85x and Title is Semibold at 1.25x (ImGui 1.91 fonts have a fixed size, so each size is
// its own font).
enum class Face
{
    Regular,
    Medium,
    Semibold,
    Caption,
    Title
};

// Loads Geist (SIL OFL 1.1) from fontDir (Geist-Regular.ttf, Geist-Medium.ttf,
// Geist-SemiBold.ttf). sizePx is the body size in pixels (Solace uses 14); the effective size
// is sizePx * dpiScale. Clears the current ImFontAtlas. Returns false and falls back to the
// ImGui default font if a file is missing. Call before the font texture is uploaded, or
// rebuild the OpenGL3 font texture afterwards.
bool LoadFonts(float sizePx = 14.f, float dpiScale = 1.0f, const char* fontDir = "fonts/solace");
ImFont* Font(Face face = Face::Regular);

// ---------------------------------------------------------------------------------------------
// Icons (Lucide paths, ISC licence; stroke-based, 24 unit viewBox)
// ---------------------------------------------------------------------------------------------

enum class Icon
{
    None,
    Play,
    Pause,
    Stop,
    Save,
    Build,      // hammer
    Run,        // external link / launch
    Package,
    Folder,
    Search,
    Eye,
    EyeOff,
    Settings,
    Layers,
    Flag,
    Map,
    Camera,
    Light,      // lightbulb
    Sun,
    Moon,
    Undo,
    Redo,
    Plus,
    Copy,
    Trash,
    Refresh,
    ChevronLeft,
    ChevronRight,
    ChevronUp,
    ChevronDown,
    Check,
    Cross,
    Info,
    Alert,
    PanelLeft,
    User,
    Mail,
    Lock,
    Sparkles,
    Bell,
    Command
};

// Draws the icon centred on `center`, `size` pixels square. strokeWidth is in the 24 unit
// viewBox (Lucide default 2). Colour alpha is honoured.
void DrawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, float size, ImU32 color,
              float strokeWidth = 2.f);

// Draws a rotating arc (loading indicator) centred on `center`.
void DrawSpinnerArc(ImDrawList* drawList, ImVec2 center, float radius, float thickness,
                    float angle, ImU32 color);

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------

enum class ButtonKind
{
    Primary,     // accent fill, accentText label (Solace "Continue")
    Secondary,   // surface fill with hairline border
    Ghost,       // no fill until hovered
    Destructive  // error-tinted outline
};

// Text button. label may contain "##id" to disambiguate; only the part before "##" is drawn.
// size.x = 0 uses the label width plus padding; size.y = 0 uses 36 dp.
// Returns true on the frame it is clicked. Shows focus ring and animated hover/press.
bool Button(const char* label, ButtonKind kind = ButtonKind::Secondary, ImVec2 size = ImVec2(0, 0),
            bool enabled = true);
inline bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true)
{
    return Button(label, ButtonKind::Primary, size, enabled);
}
inline bool SecondaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true)
{
    return Button(label, ButtonKind::Secondary, size, enabled);
}

// Square icon button, `size` dp. active draws the selected state (accent tint, white glyph).
// tooltip, if given, is shown after the standard hover delay.
bool IconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool active = false,
                bool enabled = true, float size = 28.f);

// Icon + label toolbar button (32 dp high). Returns true when clicked.
bool ToolbarButton(const char* id, Icon icon, const char* label, bool active = false,
                   bool enabled = true, const char* tooltip = nullptr);

// Pill toggle (Solace switch). Flips *value on click; returns true when it changed.
bool ToggleSwitch(const char* id, bool* value, bool enabled = true, const char* label = nullptr);

// Segmented control. Returns the (possibly new) selected index.
int SegmentedControl(const char* id, const char* const* labels, int count, int current,
                     float width = 0.f);

// Left-aligned sidebar / navigation row, full width when width <= 0. Returns true when clicked.
bool NavItem(const char* id, Icon icon, const char* label, bool selected = false,
             const char* trailing = nullptr, float width = 0.f, bool enabled = true);

// ---------------------------------------------------------------------------------------------
// Inputs and sections
// ---------------------------------------------------------------------------------------------

// Search field with leading magnifier, placeholder and a clear button that appears when the
// buffer is non-empty. Returns true when the buffer changed this frame (typing or clearing).
bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint = "Search",
                 float width = 0.f);

// Collapsible section header in the Solace settings style (small uppercase, muted, chevron).
// Toggles *open on click and returns the new value.
bool SectionHeader(const char* id, const char* title, bool* open);

// Inspector property grid. BeginPropertyGrid returns true when the table was started; only
// call EndPropertyGrid if it returned true. labelWidth is the label column width in dp.
// For each row: PropertyLabel(name) draws the label in column 0 and leaves the cursor in
// column 1, so the caller then draws the widget (ImGui::SetNextItemWidth(-FLT_MIN) fills it).
bool BeginPropertyGrid(const char* id, float labelWidth = 128.f);
void PropertyLabel(const char* label, const char* tooltip = nullptr);
void EndPropertyGrid();

// Card: rounded surface with hairline border (Solace card). Pair with EndCard() always.
bool BeginCard(const char* id, ImVec2 size = ImVec2(0, 0), bool padded = true);
void EndCard();

// ---------------------------------------------------------------------------------------------
// Status and feedback
// ---------------------------------------------------------------------------------------------

enum class Status
{
    Neutral,
    Info,
    Success,
    Warning,
    Error
};

// Pill badge with a tinted outline, Solace badge style.
void Badge(const char* label, Status status = Status::Neutral);

// Spinner of `diameter` dp. color 0 uses the palette text colour.
void Spinner(float diameter = 16.f, ImU32 color = 0);

// Determinate bar (fraction in [0,1]) of `width` dp (0 = fill available width), 6 dp high.
void ProgressBar(float fraction, float width = 0.f);
// Indeterminate sweeping bar of `width` dp.
void IndeterminateProgressBar(float width = 0.f);

// Small dot marking unsaved changes. Draws at the cursor and advances it by the dot width.
void DirtyDot(bool dirty = true, float diameter = 6.f);

// Styled tooltip (Solace popover colours). Call immediately after the item it describes.
void TooltipOnHover(const char* text);

// Toasts: queue with PushToast, then draw once per frame with DrawToasts.
enum class ToastCorner
{
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};
void PushToast(const char* title, const char* body = nullptr, Status status = Status::Neutral,
               float seconds = 4.f);
// Draws the active toasts stacked from `corner` of `area` (screen coordinates), on the
// foreground draw list so they overlay all windows. Expires toasts using io.DeltaTime.
void DrawToasts(const ImRect& area, ToastCorner corner = ToastCorner::BottomRight);

} // namespace solace
