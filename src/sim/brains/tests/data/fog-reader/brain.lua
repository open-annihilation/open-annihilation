-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Reads what the seat can see. The name is not `brain`: that would hide the global.
local hooks = {}
log = {}
held = nil

local function note(name)
    log[#log + 1] = name
end

function hooks.start()
    return true
end

function hooks.onUnitDamaged()
    note("damaged")
end

function hooks.onRadarContact(contact)
    note("contact")
    held = contact
end

function hooks.onRadarLost(contact)
    note("lost")
    held = contact
end

function hooks.onTick(tick)
    note("tick")
    local _tick = tick
    local seen = me.seen
    local _seen_tick = me.seen_tick
    local radar = me.radar
    local _radar_tick = me.radar_tick
    local sight = me.sight
    local explored = me.explored
    if sight ~= nil and explored ~= nil then
        local width = sight.width
        local height = sight.height
        local _cell = sight.cell_size
        local _explored_width = explored.width
        local _explored_height = explored.height
        local _explored_cell = explored.cell_size
        for cz = 0, height - 1 do
            for cx = 0, width - 1 do
                local _in_sight = sight:at(cx, cz)
                local _mapped = explored:at(cx, cz)
            end
        end
        local _cx, _cz = sight:cell_of(0, 0)
        local _off_x, _off_z = sight:cell_of(-100, 0)
    end
    local _here = me:sees(0, 0)
    local _high = me:sees(0, 0, 64)
    if seen ~= nil and seen[1] ~= nil then
        local _visible = me:sees_unit(seen[1])
    end
    if radar ~= nil and radar[1] ~= nil then
        local _x = radar[1].x
        local _sonar = radar[1].sonar
        local _radar = radar[1].radar
        local _current = radar[1].current
        local _type = radar[1].type
    end
    if me.full_awareness then
        local listed = game.units:list()
        local _own = game.units:list(me.player)
        local count = 0
        game.units:each(function(unit)
            count = count + 1
            local _slot = unit.slot
        end)
        local _near = me:nearest_enemy(0, 0)
        local _wide = me:nearest_enemy(8, 8, {widened = true})
        local _sub = me:nearest_enemy(8, 8, {skip_submerged = true})
        local _both = me:nearest_enemy(8, 8, {widened = true, skip_submerged = true})
        local raw = me:seen_raw()
        local _listed = #listed
        local _raw = #raw
        local _walked = count
    end
end

return hooks
