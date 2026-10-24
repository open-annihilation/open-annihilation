-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Learned Scout walks the map. Every 30 ticks, each of this seat's units
-- that can move is considered.
-- A sector counts only when the unit can stand on that centre.
-- The rule level records that choice every time. It sends the unit when
-- the unit is free: it has no order, or it is standing within its footprint
-- of the centre it was last sent to. A move order can remain after the unit
-- has stopped there.
-- The learned level asks nets/scout.oann which sector when the unit is
-- free, and follows the written rule when that centre is blocked.

local sectors = require("sectors")
local rule = require("rule")

-- A map cell is 16 world units. The footprint is counted in cells.
local CELL = 16

local hooks = {}
local scout = nil
local teaching = false
local sent = {}

function hooks.fits()
    return true
end

function hooks.start()
    teaching = me.level == "rule"
    if not teaching then
        scout = brain.network("scout")
    end
    return true
end

local function footprint_reach(unit)
    local definition = unit.type
    local wide = 1
    if definition ~= nil then
        if definition.footprintx ~= nil and definition.footprintx > wide then
            wide = definition.footprintx
        end
        if definition.footprintz ~= nil and definition.footprintz > wide then
            wide = definition.footprintz
        end
    end
    return wide * CELL
end

-- True when the unit is standing within its footprint of the last centre.
local function standing_there(unit)
    local slot = unit.slot
    if slot == nil or unit.x == nil or unit.z == nil then
        return false
    end
    local goal = sent[slot]
    if goal == nil then
        return false
    end
    local dx = unit.x:floor() - goal.x
    local dz = unit.z:floor() - goal.z
    local reach = footprint_reach(unit)
    return dx * dx + dz * dz <= reach * reach
end

local function consider(unit, explored, width, height)
    local definition = unit.type
    if definition == nil or definition.canmove ~= true then
        return
    end
    local free = unit:head_order() == nil or standing_there(unit)
    if not teaching and not free then
        return
    end
    local look = sectors.look(unit, explored, width, height)
    if look == nil then
        return
    end
    local choice = nil
    if teaching then
        choice = rule.choose(look.explored_raw, look.squared, look.stand)
    else
        local out = scout:run({sectors = look.tensor})
        choice = out.choice - 1
        if choice < 0 or choice >= 64 or not look.stand[choice + 1] then
            choice = rule.choose(look.explored_raw, look.squared, look.stand)
        end
    end
    if choice == nil then
        return
    end
    if teaching then
        brain.record({
            features = {sectors = look.tensor},
            decision = choice,
        })
    end
    if not free then
        return
    end
    if sectors.send(unit, choice, width, height) and unit.slot ~= nil then
        local cx, cz = sectors.centre(choice, width, height)
        sent[unit.slot] = {x = cx, z = cz}
    end
end

local function survey()
    local width = game.map.world_width
    local height = game.map.world_height
    if width == nil or height == nil or width <= 0 or height <= 0 then
        return
    end
    local explored = brain.grid({"explored"}, 8)
    if explored == nil then
        return
    end
    me:each_unit(function(unit)
        consider(unit, explored, width, height)
    end)
end

function hooks.onTick()
    if me.tick % 30 ~= 0 then
        return
    end
    survey()
end

return hooks
