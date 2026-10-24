-- follow.lua -- a lamp that orbits its object, and sparks where it lands.
--
-- `self` is the name of the instance this script is attached to. The engine calls onStart once, onUpdate every frame
-- before physics, onContact when a body touches another, and onDestroy when the scene goes away. ukiyo.* is the whole
-- API; Scripts/.ukiyo/lua/ukiyo.lua describes it to any editor with LuaLS.

local SPEED = 2.5          -- radians per second
local RADIUS = 1.8         -- metres from the object
local LAMP = "Lamp"

local angle = 0.0

local function orbit(x, z, at)
    return x + RADIUS * math.cos(at), z + RADIUS * math.sin(at)
end

function onStart()
    ukiyo.log("follow.lua: " .. self .. " is awake at t = " .. string.format("%.2f", ukiyo.time()))
end

function onUpdate(dt)
    angle = (angle + dt * SPEED) % (2.0 * math.pi)
    local x, y, z = ukiyo.getPosition(self)
    local lx, lz = orbit(x, z, angle)
    ukiyo.setPosition(LAMP, lx, y + 2.4, lz)

    -- Hold Use to drop a burst of particles under the lamp.
    if ukiyo.actionDown("Use") then
        local hit, hx, hy, hz = ukiyo.raycast(lx, y + 2.4, lz, 0.0, -1.0, 0.0, 6.0)
        if hit then ukiyo.spawnEffect(hx, hy, hz, 24) end
    end
end

function onContact(contact)
    if contact.a ~= self and contact.b ~= self then return end
    if contact.kind ~= "added" then return end
    local p = contact.point
    ukiyo.log(string.format("%s touched at %.2f, %.2f, %.2f", self, p.x, p.y, p.z))
end

function onDestroy()
    -- Lamps are other instances: putting it back is the polite thing to do.
    ukiyo.log("follow.lua: " .. self .. " is going away")
end
