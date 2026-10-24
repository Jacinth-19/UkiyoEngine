-- Afterglow: a dusk-village lantern adventure.
--
-- Every lantern that still burns at dusk was lit by someone else; the ones that went out are yours. Walk to an
-- extinguished lantern and it remembers — sparks, a chime, the flame coming back, the path brightening, the run
-- saved. Four of the stone lanterns and all six paper ones start dark: ten relights, and the gate answers.
--
-- Everything here is proximity-driven on purpose. The game has one verb and one thumb: no interact key to bind,
-- nothing for a touch screen to miss. Progress writes itself to save slot 0 the moment anything changes, so a
-- player who closes the lid mid-walk opens where they stood.

local musicPath = "assets/music/dusk_loop.wav"
local sparkPath = "assets/audio/spark.wav"
local bellPath = "assets/audio/bell.wav"

-- id, instance, light: the ten relights. Light states come from the scene, never from this table — a level edit
-- that lights "Stone F" at dusk makes it not-a-target by itself.
local targets = {
    { id = "stone-B", inst = "Stone B", light = "Stone B", kind = "stone" },
    { id = "stone-D", inst = "Stone D", light = "Stone D", kind = "stone" },
    { id = "stone-F", inst = "Stone F", light = "Stone F", kind = "stone" },
    { id = "stone-H", inst = "Stone H", light = "Stone H", kind = "stone" },
    { id = "flame-1", inst = "Lantern 1", light = "Flame 1", kind = "flame" },
    { id = "flame-2", inst = "Lantern 2", light = "Flame 2", kind = "flame" },
    { id = "flame-3", inst = "Lantern 3", light = "Flame 3", kind = "flame" },
    { id = "flame-4", inst = "Lantern 4", light = "Flame 4", kind = "flame" },
    { id = "flame-5", inst = "Lantern 5", light = "Flame 5", kind = "flame" },
    { id = "flame-6", inst = "Lantern 6", light = "Flame 6", kind = "flame" },
}

local total = #targets                -- four stones (B, D, F, H) + six paper flames — whatever the table holds

local dark = {}         -- targets still out, refreshed once at start
local near              -- the cold target the hero is standing next to, when one is
local lit = 0
local complete = false
local elapsed = 0.0
local flickerPhase = {} -- one seed per paper flame so they never flicker in step

local function hud()    -- near is an upvalue: declared above so the prompt line and onUpdate share one variable
    if complete then
        ukiyo.hud(ukiyo.tr("complete", "The village remembers its light."))
    else
        local line = ukiyo.tr("progress", "Lanterns %d/%d"):format(lit, total)
        -- The prompt borrows the same line while the hero is close to something cold. One verb, one line.
        if near ~= nil then
            line = line .. "  ·  " .. ukiyo.tr("near", "the flame is close — walk in")
        end
        ukiyo.hud(line)
    end
end

local function lightTarget(t, silent)
    if ukiyo.lightEnabled(t.light) then return false end
    ukiyo.setLightEnabled(t.light, true)
    if not silent then
        local p = ukiyo.getPosition(t.inst)
        ukiyo.spawnEffect(p.x, p.y + 0.4, p.z, 14)
        ukiyo.playSound(sparkPath, t.inst)
    end
    if t.kind == "flame" then
        flickerPhase[#flickerPhase + 1] = (lit * 1.7) % 6.283
    end
    return true
end

function onStart()
    -- Continue before first light: a slot-0 read replaces whatever the scene said, so a relit lantern from last
    -- session stays relit even if the editor's copy of the scene disagrees.
    if ukiyo.saveSlotExists(0) and ukiyo.saveRead(0) then end
    for _, t in ipairs(targets) do
        if ukiyo.saveGet("lit." .. t.id) == true then
            ukiyo.setLightEnabled(t.light, true)
        end
    end
    for _, t in ipairs(targets) do
        if not ukiyo.lightEnabled(t.light) then
            dark[#dark + 1] = t
        else
            lit = lit + 1
            if t.kind == "flame" then
                flickerPhase[#flickerPhase + 1] = (lit * 1.7) % 6.283
            end
        end
    end
    if ukiyo.saveGet("complete") == true then
        complete = true
        lit = total
    end
    ukiyo.playMusic(musicPath, true, 2.0)
    hud()
end

function onUpdate(dt)
    elapsed = elapsed + dt
    if complete then return end

    -- The one interaction: proximity. Within reach, a dark lantern comes back and the run saves itself.
    near = nil
    local hero = ukiyo.getPosition("Hero")
    for i = 1, #dark do
        local t = dark[i]
        local p = ukiyo.getPosition(t.inst)
        local dx, dz = p.x - hero.x, p.z - hero.z
        local d2 = dx * dx + dz * dz
        if d2 < 9.0 then
            near = t
            if d2 < 2.6 then
                table.remove(dark, i)
                if lightTarget(t, false) then
                    lit = lit + 1
                    ukiyo.saveSet("lit." .. t.id, true)
                    ukiyo.saveSet("lanterns", lit)
                    ukiyo.saveWrite(0)
                end
                if lit >= total then
                    complete = true
                    ukiyo.saveSet("complete", true)
                    ukiyo.saveWrite(0)
                    local gate = ukiyo.getPosition("Gate North")
                    ukiyo.spawnEffect(gate.x, gate.y + 3.2, gate.z, 40)
                    ukiyo.playSound(bellPath, "Gate North")
                    ukiyo.stopMusic(3.0)
                end
                near = nil
                hud()
                break
            end
        end
    end

    -- A lit paper flame is a flame: a small, own-rhythm tremble in its intensity. Stones hold steady.
    local k = 0
    for _, t in ipairs(targets) do
        if t.kind == "flame" and ukiyo.lightEnabled(t.light) then
            k = k + 1
            local ph = flickerPhase[k] or 0.0
            ukiyo.setLightIntensity(t.light, 2.8 + 0.3 * math.sin(elapsed * 6.3 + ph)
                                               + 0.18 * math.sin(elapsed * 13.7 + ph * 2.0))
        end
    end
end
