-- Road polylines for the presentation (minimap and world map), cached per nox.roads handle.
--
-- nox.roads(name) is declared in project.json ("roads"); edge_polylines returns one flat
-- {x1, z1, x2, z2, ...} array per edge. Each entry keeps its bounds for per-frame culling.
-- The cache is rebuilt when the handle changes (a reload gives a new handle).

local M = {}

local cache = { handle = nil, lines = {}, bounds = nil }

-- Returns the list of { pts = flat, minx, minz, maxx, maxz } for the handle (empty when nil).
function M.lines(handle)
  if cache.handle ~= handle then
    cache.handle = handle
    cache.lines = {}
    cache.bounds = nil
    if handle then
      local ok, polys = pcall(function() return handle:edge_polylines(4) end)
      if ok and polys then
        local bminx, bminz, bmaxx, bmaxz = math.huge, math.huge, -math.huge, -math.huge
        for _, flat in ipairs(polys) do
          if #flat >= 4 then
            local minx, minz, maxx, maxz = math.huge, math.huge, -math.huge, -math.huge
            for i = 1, #flat - 1, 2 do
              local x, z = flat[i], flat[i + 1]
              if x < minx then minx = x end
              if x > maxx then maxx = x end
              if z < minz then minz = z end
              if z > maxz then maxz = z end
            end
            cache.lines[#cache.lines + 1] = { pts = flat, minx = minx, minz = minz, maxx = maxx, maxz = maxz }
            if minx < bminx then bminx = minx end
            if maxx > bmaxx then bmaxx = maxx end
            if minz < bminz then bminz = minz end
            if maxz > bmaxz then bmaxz = maxz end
          end
        end
        if #cache.lines > 0 then cache.bounds = { bminx, bminz, bmaxx, bmaxz } end
      end
    end
  end
  return cache.lines
end

-- Bounds of all lines {minx, minz, maxx, maxz} or nil (after lines() ran for the handle).
function M.bounds(handle)
  M.lines(handle)
  return cache.bounds
end

return M
