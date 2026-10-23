-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Loaded with no match. Saying a line answers that chat is not connected,
-- and a bad argument still raises.
local hooks = {}

function hooks.fits()
    local ok, reason = brain.say("hello", "allies")
    if ok ~= false or reason ~= "no_chat" then
        return "say"
    end
    local raised, err = pcall(function()
        brain.say(1)
    end)
    if raised then
        return "no raise"
    end
    if string.find(tostring(err), "say expects text", 1, true) == nil then
        return "raise"
    end
    return true
end

return hooks
