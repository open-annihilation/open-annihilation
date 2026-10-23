-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- A seat with no onChat. onTick still runs.
local hooks = {}
log = {}

function hooks.fits()
    return true
end

function hooks.start()
    return true
end

function hooks.onTick(tick)
    log[#log + 1] = "tick " .. tostring(tick)
end

return hooks
