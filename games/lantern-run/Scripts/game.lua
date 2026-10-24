-- Lantern Run. The first game shipped with Ukiyo, and deliberately small: walk the court, touch each of the five
-- paper lanterns, and the night is yours. Everything it does is the engine's own script API — getPosition, setPosition,
-- spawnEffect, hud — so the game doubles as the example every new project starts from.
--
-- The script sits on an empty instance called "Night" so it has a place in the scene and can be found in the editor.
-- The hero drives itself (the player's character controller reads WASD); this file only decides what a lantern does
-- when the hero reaches it.

local lanterns = { "Lantern 1", "Lantern 2", "Lantern 3", "Lantern 4", "Lantern 5" }
local rest = {}        -- where each lantern floats, so the bob is a sine around a fixed point
local taken = {}
local remaining = #lanterns
local elapsed = 0.0
local finished = false

local function say()
    if finished then
        ukiyo.hud("All five lanterns lit. The court is yours.")
    else
        ukiyo.hud(string.format("Lanterns %d / %d", #lanterns - remaining, #lanterns))
    end
end

function onStart()
    for i, name in ipairs(lanterns) do
        local x, y, z = ukiyo.getPosition(name)
        rest[i] = { x = x, y = y, z = z }
    end
    say()
end

function onUpdate(dt)
    elapsed = elapsed + dt
    local hx, hy, hz = ukiyo.getPosition("Hero")

    for i, name in ipairs(lanterns) do
        if not taken[i] then
            local r = rest[i]
            -- A slow bob, one lantern out of step with the next, so the court never looks frozen.
            ukiyo.setPosition(name, r.x, r.y + math.sin(elapsed * 1.3 + i * 1.7) * 0.12, r.z)

            local x, y, z = ukiyo.getPosition(name)
            local dx, dz = x - hx, z - hz
            if dx * dx + dz * dz < 0.9 and math.abs(y - hy) < 2.2 then
                taken[i] = true
                remaining = remaining - 1
                -- The lantern goes up and out of the court; the burst is what the eye reads as "taken".
                ukiyo.spawnEffect(x, y, z, 48)
                ukiyo.setPosition(name, x, y + 24.0, z)
                if remaining == 0 then
                    finished = true
                    ukiyo.spawnEffect(0.0, 3.4, -5.2, 160)   -- the torii answers
                end
                say()
            end
        end
    end
end

function onSceneLoaded()
    -- The scene is fully built here: every entity exists, so a script can find the others.
    if not finished then say() end
end
