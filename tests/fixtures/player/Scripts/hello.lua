-- The fixture's script: proves the player starts a script host and calls its hooks. It must not switch scenes, so the
-- self-test can report the instance list of the start scene.
local frames = 0

function onStart()
  ukiyo.log("fixture script started")
end

function onUpdate(dt)
  frames = frames + 1
  if frames == 60 then ukiyo.log("fixture script ran 60 frames") end
end
