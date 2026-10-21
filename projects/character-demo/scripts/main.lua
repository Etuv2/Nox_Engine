-- Character demo: a capsule player driven by its CharacterController (WASD or arrows to walk, Space to jump) on the
-- mesh level in models/level.glb, with a follow camera. The engine does the collision: the controller slides along
-- walls, climbs the 0.25 m steps, walks up the 30 degree ramp and snaps to the ground. The script only sets the
-- horizontal motion and the jump velocity, and logs the player state every 60 frames.

local SPEED = 4.0        -- m/s on the ground
local JUMP_SPEED = 5.0   -- m/s upward when jumping from the floor
local LOG_EVERY = 60     -- frames between [Character] log lines

local function axis_pair(ctx, name)
    return ctx:axis(name)
end

nox.system({
    name = "character_player",
    order = 10,
    update = function(ctx, dt)
        local player = ctx:find("Player")
        if not player then return end
        local cc = ctx:component(player, "CharacterController")
        if not cc then return end

        -- Horizontal input on the world axes: D/Right is +x, W/Up is -z (away from the camera, which sits on +z).
        local mx = axis_pair(ctx, "move_x")
        local mz = axis_pair(ctx, "move_z")
        local dx = mx * SPEED * dt
        local dz = -mz * SPEED * dt
        if dx ~= 0 or dz ~= 0 then
            cc:move({ x = dx, y = 0, z = dz })
        end

        if ctx:action_pressed("jump") and cc.is_on_floor then
            cc.velocity = { x = 0, y = JUMP_SPEED, z = 0 }
        end

        local x, y, z = ctx:position(player)
        -- Follow camera: 6 m behind the player on +z, looking at a point just above its feet.
        ctx:camera_look_at(x, y + 2.5, z + 6.0, x, y + 1.0, z)

        local frame = ctx:frame()
        if frame % LOG_EVERY == 0 then
            print(string.format("[Character] frame=%d pos=(%.3f, %.3f, %.3f) on_floor=%s",
                frame, x, y, z, tostring(cc.is_on_floor)))
        end
    end,
})
