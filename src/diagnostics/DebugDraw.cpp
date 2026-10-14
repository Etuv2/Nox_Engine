#include "DebugDraw.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <mutex>

namespace DebugDraw {
namespace {

std::mutex g_mutex;
std::vector<Item> g_items;

constexpr int kCircleSegments = 32;

ImU32 ToImU32(uint32_t c) {
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
}

void Push(Item item) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_items.size() >= kMaxItems) return;
    g_items.push_back(std::move(item));
}

Item MakeItem(Kind kind, uint32_t color, float duration) {
    Item item;
    item.kind = kind;
    item.color = color;
    const float d = std::isfinite(duration) ? duration : 0.0f;
    item.timed = d > 0.0f;
    item.remaining = d;
    return item;
}

} // namespace

void AddLine(const glm::vec3& a, const glm::vec3& b, uint32_t color, float duration) {
    Item item = MakeItem(Kind::Line, color, duration);
    item.a = a;
    item.b = b;
    Push(std::move(item));
}

void AddBox(const glm::vec3& center, const glm::vec3& halfExtents, uint32_t color, float duration) {
    Item item = MakeItem(Kind::Box, color, duration);
    item.a = center;
    item.b = glm::abs(halfExtents);
    Push(std::move(item));
}

void AddSphere(const glm::vec3& center, float radius, uint32_t color, float duration) {
    Item item = MakeItem(Kind::Sphere, color, duration);
    item.a = center;
    item.radius = std::isfinite(radius) ? std::fabs(radius) : 0.0f;
    Push(std::move(item));
}

void AddText(const glm::vec3& position, const std::string& text, uint32_t color, float duration) {
    Item item = MakeItem(Kind::Text, color, duration);
    item.a = position;
    item.text = text;
    Push(std::move(item));
}

void Age(float dt) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const float step = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    std::vector<Item> kept;
    kept.reserve(g_items.size());
    for (Item& item : g_items) {
        if (!item.timed) continue;   // this-frame items have been drawn already
        item.remaining -= step;
        if (item.remaining > 0.0f) kept.push_back(std::move(item));
    }
    g_items.swap(kept);
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_items.clear();
}

size_t Count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_items.size();
}

std::vector<Item> Items() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_items;
}

std::vector<Segment> Segments(const Item& item) {
    std::vector<Segment> out;
    switch (item.kind) {
    case Kind::Line:
        out.push_back({ item.a, item.b });
        break;
    case Kind::Box: {
        const glm::vec3 c = item.a;
        const glm::vec3 h = item.b;
        glm::vec3 corner[8];
        for (int i = 0; i < 8; ++i) {
            corner[i] = c + glm::vec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        }
        for (int i = 0; i < 8; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                const int bit = 1 << axis;
                if (i & bit) continue;
                out.push_back({ corner[i], corner[i | bit] });
            }
        }
        break;
    }
    case Kind::Sphere: {
        for (int plane = 0; plane < 3; ++plane) {
            glm::vec3 prev{0.0f};
            for (int s = 0; s <= kCircleSegments; ++s) {
                const float t = 6.2831853f * static_cast<float>(s) / static_cast<float>(kCircleSegments);
                const float u = std::cos(t) * item.radius;
                const float v = std::sin(t) * item.radius;
                glm::vec3 p = item.a;
                if (plane == 0) p += glm::vec3(u, v, 0.0f);
                else if (plane == 1) p += glm::vec3(u, 0.0f, v);
                else p += glm::vec3(0.0f, u, v);
                if (s > 0) out.push_back({ prev, p });
                prev = p;
            }
        }
        break;
    }
    case Kind::Text:
        break;
    }
    return out;
}

void Draw(ImDrawList* drawList, const Projector& project) {
    if (!drawList || !project) return;
    const std::vector<Item> items = Items();
    for (const Item& item : items) {
        const ImU32 col = ToImU32(item.color);
        if (item.kind == Kind::Text) {
            ImVec2 s;
            if (project(item.a, s)) drawList->AddText(s, col, item.text.c_str());
            continue;
        }
        for (const Segment& seg : Segments(item)) {
            ImVec2 sa, sb;
            if (project(seg.a, sa) && project(seg.b, sb)) drawList->AddLine(sa, sb, col, 1.5f);
        }
    }
}

bool ProjectView(const glm::mat4& view, float fovDegrees, const ImVec2& rectPos, const ImVec2& rectSize,
    const glm::vec3& world, ImVec2& out) {
    if (rectSize.x < 1.0f || rectSize.y < 1.0f) return false;
    const glm::mat4 proj = glm::perspective(glm::radians(fovDegrees), rectSize.x / rectSize.y, 0.05f, 5000.0f);
    const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
    if (clip.w <= 0.05f) return false;
    const glm::vec3 n = glm::vec3(clip) / clip.w;
    out = ImVec2(rectPos.x + (n.x * 0.5f + 0.5f) * rectSize.x, rectPos.y + (0.5f - n.y * 0.5f) * rectSize.y);
    return true;
}

} // namespace DebugDraw
