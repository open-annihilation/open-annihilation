-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Learned Scout walks the map. Every 30 ticks, each of this seat's units
-- that can move is considered.
-- A unit is free when head_order() returns nil, or when its third value
-- is true. That value is the idle bit: a finished move stays at the head
-- of the queue with the bit set.
-- A centre the unit cannot stand on is written as fully explored, and the
-- rule reads that. The rule level records the choice every time and sends
-- the unit when it is free. The learned level sends a free unit to the
-- sector nets/scout.oann picks.

local sectors = require("sectors")
local rule = require("rule")

local hooks = {}
local scout = nil
local teaching = false

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

-- True when the unit has no head order, or that order's idle bit is set.
local function is_free(unit)
    local kind, _, idle = unit:head_order()
    return kind == nil or idle == true
end

local function consider(unit, explored, width, height)
    local definition = unit.type
    if definition == nil or definition.canmove ~= true then
        return
    end
    local free = is_free(unit)
    if not teaching and not free then
        return
    end
    local look = sectors.look(unit, explored, width, height)
    if look == nil then
        return
    end
    local choice = nil
    if teaching then
        choice = rule.choose(look.explored_raw, look.squared)
    else
        local out = scout:run({sectors = look.tensor})
        choice = out.choice - 1
    end
    if choice == nil or choice < 0 or choice >= 64 then
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
    sectors.send(unit, choice, width, height)
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
