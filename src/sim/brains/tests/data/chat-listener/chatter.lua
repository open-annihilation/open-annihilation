-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Says one line on every tick. The say goes through the test hook and does
-- not touch the match.
local hooks = {}

function hooks.fits()
    return true
end

function hooks.start()
    return true
end

function hooks.onTick()
    brain.say("holding", "all")
end

return hooks
