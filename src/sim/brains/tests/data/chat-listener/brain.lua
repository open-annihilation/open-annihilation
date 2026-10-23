-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Records the order of one turn. The name is not `brain`: that would hide
-- the global table.
local hooks = {}
log = {}

local function note(line)
    log[#log + 1] = line
end

function hooks.fits()
    return true
end

function hooks.start()
    return true
end

function hooks.onUnitCreated(unit, builder)
    local builder_text = "nil"
    if builder ~= nil then
        builder_text = string.format("u%d:%d", builder.slot, builder.id)
    end
    note(string.format("created u%d:%d %s", unit.slot, unit.id, builder_text))
end

function hooks.onAllianceChanged(player, allied)
    note("alliance " .. tostring(player) .. " " .. (allied and "true" or "false"))
end

function hooks.onPlayerDefeated(player)
    note("defeated " .. tostring(player))
end

function hooks.onChat(from, text, to)
    note("chat " .. tostring(from) .. " " .. tostring(#text) .. " " .. to .. " " .. text)
end

function hooks.onTick(tick)
    note("tick " .. tostring(tick))
end

return hooks
