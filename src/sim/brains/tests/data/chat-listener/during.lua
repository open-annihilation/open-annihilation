-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Says a line from onTick, so a test can hear another line while the hooks run.
local hooks = {}
log = {}

function hooks.fits()
    return true
end

function hooks.start()
    return true
end

function hooks.onChat(from, text, to)
    log[#log + 1] = "chat " .. tostring(from) .. " " .. tostring(#text) .. " " .. to .. " " .. text
end

function hooks.onTick(tick)
    log[#log + 1] = "tick " .. tostring(tick)
    brain.say("during")
end

return hooks
