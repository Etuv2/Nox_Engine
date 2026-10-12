-- Numeric helpers shared by the NFS vehicle controller. Same formulas as the legacy C++ policy
-- (VehicleMath.h): frame-rate independent approach, clamp, mix, angle helpers.
local M = {}

local exp, abs, min, max = math.exp, math.abs, math.min, math.max

function M.clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- rate is already multiplied by dt
function M.approach(current, target, rate)
    return current + (target - current) * (1.0 - exp(-rate))
end

-- glm::mix(x, y, a) = x * (1 - a) + y * a
function M.mix(x, y, a)
    return x * (1.0 - a) + y * a
end

function M.radians(deg)
    return deg * (math.pi / 180.0)
end

function M.sign(v)
    if v < 0 then return -1.0 end
    return 1.0
end

function M.wrap_angle(a)
    local pi = math.pi
    while a > pi do a = a - 2 * pi end
    while a < -pi do a = a + 2 * pi end
    return a
end

M.abs = abs
M.min = min
M.max = max

return M
