#pragma once
// Immediate world-space debug drawing (engine service, game-agnostic): lines, boxes, spheres and text labels.
//
// Scripts and native code queue primitives during Update. Core draws the queue after the scene every frame, in the
// editor viewport and in play mode, through the ImGui draw list (the same path the editor viewport overlay uses), so
// no renderer is involved. Colours are 0xAARRGGBB, like ed.line3.
//
// Lifetime: duration <= 0 means "this frame only"; a positive duration is in real seconds (unscaled, so debug lines
// keep their length while the game time scale is changed). Expired items are removed by Age(), which Core calls at
// the start of each frame.
#include <IMGUI/imgui.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace DebugDraw {

enum class Kind : uint8_t { Line, Box, Sphere, Text };

struct Item {
    Kind kind = Kind::Line;
    glm::vec3 a{0.0f};       // line start, box center, sphere center, text anchor
    glm::vec3 b{0.0f};       // line end, box half extents
    float radius = 0.0f;     // sphere radius
    uint32_t color = 0xFFFFFFFFu;
    float remaining = 0.0f;  // seconds left (timed items)
    bool timed = false;
    std::string text;
};

// World point to ImGui screen position; false when the point is not drawable (behind the camera).
using Projector = std::function<bool(const glm::vec3& world, ImVec2& out)>;

// Upper bound on queued items; additions beyond it are dropped.
constexpr size_t kMaxItems = 4096;

void AddLine(const glm::vec3& a, const glm::vec3& b, uint32_t color, float duration);
void AddBox(const glm::vec3& center, const glm::vec3& halfExtents, uint32_t color, float duration);
void AddSphere(const glm::vec3& center, float radius, uint32_t color, float duration);
void AddText(const glm::vec3& position, const std::string& text, uint32_t color, float duration);

// Removes this-frame items and ages timed items by dt seconds (dropping those that reach zero).
void Age(float dt);
void Clear();
size_t Count();
// Snapshot of the queue (for tests and tooling).
std::vector<Item> Items();

// World-space line segments of an item: a line is one, a box its 12 edges, a sphere three great circles of 32
// segments each. Text has none.
struct Segment {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
};
std::vector<Segment> Segments(const Item& item);

// Draws every queued item through the projector. Lines and text outside the projection are skipped; a sphere or
// box edge is skipped when either end is behind the camera.
void Draw(ImDrawList* drawList, const Projector& project);

// Projection used by the engine views: perspective with the camera's vertical FOV, mapped into the rectangle.
// view is the camera view matrix; false when the point is behind the near plane or the rectangle is empty.
bool ProjectView(const glm::mat4& view, float fovDegrees, const ImVec2& rectPos, const ImVec2& rectSize,
    const glm::vec3& world, ImVec2& out);

} // namespace DebugDraw
