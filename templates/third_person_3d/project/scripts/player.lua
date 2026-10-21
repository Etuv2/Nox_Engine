-- Third-person controller and follow camera (docs/lua_api.md: "Physics", "Camera", "Input").
-- Horizontal motion is a script displacement (CharacterController.move). Gravity is engine-integrated through
-- velocity.y, so the jump only sets velocity.y while the controller reports is_on_floor.
-- The camera is the module camera: it is set from the player position every frame.

local CONFIG = {
  speed = 6.0,            -- metres per second on the ground
  jump_speed = 7.0,       -- initial vertical velocity of a jump
  orbit_speed = 2.2,      -- radians per second at full look input
  camera_distance = 7.0,
  camera_height = 3.4,
  focus_height = 1.2,     -- the camera looks at this height above the feet
  respawn_y = -10.0,      -- falling below this teleports the player back to the start
}

local controller = { yaw = 0.0 }

-- Yaw convention: yaw 0 looks along -Z, a positive yaw turns to the left.
local function forward_of(yaw) return -math.sin(yaw), -math.cos(yaw) end
local function right_of(yaw) return math.cos(yaw), -math.sin(yaw) end

nox.on("begin_play", function(ctx)
  controller.yaw = 0.0
end)

nox.system{
  name = "third_person_controller",
  order = 0,
  update = function(ctx, dt)
    local player = ctx:find("Player")
    if not player then return end

    controller.yaw = controller.yaw + ctx:axis("look") * CONFIG.orbit_speed * dt

    -- The left stick reports lefty as negative when pushed up, so the pad axis is subtracted.
    local forward = ctx:axis("move_forward") - ctx:axis("move_forward_pad")
    local strafe = ctx:axis("move_strafe")
    local fx, fz = forward_of(controller.yaw)
    local rx, rz = right_of(controller.yaw)
    local mx = rx * strafe + fx * forward
    local mz = rz * strafe + fz * forward
    local length = math.sqrt(mx * mx + mz * mz)
    if length > 1.0 then
      mx, mz = mx / length, mz / length
      length = 1.0
    end

    local cc = ctx:component(player, "CharacterController")
    if length > 0.05 then
      -- Face the movement direction (Transform rotation is Euler degrees; the body is a child).
      ctx:component(player, "Transform").rotation = { x = 0.0, y = math.deg(math.atan(mx, mz)), z = 0.0 }
    end
    if ctx:action_pressed("jump") and cc.is_on_floor then
      cc.velocity = { x = 0.0, y = CONFIG.jump_speed, z = 0.0 }
    end
    cc:move({ x = mx * CONFIG.speed * dt, y = 0.0, z = mz * CONFIG.speed * dt })

    local px, py, pz = ctx:position(player)
    if py < CONFIG.respawn_y then
      ctx:teleport(player, 0.0, 1.0, 0.0)
      cc.velocity = { x = 0.0, y = 0.0, z = 0.0 }
    end
  end,
}

nox.system{
  name = "third_person_camera",
  order = 10,
  update = function(ctx, dt)
    local player = ctx:find("Player")
    if not player then return end
    local px, py, pz = ctx:position(player)
    local fx, fz = forward_of(controller.yaw)
    local tx, ty, tz = px, py + CONFIG.focus_height, pz
    local cx = tx - fx * CONFIG.camera_distance
    local cz = tz - fz * CONFIG.camera_distance
    ctx:camera_look_at(cx, py + CONFIG.camera_height, cz, tx, ty, tz)
    ctx:camera_set_fov(60.0)
  end,
}
