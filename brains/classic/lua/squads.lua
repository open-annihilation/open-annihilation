-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Squad sort. Selectable units in the seat's range take standing orders,
-- and a unit still in squad 0 joins the squad its type sorts into.
-- A unit already in a squad, including squad -1, keeps that squad.

local types = require("types")

local squads = {}

local flag_builder = 0x00000040
local flag_can_fly = 0x00000800
local flag_amphibious = 0x00200000
local ability_capture = 0x00001000
local ability_commander = 0x00040000

local squad_none = 0
local squad_structures = 1
local squad_land_army = 3
local squad_builders = 4
local squad_armed_structures = 5
local squad_navy = 7
local squad_aircraft = 8

local role_energy_high_word = 0x4269
local role_navy_depth = 128

local function assignment_of(rules)
    if type(rules) == "string" then
        return rules
    end
    if type(rules) == "table" and type(rules.squad_assignment) == "string" then
        return rules.squad_assignment
    end
    return "base"
end

-- The float's high 16 bits, read as a signed number.
local function energy_high_word(energy)
    local bits = types.bits(energy)
    local word = (bits >> 16) & 0xFFFF
    if word >= 0x8000 then
        word = word - 0x10000
    end
    return word
end

local function has_bit(value, mask)
    if type(value) ~= "number" then
        return false
    end
    return (value & mask) ~= 0
end

function squads.sort_squad(unit, type_row, rules)
    local assignment = assignment_of(rules)
    local flags = 0
    local min_water = 0
    local max_water = 0
    if type(type_row) == "table" then
        flags = type_row.flags or 0
        min_water = type_row.min_water_depth or 0
        max_water = type_row.max_water_depth or 0
    end
    if assignment == "role-squads" then
        if unit.building then
            local spends = energy_high_word(type_row and type_row.energy_use or nil) > role_energy_high_word
            if has_bit(flags, flag_builder) or spends then
                return squad_structures
            end
            return squad_armed_structures
        end
        if has_bit(flags, flag_builder) then
            return squad_builders
        end
        if has_bit(flags, flag_can_fly) then
            return squad_aircraft
        end
        if min_water > 0 or max_water >= role_navy_depth or has_bit(flags, flag_amphibious) then
            return squad_navy
        end
        return squad_land_army
    end
    if unit.building then
        if unit.armed then
            return squad_armed_structures
        end
        return squad_structures
    end
    if has_bit(flags, flag_builder) then
        return squad_builders
    end
    if has_bit(flags, flag_can_fly) then
        return squad_aircraft
    end
    if min_water > 0 then
        return squad_navy
    end
    if unit.armed then
        return squad_land_army
    end
    return squad_none
end

function squads.sort(seat)
    local assignment = assignment_of(seat.rules)
    local manoeuvre_ability = ability_capture
    if assignment == "role-squads" then
        manoeuvre_ability = ability_commander
    end
    local list = seat.me:units()
    for index = 1, #list do
        local unit = list[index]
        if unit.selectable then
            local type_index = unit.type_index
            local type_row = nil
            if type(type_index) == "number" then
                type_row = seat.types[type_index]
            end
            if type_row ~= nil then
                local move = "roam"
                if has_bit(type_row.abilities, manoeuvre_ability) then
                    move = "manoeuvre"
                end
                unit:stance({move = move, fire = "at_will", direct = true})
                if unit.squad == 0 then
                    local chosen = squads.sort_squad(unit, type_row, assignment)
                    if chosen ~= squad_none then
                        seat.me:assign(unit, chosen)
                    end
                end
            end
        end
    end
end

return squads
