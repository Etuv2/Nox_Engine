// Adapted from Solace src/ui/foundation/svg.cpp (MIT, Copyright (c) 2026 Pondot).
// Only the include paths and a <cstdlib> include were changed. Declarations live in SolaceInternal.h.
#include "editor/solace/SolaceInternal.h"

#include <cstdlib>

#include <map>
#include <string>

namespace solace::svg
{
namespace
{

constexpr int k_arc_segments_per_turn = 96;
constexpr float k_min_step = 0.35f;

struct cursor
{
    const char* p;
    bool valid = true;

    void skip()
    {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
    }

    bool at_command() const
    {
        const char c = *p;
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    bool more()
    {
        skip();
        return *p != '\0' && !at_command();
    }

    float number()
    {
        skip();
        char* end = nullptr;
        const float v = strtof(p, &end);
        if (end == p)
        {
            valid = false;
            return 0.f;
        }
        p = end;
        return v;
    }

    float flag()
    {
        skip();
        if (*p == '0' || *p == '1')
        {
            const float v = (*p == '1') ? 1.f : 0.f;
            p++;
            return v;
        }
        return number();
    }
};

void push_point(sub_path& sp, const ImVec2& pt)
{
    if (sp.points.Size > 0)
    {
        const ImVec2 last = sp.points.back();
        if (ImFabs(last.x - pt.x) < 1e-5f && ImFabs(last.y - pt.y) < 1e-5f)
            return;
    }
    sp.points.push_back(pt);
}

void arc_to(sub_path& sp, const ImVec2& from, const ImVec2& to, float rx, float ry, float x_rot_deg,
            bool large_arc, bool sweep)
{
    if (rx == 0.f || ry == 0.f)
    {
        push_point(sp, to);
        return;
    }

    rx = ImFabs(rx);
    ry = ImFabs(ry);

    const float phi = x_rot_deg * (IM_PI / 180.f);
    const float cos_phi = ImCos(phi);
    const float sin_phi = ImSin(phi);

    const float dx2 = (from.x - to.x) * 0.5f;
    const float dy2 = (from.y - to.y) * 0.5f;

    const float x1p = cos_phi * dx2 + sin_phi * dy2;
    const float y1p = -sin_phi * dx2 + cos_phi * dy2;

    const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.f)
    {
        const float s = ImSqrt(lambda);
        rx *= s;
        ry *= s;
    }

    const float rx2 = rx * rx, ry2 = ry * ry;
    const float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float factor = den > 0.f ? ImSqrt(ImMax(num, 0.f) / den) : 0.f;
    if (large_arc == sweep)
        factor = -factor;

    const float cxp = factor * (rx * y1p / ry);
    const float cyp = factor * -(ry * x1p / rx);

    const ImVec2 centre(cos_phi * cxp - sin_phi * cyp + (from.x + to.x) * 0.5f,
                        sin_phi * cxp + cos_phi * cyp + (from.y + to.y) * 0.5f);

    auto angle_of = [&](float x, float y) { return atan2f((y - cyp) / ry, (x - cxp) / rx); };

    const float theta1 = angle_of(x1p, y1p);
    float delta = angle_of(-x1p, -y1p) - theta1;

    if (!sweep && delta > 0.f)
        delta -= 2.f * IM_PI;
    else if (sweep && delta < 0.f)
        delta += 2.f * IM_PI;

    const int segments =
        ImMax(2, (int)ceilf(ImFabs(delta) / (2.f * IM_PI) * k_arc_segments_per_turn));
    for (int i = 1; i <= segments; i++)
    {
        const float theta = theta1 + delta * ((float)i / (float)segments);
        const float ex = rx * ImCos(theta);
        const float ey = ry * ImSin(theta);
        push_point(sp, ImVec2(cos_phi * ex - sin_phi * ey + centre.x,
                              sin_phi * ex + cos_phi * ey + centre.y));
    }
}

void cubic_to(sub_path& sp, const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3)
{

    const float hull = ImSqrt(ImLengthSqr(ImVec2(p1.x - p0.x, p1.y - p0.y))) +
                       ImSqrt(ImLengthSqr(ImVec2(p2.x - p1.x, p2.y - p1.y))) +
                       ImSqrt(ImLengthSqr(ImVec2(p3.x - p2.x, p3.y - p2.y)));

    const int steps = ImClamp((int)ceilf(hull / k_min_step), 2, 512);
    for (int i = 1; i <= steps; i++)
    {
        const float t = (float)i / (float)steps;
        const float u = 1.f - t;
        const float w0 = u * u * u;
        const float w1 = 3.f * u * u * t;
        const float w2 = 3.f * u * t * t;
        const float w3 = t * t * t;
        push_point(sp, ImVec2(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                              w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y));
    }
}

void quadratic_to(sub_path& sp, const ImVec2& p0, const ImVec2& p1, const ImVec2& p2)
{

    const ImVec2 c1(p0.x + (2.f / 3.f) * (p1.x - p0.x), p0.y + (2.f / 3.f) * (p1.y - p0.y));
    const ImVec2 c2(p2.x + (2.f / 3.f) * (p1.x - p2.x), p2.y + (2.f / 3.f) * (p1.y - p2.y));
    cubic_to(sp, p0, c1, c2, p2);
}

void line_to(sub_path& sp, const ImVec2& from, const ImVec2& to)
{
    const float dist = ImSqrt(ImLengthSqr(ImVec2(to.x - from.x, to.y - from.y)));
    const int steps = ImMax(1, (int)ceilf(dist / k_min_step));
    for (int i = 1; i <= steps; i++)
        push_point(sp, ImLerp(from, to, (float)i / (float)steps));
}

void measure(shape& s)
{
    s.length = 0.f;
    for (sub_path& sp : s.subs)
    {
        sp.length = 0.f;
        sp.cumulative.clear();
        sp.cumulative.push_back(0.f);
        for (int i = 1; i < sp.points.Size; i++)
        {
            const ImVec2 d(sp.points[i].x - sp.points[i - 1].x,
                           sp.points[i].y - sp.points[i - 1].y);
            sp.length += ImSqrt(ImLengthSqr(d));
            sp.cumulative.push_back(sp.length);
        }
        s.length += sp.length;
    }
}

shape build_path(const char* d)
{
    shape out;
    cursor c{d};

    ImVec2 pen(0, 0);
    ImVec2 start(0, 0);
    char command = 0;

    ImVec2 last_cubic_ctrl(0, 0);
    ImVec2 last_quad_ctrl(0, 0);
    bool had_cubic = false, had_quad = false;

    c.skip();
    while (*c.p)
    {
        const char* iteration_start = c.p;
        if (c.at_command())
        {
            command = *c.p;
            c.p++;
        }
        else if (command == 'M')
            command = 'L';
        else if (command == 'm')
            command = 'l';

        switch (command)
        {
        case 'M':
        case 'm':
        {
            const float x = c.number();
            const float y = c.number();
            pen = (command == 'm') ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            start = pen;
            out.subs.push_back(sub_path());
            push_point(out.subs.back(), pen);
            break;
        }
        case 'L':
        case 'l':
        {
            const float x = c.number();
            const float y = c.number();
            const ImVec2 next = (command == 'l') ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                line_to(out.subs.back(), pen, next);
            pen = next;
            break;
        }
        case 'H':
        case 'h':
        {
            const float x = c.number();
            const ImVec2 next = (command == 'h') ? ImVec2(pen.x + x, pen.y) : ImVec2(x, pen.y);
            if (!out.subs.empty())
                line_to(out.subs.back(), pen, next);
            pen = next;
            break;
        }
        case 'V':
        case 'v':
        {
            const float y = c.number();
            const ImVec2 next = (command == 'v') ? ImVec2(pen.x, pen.y + y) : ImVec2(pen.x, y);
            if (!out.subs.empty())
                line_to(out.subs.back(), pen, next);
            pen = next;
            break;
        }
        case 'C':
        case 'c':
        {
            const float x1 = c.number(), y1 = c.number();
            const float x2 = c.number(), y2 = c.number();
            const float x = c.number(), y = c.number();
            const bool rel = (command == 'c');
            const ImVec2 c1 = rel ? ImVec2(pen.x + x1, pen.y + y1) : ImVec2(x1, y1);
            const ImVec2 c2 = rel ? ImVec2(pen.x + x2, pen.y + y2) : ImVec2(x2, y2);
            const ImVec2 next = rel ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                cubic_to(out.subs.back(), pen, c1, c2, next);
            pen = next;
            last_cubic_ctrl = c2;
            had_cubic = true;
            had_quad = false;
            break;
        }
        case 'S':
        case 's':
        {
            const float x2 = c.number(), y2 = c.number();
            const float x = c.number(), y = c.number();
            const bool rel = (command == 's');
            const ImVec2 c1 =
                had_cubic ? ImVec2(2.f * pen.x - last_cubic_ctrl.x, 2.f * pen.y - last_cubic_ctrl.y)
                          : pen;
            const ImVec2 c2 = rel ? ImVec2(pen.x + x2, pen.y + y2) : ImVec2(x2, y2);
            const ImVec2 next = rel ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                cubic_to(out.subs.back(), pen, c1, c2, next);
            pen = next;
            last_cubic_ctrl = c2;
            had_cubic = true;
            had_quad = false;
            break;
        }
        case 'Q':
        case 'q':
        {
            const float x1 = c.number(), y1 = c.number();
            const float x = c.number(), y = c.number();
            const bool rel = (command == 'q');
            const ImVec2 c1 = rel ? ImVec2(pen.x + x1, pen.y + y1) : ImVec2(x1, y1);
            const ImVec2 next = rel ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                quadratic_to(out.subs.back(), pen, c1, next);
            pen = next;
            last_quad_ctrl = c1;
            had_quad = true;
            had_cubic = false;
            break;
        }
        case 'T':
        case 't':
        {
            const float x = c.number(), y = c.number();
            const bool rel = (command == 't');
            const ImVec2 c1 =
                had_quad ? ImVec2(2.f * pen.x - last_quad_ctrl.x, 2.f * pen.y - last_quad_ctrl.y)
                         : pen;
            const ImVec2 next = rel ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                quadratic_to(out.subs.back(), pen, c1, next);
            pen = next;
            last_quad_ctrl = c1;
            had_quad = true;
            had_cubic = false;
            break;
        }
        case 'A':
        case 'a':
        {
            const float rx = c.number();
            const float ry = c.number();
            const float rot = c.number();
            const bool large = c.flag() != 0.f;
            const bool sweep = c.flag() != 0.f;
            const float x = c.number();
            const float y = c.number();
            const ImVec2 next = (command == 'a') ? ImVec2(pen.x + x, pen.y + y) : ImVec2(x, y);
            if (!out.subs.empty())
                arc_to(out.subs.back(), pen, next, rx, ry, rot, large, sweep);
            pen = next;
            had_cubic = had_quad = false;
            break;
        }
        case 'Z':
        case 'z':
        {
            if (!out.subs.empty())
            {
                line_to(out.subs.back(), pen, start);
                out.subs.back().closed = true;
            }
            pen = start;
            break;
        }
        default:
            c.number();
            break;
        }

        if (!c.valid)
            return {};

        if (command != 'Z' && command != 'z')
        {
            if (!c.more())
            {
                c.skip();
                if (!*c.p)
                    break;
            }
        }
        else
        {
            c.skip();
        }

        if (c.p == iteration_start)
            return {};
    }

    measure(out);
    return out;
}

shape build_circle(float cx, float cy, float r)
{
    shape out;
    out.subs.push_back(sub_path());
    sub_path& sp = out.subs.back();
    sp.closed = true;

    const int segments = k_arc_segments_per_turn;
    for (int i = 0; i <= segments; i++)
    {
        const float a = (float)i / (float)segments * 2.f * IM_PI;
        sp.points.push_back(ImVec2(cx + ImCos(a) * r, cy + ImSin(a) * r));
    }

    measure(out);
    return out;
}

shape build_rounded_rect(float x, float y, float w, float h, float rx)
{
    shape out;
    out.subs.push_back(sub_path());
    sub_path& sp = out.subs.back();
    sp.closed = true;

    rx = ImMin(rx, ImMin(w, h) * 0.5f);

    const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    const int corner_segments = k_arc_segments_per_turn / 4;

    auto corner = [&](float ccx, float ccy, float from, float to)
    {
        for (int i = 0; i <= corner_segments; i++)
        {
            const float a = ImLerp(from, to, (float)i / (float)corner_segments);
            push_point(sp, ImVec2(ccx + ImCos(a) * rx, ccy + ImSin(a) * rx));
        }
    };

    push_point(sp, ImVec2(x0 + rx, y0));
    line_to(sp, ImVec2(x0 + rx, y0), ImVec2(x1 - rx, y0));
    corner(x1 - rx, y0 + rx, -IM_PI * 0.5f, 0.f);
    line_to(sp, ImVec2(x1, y0 + rx), ImVec2(x1, y1 - rx));
    corner(x1 - rx, y1 - rx, 0.f, IM_PI * 0.5f);
    line_to(sp, ImVec2(x1 - rx, y1), ImVec2(x0 + rx, y1));
    corner(x0 + rx, y1 - rx, IM_PI * 0.5f, IM_PI);
    line_to(sp, ImVec2(x0, y1 - rx), ImVec2(x0, y0 + rx));
    corner(x0 + rx, y0 + rx, IM_PI, IM_PI * 1.5f);

    measure(out);
    return out;
}

std::map<std::string, shape>& cache()
{
    static std::map<std::string, shape> store;
    return store;
}
} // namespace

const shape& path(const char* d)
{
    static const shape empty;
    if (!d)
        return empty;

    auto& store = cache();
    auto it = store.find(d);
    if (it != store.end())
        return it->second;

    return store.emplace(d, build_path(d)).first->second;
}

const shape& circle(float cx, float cy, float r)
{
    char key[96];
    ImFormatString(key, IM_ARRAYSIZE(key), "@circle %.4f %.4f %.4f", cx, cy, r);

    auto& store = cache();
    auto it = store.find(key);
    if (it != store.end())
        return it->second;

    return store.emplace(key, build_circle(cx, cy, r)).first->second;
}

const shape& rounded_rect(float x, float y, float w, float h, float rx)
{
    char key[128];
    ImFormatString(key, IM_ARRAYSIZE(key), "@rect %.4f %.4f %.4f %.4f %.4f", x, y, w, h, rx);

    auto& store = cache();
    auto it = store.find(key);
    if (it != store.end())
        return it->second;

    return store.emplace(key, build_rounded_rect(x, y, w, h, rx)).first->second;
}

void stroke(ImDrawList* draw_list, const shape& s, const ImVec2& origin, float scale, ImU32 col,
            float width, float fraction)
{
    if ((col & IM_COL32_A_MASK) == 0 || s.subs.empty())
        return;

    fraction = ImClamp(fraction, 0.f, 1.f);
    if (fraction <= 0.f)
        return;

    float budget = s.length * fraction;

    ImVector<ImVec2> screen;
    for (const sub_path& sp : s.subs)
    {
        if (sp.points.Size < 2 || budget <= 0.f)
            break;

        const bool whole = budget >= sp.length - 1e-4f;
        screen.resize(0);

        if (whole)
        {
            for (const ImVec2& p : sp.points)
                screen.push_back(ImVec2(origin.x + p.x * scale, origin.y + p.y * scale));
            budget -= sp.length;
        }
        else
        {
            for (int i = 0; i < sp.points.Size; i++)
            {
                if (sp.cumulative[i] > budget)
                {

                    const float span = sp.cumulative[i] - sp.cumulative[i - 1];
                    const float t = span > 0.f ? (budget - sp.cumulative[i - 1]) / span : 0.f;
                    const ImVec2 p = ImLerp(sp.points[i - 1], sp.points[i], t);
                    screen.push_back(ImVec2(origin.x + p.x * scale, origin.y + p.y * scale));
                    break;
                }
                screen.push_back(
                    ImVec2(origin.x + sp.points[i].x * scale, origin.y + sp.points[i].y * scale));
            }
            budget = 0.f;
        }

        if (screen.Size < 2)
            continue;

        const float thickness = width * scale;
        // ImGui 1.91 order: (points, count, col, flags, thickness).
        draw_list->AddPolyline(screen.Data, screen.Size, col,
                               sp.closed && whole ? ImDrawFlags_Closed : ImDrawFlags_None,
                               thickness);

        if (!sp.closed || !whole)
        {
            const float radius = thickness * 0.5f;
            draw_list->AddCircleFilled(screen.front(), radius, col, 12);
            draw_list->AddCircleFilled(screen.back(), radius, col, 12);
        }
    }
}
} // namespace solace::svg
