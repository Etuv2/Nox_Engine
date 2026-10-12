-- Shared 2D geometry and angle helpers for the race, camera and autopilot modules.
-- Positions are world metres on the XZ plane. Heading 0 = +Z, growing toward +X (native convention).
local M = {}

local pi = math.pi

-- Rounds a number to float32 (the native game keeps timers and accumulators in float). Use it where a
-- float timer decides a discrete event, so the schedule matches the native frame for frame.
local spack, sunpack = string.pack, string.unpack
function M.f32(x) return (sunpack("<f", spack("<f", x))) end

function M.wrap(a)
    while a > pi do a = a - 2.0 * pi end
    while a < -pi do a = a + 2.0 * pi end
    return a
end

function M.clamp(x, lo, hi)
    if x < lo then return lo end
    if x > hi then return hi end
    return x
end

function M.approach(cur, target, rate_dt)
    if cur < target then return math.min(cur + rate_dt, target) end
    return math.max(cur - rate_dt, target)
end

-- Proper segment intersection (the swept car path against a gate line).
local function cross(ax, az, bx, bz, cx, cz)
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax)
end

function M.segments_intersect(p0x, p0z, p1x, p1z, q0x, q0z, q1x, q1z)
    local d1 = cross(q0x, q0z, q1x, q1z, p0x, p0z)
    local d2 = cross(q0x, q0z, q1x, q1z, p1x, p1z)
    local d3 = cross(p0x, p0z, p1x, p1z, q0x, q0z)
    local d4 = cross(p0x, p0z, p1x, p1z, q1x, q1z)
    return ((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0))
end

-- Reads an authored point: [x, z] or [x, y, z] (scene convention), or {x=, z=, y=}.
-- Returns x, z, y, has_y.
function M.read_point(v)
    if type(v) ~= "table" then return nil end
    if v.x ~= nil then return v.x, v.z, v.y, v.y ~= nil end
    if #v == 2 then return v[1], v[2], nil, false end
    if #v >= 3 then return v[1], v[3], v[2], true end
    return nil
end

-- Heading (radians) of the direction (dx, dz).
function M.heading_of(dx, dz)
    return math.atan(dx, dz)
end

function M.deg(r) return r * 180.0 / pi end
function M.rad(d) return d * pi / 180.0 end

return M
