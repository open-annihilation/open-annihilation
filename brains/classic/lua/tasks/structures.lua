-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The structures tick. A metal maker follows the energy on hand. An idle
-- factory queues one weighted pick. A building whose secondary queue is
-- busy is left alone when the factory filter says so.

local build = require("build")
local types = require("types")

local structures = {}

local power_top_byte = 0x42

local function runs_power_toggle(rules, row)
    local filter = rules.factory_tick_filter
    if type(filter) == "table" and filter.power_toggle == "energy-use-32" then
        local bits = types.bits(row.energy_use)
        local top = (bits >> 24) & 0xFF
        if top >= 128 then
            top = top - 256
        end
        return top >= power_top_byte
    end
    return (row.makes_metal or 0) ~= 0
end

local function one_structure(seat, unit)
    if unit == nil or unit.building ~= true or unit.live ~= true then
        return
    end
    local filter = seat.rules.factory_tick_filter
    if type(filter) == "table" and filter.skip_busy and unit:has_background_order() == true then
        return
    end
    local row = seat.types[unit.type_index]
    if type(row) ~= "table" then
        return
    end
    local me = seat.me
    if runs_power_toggle(seat.rules, row) then
        local on = false
        if me.energy > me.metal + me.metal then
            if not (me.energy_produced - me.energy_requested > 0) then
                return
            end
            if brain.shared_random(5) == 0 then
                return
            end
            on = true
        end
        unit:on(on)
        return
    end
    if (row.build_count or 0) == 0 then
        return
    end
    if unit:head_order() ~= nil then
        return
    end
    local pick = build.pick(seat, unit)
    if pick ~= 0 then
        unit:queue(pick, 1)
    end
end

function structures.run(seat, squad)
    local me = seat.me
    if me == nil or type(me.squad_size) ~= "function" then
        return
    end
    local count = me:squad_size(squad)
    if type(count) ~= "number" then
        return
    end
    for index = 1, count do
        one_structure(seat, me:squad_member(squad, index))
    end
end

return structures
